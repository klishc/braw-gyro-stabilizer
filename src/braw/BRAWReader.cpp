// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// BRAWReader.cpp
// Blackmagic RAW SDK — Mac framework, verified against the installed
// BlackmagicRawAPI.h and the official ExtractMotionSamples sample.
//
// Architecture:
//   IBlackmagicRawFactory  → CreateCodec() → IBlackmagicRaw (codec)
//   IBlackmagicRaw         → OpenClip(CFStringRef) → IBlackmagicRawClip
//   IBlackmagicRawClip     → GetMetadata(CFStringRef key, Variant*)
//                          → QueryInterface(IID_IBlackmagicRawClipGyroscopeMotion, …)
//                          → QueryInterface(IID_IBlackmagicRawClipAccelerometerMotion, …)
//
// Variant struct (header lines 352-367) — the field is `vt`, NOT `variantType`,
// and the union members use COM-style names:
//   struct Variant {
//       BlackmagicRawVariantType vt;                 // uint32_t enum
//       union { int16_t iVal; uint16_t uiVal; int32_t intVal; uint32_t uintVal;
//               float fltVal; double dblVal; CFStringRef bstrVal; SafeArray* parray; };
//   };
// The enum (header lines 90-100) has ONLY: Empty, U8, S16, U16, S32, U32,
// Float32, String, SafeArray, Float64 — there is NO S64/U64 and NO ArrayFloat32.
// An array is variantType==SafeArray; the element type lives on SafeArray.variantType.
//
// SafeArray struct (header lines 342-348):
//   struct SafeArray { BlackmagicRawVariantType variantType; uint32_t cDims;
//                      uint8_t* data; SafeArrayBound bounds; };
//   struct SafeArrayBound { uint32_t lLbound; uint32_t cElements; };
// i.e. element count is `bounds.cElements`, payload pointer is `data` — there is
// no `cElements`/`pElements` directly on SafeArray and no `safeArray` Variant member.
//
// NOTE: gyro/accel samples do NOT come through metadata keys. The SDK exposes them
// via dedicated interfaces (GetSampleRange) as 3-float [x,y,z] samples — there is no
// per-sample timestamp; timestamps are derived from the sample rate.

#include "BRAWReader.h"
#include "../common/DebugLog.h"
#include <BlackmagicRawAPI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>

// ── Process-global focal cache (keyed by clip path) ───────────────────────────
// Premiere opens the same .braw through several BRAWReaders (the CPU EffectMain path
// AND the GPU filter's cached reader). Each reader's per-instance async focal read can
// get starved/stuck contending on the BRAW codec, so the value may never land in that
// reader. Sharing the first successful read across all readers of the same path makes
// auto-focal reliable: whoever decodes frame 0 first publishes it here for everyone.
static std::mutex                    sFocalMutex;
static std::map<std::string, float>  sFocalCache;
// Full per-frame focal track + lens, shared across readers of the same clip this session
// (the CPU EffectMain path and the GPU filter each open the clip). sScanMutex serializes
// the actual BRAW decode-scan so two readers don't contend on the decoder and stall.
static std::map<std::string, std::vector<std::pair<double,float>>> sTrackCache;
static std::map<std::string, std::string> sLensCache;
static std::mutex                    sScanMutex;

// ── CFString helpers ──────────────────────────────────────────────────────────
static std::string CFStr(CFStringRef s)
{
    if (!s) return {};
    char buf[512] = {};
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
    return buf;
}
static CFStringRef MakeCF(const char* s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static void DumpAllMetadata(IBlackmagicRawClip* clip);   // defined below
static void VarToStr(const Variant& v, char* buf, size_t n);   // defined below

// ── Variant → scalar float ────────────────────────────────────────────────────
static float VarFloat(const Variant& v)
{
    switch (v.vt) {
        case blackmagicRawVariantTypeFloat32: return v.fltVal;
        case blackmagicRawVariantTypeFloat64: return (float)v.dblVal;
        case blackmagicRawVariantTypeS16:     return (float)v.iVal;
        case blackmagicRawVariantTypeU8:                      // no dedicated 8-bit member
        case blackmagicRawVariantTypeU16:     return (float)v.uiVal;
        case blackmagicRawVariantTypeS32:     return (float)v.intVal;
        case blackmagicRawVariantTypeU32:     return (float)v.uintVal;
        default: return 0.f;
    }
}

// ── Variant(SafeArray of Float32) → float array ───────────────────────────────
// An array Variant has vt == blackmagicRawVariantTypeSafeArray and carries a
// SafeArray* in `parray`. The element type is on SafeArray::variantType, the
// element count is SafeArray::bounds.cElements, and the payload is SafeArray::data.
static std::vector<float> VarFloatArray(const Variant& v)
{
    std::vector<float> out;
    if (v.vt != blackmagicRawVariantTypeSafeArray) return out;
    SafeArray* sa = v.parray;
    if (!sa || sa->variantType != blackmagicRawVariantTypeFloat32 || !sa->data) return out;
    uint32_t count = sa->bounds.cElements;
    const float* data = reinterpret_cast<const float*>(sa->data);
    out.assign(data, data + count);
    return out;
}

// ── Async callback to read the first frame's metadata (per-frame focal length) ─
// On a zoom lens the focal length lives in the FRAME metadata, not the clip's. We
// read frame 0 synchronously (Submit + FlushJobs) and pull any "focal*" key out of
// its metadata iterator. Lifetime is the caller's stack, so refcounting is a no-op.
class FocalReadCallback : public IBlackmagicRawCallback
{
public:
    float       focalMM = 0.f;
    std::string lensName;
    bool        logDump = false;        // only dump the full key list once

    // Signaling so the scan can wait with a timeout instead of a blocking FlushJobs
    // (a stalled decode must not hang the scan). This object is intentionally leaked
    // (its refcount is a no-op) so a late callback after a timeout is always safe.
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    done = false;

    void signalDone() { { std::lock_guard<std::mutex> lk(mtx); done = true; } cv.notify_one(); }

    void ReadComplete(IBlackmagicRawJob*, HRESULT result, IBlackmagicRawFrame* frame) override
    {
        focalMM = 0.f;                  // reset per frame (callback is reused across the scan)
        if (FAILED(result) || !frame) { signalDone(); return; }
        IBlackmagicRawMetadataIterator* it = nullptr;
        if (FAILED(frame->GetMetadataIterator(&it)) || !it) { signalDone(); return; }

        FILE* f = logDump ? dbg::open() : nullptr;
        if (f) std::fprintf(f, "=== FRAME METADATA ===\n");
        for (;;) {
            CFStringRef key = nullptr;
            Variant val; memset(&val, 0, sizeof(val));
            it->GetKey(&key);
            it->GetData(&val);
            std::string ks = CFStr(key);
            char vbuf[300]; VarToStr(val, vbuf, sizeof(vbuf));
            if (f) std::fprintf(f, "  %s = %s\n", ks.c_str(), vbuf);

            std::string low = ks;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            if (low == "focal_length") {
                // Stored as a string like "48mm" on a zoom lens — parse the number.
                float fv = (val.vt == blackmagicRawVariantTypeString)
                               ? std::strtof(CFStr(val.bstrVal).c_str(), nullptr)
                               : VarFloat(val);
                if (fv > 0.f) focalMM = fv;
            }
            // Lens model/type, captured once (best effort; key name varies by camera).
            if (lensName.empty() && val.vt == blackmagicRawVariantTypeString &&
                (low == "lens_type" || low == "lens_model" || low.find("lens") != std::string::npos)) {
                std::string lv = CFStr(val.bstrVal);
                if (!lv.empty()) lensName = lv;
            }
            if (it->Next() != S_OK) break;
        }
        if (f) { std::fprintf(f, "=== END FRAME METADATA (focal=%.2f lens='%s') ===\n",
                              focalMM, lensName.c_str()); std::fclose(f); }
        it->Release();
        signalDone();
    }

    void DecodeComplete(IBlackmagicRawJob*, HRESULT) override {}
    void ProcessComplete(IBlackmagicRawJob*, HRESULT, IBlackmagicRawProcessedImage*) override {}
    void TrimProgress(IBlackmagicRawJob*, float) override {}
    void TrimComplete(IBlackmagicRawJob*, HRESULT) override {}
    void SidecarMetadataParseWarning(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) override {}
    void SidecarMetadataParseError(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) override {}
    void PreparePipelineComplete(void*, HRESULT) override {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOTIMPL; }
    ULONG   STDMETHODCALLTYPE AddRef(void)  override { return 0; }
    ULONG   STDMETHODCALLTYPE Release(void) override { return 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
BRAWReader::BRAWReader()  = default;
BRAWReader::~BRAWReader() { Close(); }

bool BRAWReader::Open(const std::string& filePath)
{
    mPath = filePath;

    // Factory → Codec → Clip
    mFactory = CreateBlackmagicRawFactoryInstance();
    if (!mFactory) return false;

    HRESULT hr = mFactory->CreateCodec(&mCodec);
    if (FAILED(hr) || !mCodec) return false;

    // Decode on the CPU pipeline. Our codec is used only for the focal-length frame scan;
    // routing it to the CPU keeps it off Premiere's GPU (Metal) decoder, which was starving
    // the scan during playback (it hung in FlushJobs and the focal callback never fired).
    {
        IBlackmagicRawConfiguration* config = nullptr;
        if (SUCCEEDED(mCodec->QueryInterface(IID_IBlackmagicRawConfiguration, (void**)&config)) && config) {
            config->SetPipeline(blackmagicRawPipelineCPU, nullptr, nullptr);
            config->Release();
        }
    }

    CFStringRef cfPath = MakeCF(filePath.c_str());
    hr = mCodec->OpenClip(cfPath, &mClip);
    CFRelease(cfPath);
    if (FAILED(hr) || !mClip) return false;

    // Basic attributes
    float fps = 24.f;
    mClip->GetFrameRate(&fps);
    mFrameRate = fps;

    uint64_t fc = 0;
    mClip->GetFrameCount(&fc);
    mFrameCount = (int)fc;

    uint32_t w = 0, h = 0;
    mClip->GetWidth(&w);
    mClip->GetHeight(&h);
    mWidth  = (int)w;
    mHeight = (int)h;

    ExtractGyroStream();
    ExtractClipMetadata();
    ExtractRollingShutter();
    ScanFocalTrack();         // per-frame focal length across the clip (zoom lens), cached
    return true;
}

void BRAWReader::Close()
{
    // Join the focal-read thread before tearing down the codec/clip it uses.
    if (mFocalThread.joinable()) mFocalThread.join();
    if (mClip)    { mClip->Release();    mClip    = nullptr; }
    if (mCodec)   { mCodec->Release();   mCodec   = nullptr; }
    if (mFactory) { mFactory->Release(); mFactory = nullptr; }
}

// ── Query a single metadata key directly ─────────────────────────────────────
static bool GetMeta(IBlackmagicRawClip* clip, const char* key, Variant& out)
{
    memset(&out, 0, sizeof(out));
    CFStringRef cfKey = MakeCF(key);
    HRESULT hr = clip->GetMetadata(cfKey, &out);
    CFRelease(cfKey);
    return SUCCEEDED(hr);
}

// ── One-time diagnostic: dump every clip-level metadata key/value ─────────────
// BRAW does not publish its metadata key strings in the header; this reveals the
// actual keys (focal length, crop, etc.) present in a given clip.
static void VarToStr(const Variant& v, char* buf, size_t n)
{
    switch (v.vt) {
        case blackmagicRawVariantTypeString:  std::snprintf(buf,n,"str:'%s'", CFStr(v.bstrVal).c_str()); break;
        case blackmagicRawVariantTypeFloat32: std::snprintf(buf,n,"f32:%g",  v.fltVal); break;
        case blackmagicRawVariantTypeFloat64: std::snprintf(buf,n,"f64:%g",  v.dblVal); break;
        case blackmagicRawVariantTypeS16:     std::snprintf(buf,n,"s16:%d",  v.iVal); break;
        case blackmagicRawVariantTypeU16:     std::snprintf(buf,n,"u16:%u",  v.uiVal); break;
        case blackmagicRawVariantTypeS32:     std::snprintf(buf,n,"s32:%d",  v.intVal); break;
        case blackmagicRawVariantTypeU32:     std::snprintf(buf,n,"u32:%u",  v.uintVal); break;
        case blackmagicRawVariantTypeU8:      std::snprintf(buf,n,"u8:%u",   v.uiVal); break;
        case blackmagicRawVariantTypeSafeArray:
            std::snprintf(buf,n,"array[%u]", v.parray ? v.parray->bounds.cElements : 0); break;
        default: std::snprintf(buf,n,"vt=%u", (unsigned)v.vt); break;
    }
}

static void DumpAllMetadata(IBlackmagicRawClip* clip)
{
    FILE* f = dbg::open();
    if (!f) return;
    std::fprintf(f, "=== CLIP METADATA DUMP ===\n");
    IBlackmagicRawMetadataIterator* it = nullptr;
    if (SUCCEEDED(clip->GetMetadataIterator(&it)) && it) {
        for (;;) {
            CFStringRef key = nullptr;
            Variant val; memset(&val, 0, sizeof(val));
            it->GetKey(&key);
            it->GetData(&val);
            char vbuf[300];
            VarToStr(val, vbuf, sizeof(vbuf));
            std::fprintf(f, "  %s = %s\n", CFStr(key).c_str(), vbuf);
            // Borrowed refs from the iterator — do not release (one-time dump).
            if (it->Next() != S_OK) break;
        }
        it->Release();
    } else {
        std::fprintf(f, "  (no metadata iterator)\n");
    }
    std::fprintf(f, "=== END METADATA DUMP ===\n");
    std::fclose(f);
}

// ── Gyro + accelerometer stream ───────────────────────────────────────────────
// Read via the dedicated motion interfaces (the same data Resolve uses), obtained
// by QueryInterface on the clip. Both streams share one sample rate; each sample is
// 3 floats [x,y,z]. Per the official sample, sample size must be 3 floats and the
// gyro/accel rates must match. Timestamps are derived as sampleIndex / sampleRate.
void BRAWReader::ExtractGyroStream()
{
    IBlackmagicRawClipGyroscopeMotion*     gyro  = nullptr;
    IBlackmagicRawClipAccelerometerMotion* accel = nullptr;
    mClip->QueryInterface(IID_IBlackmagicRawClipGyroscopeMotion,     (void**)&gyro);
    mClip->QueryInterface(IID_IBlackmagicRawClipAccelerometerMotion, (void**)&accel);
    if (!gyro) { if (accel) accel->Release(); return; }

    float    rate = 0.f;
    uint32_t count = 0, sizeInFloats = 0;
    if (FAILED(gyro->GetSampleRate(&rate))   || rate <= 0.f ||
        FAILED(gyro->GetSampleCount(&count)) || count == 0  ||
        FAILED(gyro->GetSampleSize(&sizeInFloats)) || sizeInFloats != 3) {
        gyro->Release(); if (accel) accel->Release(); return;
    }

    // If the accelerometer stream is present, it must align with the gyro stream.
    bool haveAccel = false;
    if (accel) {
        float    aRate = 0.f;
        uint32_t aCount = 0, aSize = 0;
        haveAccel = SUCCEEDED(accel->GetSampleRate(&aRate))   && aRate == rate &&
                    SUCCEEDED(accel->GetSampleCount(&aCount)) &&
                    SUCCEEDED(accel->GetSampleSize(&aSize))   && aSize == 3;
        if (haveAccel) count = std::min(count, aCount);
    }

    mGyroSamples.reserve(count);

    const uint32_t kChunk = 2000;                 // samples per GetSampleRange call
    std::vector<float> gBuf(kChunk * 3), aBuf(kChunk * 3);
    uint64_t base = 0;
    uint32_t remaining = count;
    while (remaining > 0) {
        const uint32_t want = std::min(kChunk, remaining);

        uint32_t gRead = 0;
        if (FAILED(gyro->GetSampleRange(base, want, gBuf.data(), &gRead)) || gRead == 0)
            break;

        uint32_t aRead = 0;
        if (haveAccel &&
            FAILED(accel->GetSampleRange(base, want, aBuf.data(), &aRead)))
            aRead = 0;

        for (uint32_t i = 0; i < gRead; ++i) {
            GyroSample s;
            s.timestampSec = double(base + i) / double(rate);
            s.gx = gBuf[i*3+0]; s.gy = gBuf[i*3+1]; s.gz = gBuf[i*3+2];
            if (i < aRead) { s.ax = aBuf[i*3+0]; s.ay = aBuf[i*3+1]; s.az = aBuf[i*3+2]; }
            else           { s.ax = s.ay = s.az = 0.f; }
            mGyroSamples.push_back(s);
        }

        base      += gRead;
        remaining -= gRead;
    }

    gyro->Release();
    if (accel) accel->Release();
}

// ── Clip-level metadata (keys verified by enumerating GetMetadataIterator) ────
// NOTE: there is no clip-level focal_length key — on a zoom lens it is per-frame.
// We DO get the photosite pitch, which lets the projection compute focal-in-pixels
// directly (fx = focal_mm * 1000 / pitch_µm) without guessing the sensor size.
void BRAWReader::ExtractClipMetadata()
{
    Variant v;
    // Photosite pitch in micrometres (e.g. 5.94 for the Cinema Camera 6K).
    mPhotositePitchUm = 0.f;
    if (GetMeta(mClip, "sensor_photosite_pitch_in_micrometres", v))
        mPhotositePitchUm = VarFloat(v);

    // Camera model (best effort; key name is "camera_type" on Blackmagic cameras,
    // e.g. "Pocket Cinema Camera 6K Pro"). Fall back to a couple of alternates.
    mCameraName.clear();
    for (const char* key : { "camera_type", "camera_model", "camera_id" }) {
        if (GetMeta(mClip, key, v) && v.vt == blackmagicRawVariantTypeString) {
            std::string s = CFStr(v.bstrVal);
            if (!s.empty()) { mCameraName = s; break; }
        }
    }

    // Focal length isn't in clip metadata for a zoom lens; the plugin supplies it
    // via the "Focal Length (mm)" parameter. Keep a sane fallback here.
    mFocalLengthMM    = 35.f;
    mSensorCropFactor = 1.f;
}

// ── Rolling shutter ───────────────────────────────────────────────────────────
// `sensor_line_time` is the per-scanline readout time in MICROSECONDS. The full
// frame readout (what the per-row warp needs) is line_time × number of lines.
void BRAWReader::ExtractRollingShutter()
{
    Variant v;
    float lineTimeUs = 0.f;
    if (GetMeta(mClip, "sensor_line_time", v)) lineTimeUs = VarFloat(v);
    mRollingShutterMs = lineTimeUs > 0.f ? (lineTimeUs * mHeight) / 1000.f : 16.f;
}

// ── Per-frame focal track (scanned on a BACKGROUND thread, cached to a sidecar) ─
// A zoom lens changes focal length mid-shot, so the stabilization intrinsics (fx) must
// track it. We sample focal across the clip (~4/sec; zoom is smooth, no need for every
// frame), smooth out the integer-mm quantization, and interpolate per frame. The scan
// decodes frames via FlushJobs (deadlocks on Premiere's render thread), so it runs off-
// thread; rendering uses the fallback focal until the track is published. Results are
// cached to "<path>.gyrostab" so later opens skip the expensive BRAW scan entirely.
void BRAWReader::ScanFocalTrack()
{
    if (!mCodec || !mClip) return;
    mFocalThread = std::thread([this]() {
        // Adopt a track already built this session by another reader of the same clip.
        auto adoptCached = [this]() -> bool {
            std::lock_guard<std::mutex> lk(sFocalMutex);
            auto it = sTrackCache.find(mPath);
            if (it == sTrackCache.end() || it->second.empty()) return false;
            mFocalTrack = it->second;
            auto lit = sLensCache.find(mPath);
            if (lit != sLensCache.end()) mLensName = lit->second;
            mFocalLengthMM.store(mFocalTrack.front().second);
            mFocalReady.store(true);
            return true;
        };
        auto publish = [this]() {
            std::lock_guard<std::mutex> lk(sFocalMutex);
            sFocalCache[mPath] = mFocalTrack.front().second;
            sTrackCache[mPath] = mFocalTrack;
            if (!mLensName.empty()) sLensCache[mPath] = mLensName;
        };

        if (adoptCached()) return;           // another reader already scanned it
        if (LoadSidecar()) {                 // disk cache: no BRAW decode needed
            if (!mFocalTrack.empty()) mFocalLengthMM.store(mFocalTrack.front().second);
            publish();
            mFocalReady.store(true);
            return;
        }

        // Serialize the actual decode-scan; re-check the cache after acquiring the lock in
        // case another reader finished while we waited.
        std::lock_guard<std::mutex> scanLk(sScanMutex);
        if (adoptCached()) return;

        // Heap-allocated + intentionally leaked: a decode that times out may still deliver
        // a late callback; keeping the object alive forever makes that always safe.
        FocalReadCallback* cb = new FocalReadCallback();
        cb->logDump = true;                        // dump the first frame's keys once
        if (FAILED(mCodec->SetCallback(cb))) return;

        const int frames = mFrameCount > 0 ? mFrameCount : 1;
        const int step   = 1;   // per-frame focal — densest signal for zoom-motion smoothing
                                 // (first scan is slower but cached to the .gyrostab sidecar)
        { FILE* f = dbg::open();
          if (f) { std::fprintf(f, "FOCAL SCAN start: frames=%d step=%d (CPU decode)\n", frames, step); std::fclose(f); } }
        std::vector<std::pair<double,float>> track;
        int consecTimeouts = 0;
        for (int idx = 0; idx < frames; idx += step) {
            IBlackmagicRawJob* job = nullptr;
            if (SUCCEEDED(mClip->CreateJobReadFrame(idx, &job)) && job) {
                // Decode at 1/8 res: focal is camera metadata (resolution-independent), ~64× cheaper.
                IBlackmagicRawReadJobHints* hints = nullptr;
                if (SUCCEEDED(job->QueryInterface(IID_IBlackmagicRawReadJobHints, (void**)&hints)) && hints) {
                    hints->SetReaderResolutionScale(blackmagicRawResolutionScaleEighth);
                    hints->Release();
                }
                { std::lock_guard<std::mutex> lk(cb->mtx); cb->done = false; cb->focalMM = 0.f; }
                bool decoded = false;
                if (SUCCEEDED(job->Submit())) {
                    // Wait for the callback instead of a blocking FlushJobs, so a stalled
                    // decode can't hang the scan — time out and move on.
                    std::unique_lock<std::mutex> lk(cb->mtx);
                    decoded = cb->cv.wait_for(lk, std::chrono::milliseconds(2000),
                                              [&]{ return cb->done; });
                }
                if (!decoded) job->Abort();
                job->Release();
                consecTimeouts = decoded ? 0 : (consecTimeouts + 1);
            }
            cb->logDump = false;
            float fm = 0.f; std::string lens;
            { std::lock_guard<std::mutex> lk(cb->mtx); fm = cb->focalMM; lens = cb->lensName; }
            if (!lens.empty() && mLensName.empty()) mLensName = lens;
            if (fm > 0.f) {
                if (track.empty()) mFocalLengthMM.store(fm);   // use the real focal ASAP
                track.emplace_back((double)idx / (double)mFrameRate, fm);
                if ((track.size() % 10) == 0) {
                    FILE* f = dbg::open();
                    if (f) { std::fprintf(f, "FOCAL SCAN progress: %zu samples (idx %d/%d)\n",
                                          track.size(), idx, frames); std::fclose(f); }
                }
            }
            if (consecTimeouts >= 5) {            // decoder is wedged — stop, use what we got
                FILE* f = dbg::open();
                if (f) { std::fprintf(f, "FOCAL SCAN: %d consecutive timeouts at idx %d — stopping\n",
                                      consecTimeouts, idx); std::fclose(f); }
                break;
            }
        }
        mCodec->SetCallback(nullptr);            // cb itself is leaked on purpose (see above)

        if (track.empty()) {                 // scan failed — leave fallback in place
            FILE* f = dbg::open();
            if (f) { std::fprintf(f, "FOCAL SCAN: no samples (using fallback %.1f)\n",
                                  mFocalLengthMM.load()); std::fclose(f); }
            return;
        }

        mFocalTrack = std::move(track);
        SmoothFocalTrack();
        mFocalLengthMM.store(mFocalTrack.front().second);
        publish();
        mFocalReady.store(true);
        WriteSidecar();

        FILE* f = dbg::open();
        if (f) {
            float lo = 1e9f, hi = -1e9f;
            for (auto& p : mFocalTrack) { lo = std::min(lo, p.second); hi = std::max(hi, p.second); }
            std::fprintf(f, "FOCAL SCAN done: %zu samples, focal %.1f..%.1f mm, lens='%s'\n",
                         mFocalTrack.size(), lo, hi, mLensName.c_str());
            std::fclose(f);
        }
    });
}

// Light moving-average smoothing to remove the integer-mm stepping (e.g. 48→49→50)
// that would otherwise make the correction magnitude pulse during a slow zoom.
void BRAWReader::SmoothFocalTrack()
{
    const size_t N = mFocalTrack.size();
    if (N < 5) return;                       // too few samples (likely a zoom) — keep raw
    std::vector<float> out(N);
    out[0]   = mFocalTrack[0].second;        // preserve endpoints: a zoom that starts/ends at
    out[N-1] = mFocalTrack[N-1].second;      // an extreme must not be averaged toward the middle
    for (size_t i = 1; i + 1 < N; ++i)       // ±1 sample only: kills integer-mm jitter, keeps zooms
        out[i] = (mFocalTrack[i-1].second + mFocalTrack[i].second + mFocalTrack[i+1].second) / 3.f;
    for (size_t i = 0; i < N; ++i) mFocalTrack[i].second = out[i];
}

// ── Sidecar cache ("<path>.gyrostab") ─────────────────────────────────────────
bool BRAWReader::LoadSidecar()
{
    std::string scPath = mPath + ".gyrostab";
    FILE* f = std::fopen(scPath.c_str(), "r");
    if (!f) return false;
    int frames = 0; double fps = 0; int n = 0; char lens[256] = {};
    bool ok = false;
    if (std::fscanf(f, "GYROSTAB2 frames=%d fps=%lf n=%d\n", &frames, &fps, &n) == 3 &&
        frames == mFrameCount && n > 0 && n < 1000000) {
        long pos = std::ftell(f);
        if (std::fscanf(f, "lens=%255[^\n]\n", lens) == 1) mLensName = lens;
        else std::fseek(f, pos, SEEK_SET);
        std::vector<std::pair<double,float>> track; track.reserve(n);
        for (int i = 0; i < n; ++i) {
            double t; float fo;
            if (std::fscanf(f, "%lf %f\n", &t, &fo) != 2) { track.clear(); break; }
            track.emplace_back(t, fo);
        }
        if ((int)track.size() == n) { mFocalTrack = std::move(track); ok = true; }
    }
    std::fclose(f);
    if (ok) {
        FILE* g = dbg::open();
        if (g) { std::fprintf(g, "FOCAL sidecar HIT: %zu samples '%s'\n",
                              mFocalTrack.size(), scPath.c_str()); std::fclose(g); }
    }
    return ok;
}

void BRAWReader::WriteSidecar() const
{
    std::string scPath = mPath + ".gyrostab";
    FILE* f = std::fopen(scPath.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "GYROSTAB2 frames=%d fps=%.6f n=%zu\n",
                 mFrameCount, (double)mFrameRate, mFocalTrack.size());
    if (!mLensName.empty()) std::fprintf(f, "lens=%s\n", mLensName.c_str());
    for (auto& p : mFocalTrack) std::fprintf(f, "%.4f %.2f\n", p.first, p.second);
    std::fclose(f);
}

float BRAWReader::GetMaxFocalLength() const
{
    if (mFocalReady.load() && !mFocalTrack.empty()) {
        float m = 0.f;
        for (const auto& p : mFocalTrack) m = std::max(m, p.second);
        if (m > 0.f) return m;
    }
    return GetFocalLength(0.0);
}


float BRAWReader::GetFocalLength(double timeSec) const
{
    // Per-frame focal from the scanned track (handles zoom lenses).
    if (mFocalReady.load() && !mFocalTrack.empty()) {
        const auto& T = mFocalTrack;
        if (timeSec <= T.front().first) return T.front().second;
        if (timeSec >= T.back().first)  return T.back().second;
        size_t lo = 0, hi = T.size() - 1;     // binary search the bracket
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (T[mid].first <= timeSec) lo = mid; else hi = mid;
        }
        double span = T[hi].first - T[lo].first;
        float  a    = (span > 1e-9) ? (float)((timeSec - T[lo].first) / span) : 0.f;
        return T[lo].second + a * (T[hi].second - T[lo].second);
    }
    // Track not ready yet — fall back to any single-value read for this clip.
    float result = mFocalLengthMM.load();
    {
        std::lock_guard<std::mutex> lk(sFocalMutex);
        auto it = sFocalCache.find(mPath);
        if (it != sFocalCache.end() && it->second > 0.f) result = it->second;
    }
    return result;
}


float BRAWReader::GetFocalLengthSmoothed(double timeSec, double windowSec) const
{
    // Gaussian-weighted average of the focal track over [t-window, t+window]. It's SYMMETRIC
    // (uses future samples too), so there's no lag — the sharp velocity corner where a zoom
    // starts or stops gets rounded on both sides, and the digital scale eases through it.
    // The focal track is fully known ahead of time, so future sampling is free.
    if (windowSec <= 1e-3 || !(mFocalReady.load() && mFocalTrack.size() > 2))
        return GetFocalLength(timeSec);
    const int    half  = 12;                 // samples per side
    const double sigma = windowSec * 0.5;    // ±window ≈ ±2σ
    double num = 0.0, den = 0.0;
    for (int i = -half; i <= half; ++i) {
        const double dt = (windowSec / half) * i;
        const double w  = std::exp(-(dt * dt) / (2.0 * sigma * sigma));
        num += w * GetFocalLength(timeSec + dt);
        den += w;
    }
    return (den > 1e-9) ? static_cast<float>(num / den) : GetFocalLength(timeSec);
}


float BRAWReader::GetZoomEaseScale(double timeSec, double windowSec, float blend) const
{
    // Make the APPARENT focal follow appFocal = lerp(actual, smoothed, blend), by returning
    // the digital magnification appFocal/actual. Normalize by the clip-wide MIN of that ratio
    // so the result is always >= 1 (the widest-needed frame sits at the base crop and never
    // shows past the frame; every other frame just zooms in a touch). The overall constant
    // 1/minRatio is the ease "headroom" — the price of decelerating the hard stops.
    if (blend <= 1e-3f || windowSec <= 1e-3 || !(mFocalReady.load() && mFocalTrack.size() > 2))
        return 1.0f;

    {   // clip-wide min appFocal/actual, cached per (blend,window)
        std::lock_guard<std::mutex> lk(mZoomEaseMutex);
        if (!mZoomEaseValid || mZoomEaseBlend != blend || mZoomEaseWindow != (float)windowSec) {
            float mn = 1e9f;
            for (const auto& p : mFocalTrack) {
                const float act = p.second;
                if (act < 1e-3f) continue;
                const float sm  = GetFocalLengthSmoothed(p.first, windowSec);
                const float app = act + blend * (sm - act);
                mn = std::min(mn, app / act);
            }
            mZoomEaseMinRatio = (mn > 1e-3f && mn < 1e8f) ? mn : 1.0f;
            mZoomEaseBlend  = blend;
            mZoomEaseWindow = (float)windowSec;
            mZoomEaseValid  = true;
        }
    }

    const float act = GetFocalLength(timeSec);
    if (act < 1e-3f) return 1.0f;
    const float sm    = GetFocalLengthSmoothed(timeSec, windowSec);
    const float app   = act + blend * (sm - act);
    const float scale = (app / act) / mZoomEaseMinRatio;   // >= 1 by construction
    return std::max(1.0f, scale);
}
