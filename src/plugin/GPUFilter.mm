// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// GPUFilter.mm
// Premiere GPU (SmartFX) video-filter path for the BRAW Gyro Stabilizer.
//
// This is a SECOND entry point (xGPUFilterEntry, via DECLARE_GPUFILTER_ENTRY) that
// lives in the same bundle as the AE-compat EffectMain and is linked to it by match
// name. When Premiere renders on the GPU it calls this instead of the CPU path, so
// frames stay GPU-resident (no per-frame CPU↔GPU readback). The Metal warp kernel
// reads the input frame buffer and writes the output buffer directly.
//
// The gyro data is loaded here independently: the filter has its own node id, from
// which we resolve the .braw media path and build a GyroIntegrator.

#include "PluginMain.h"                 // kParam* indices
#include "../braw/BRAWReader.h"
#include "../common/StatusStore.h"
#include "../common/BrawPath.h"
#include "../gyro/GyroIntegrator.h"
#include "../common/DebugLog.h"

#include "PrGPUFilterModule.h"
#include "PrSDKVideoSegmentProperties.h"
#include "PrSDKTimeSuite.h"

#import <Metal/Metal.h>
#include <memory>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <map>
#include <mutex>
#include <vector>

static void GLog(const char* fmt, ...) {
    FILE* f = dbg::open(); if (!f) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fputc('\n', f); std::fclose(f);
}

// ── Warp kernel (buffer based, runtime compiled — avoids the metal toolchain) ──
static const char* kGyroWarpKernelSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms { uint width; uint height; uint pitch; uint is16f; };

static float4 readPix(device const float* f, uint idx, bool is16f) {
    uint o = idx * 4u;
    if (is16f) { device const half* h = (device const half*)f; return float4(h[o], h[o+1], h[o+2], h[o+3]); }
    return float4(f[o], f[o+1], f[o+2], f[o+3]);
}
static void writePix(device float* f, uint idx, float4 c, bool is16f) {
    uint o = idx * 4u;
    if (is16f) { device half* h = (device half*)f; h[o]=half(c.x); h[o+1]=half(c.y); h[o+2]=half(c.z); h[o+3]=half(c.w); return; }
    f[o]=c.x; f[o+1]=c.y; f[o+2]=c.z; f[o+3]=c.w;
}

kernel void gyro_warp(
    device const float*       inBuf  [[buffer(0)]],
    device       float*       outBuf [[buffer(1)]],
    device const float*       homo   [[buffer(2)]],
    constant     Uniforms&    u      [[buffer(3)]],
    uint2                     gid    [[thread_position_in_grid]])
{
    if (gid.x >= u.width || gid.y >= u.height) return;
    bool is16f = (u.is16f != 0u);

    uint b = gid.y * 9u;
    float h00=homo[b+0], h01=homo[b+1], h02=homo[b+2];
    float h10=homo[b+3], h11=homo[b+4], h12=homo[b+5];
    float h20=homo[b+6], h21=homo[b+7], h22=homo[b+8];

    float ox = float(gid.x) + 0.5f, oy = float(gid.y) + 0.5f;
    float sw = h20*ox + h21*oy + h22;
    float srcX = (h00*ox + h01*oy + h02) / sw;   // source pixel coords
    float srcY = (h10*ox + h11*oy + h12) / sw;

    float4 color;
    if (srcX < 0.0f || srcX > float(u.width) || srcY < 0.0f || srcY > float(u.height)) {
        color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        float px = srcX - 0.5f, py = srcY - 0.5f;
        int x0 = clamp(int(floor(px)),   0, int(u.width)-1);
        int y0 = clamp(int(floor(py)),   0, int(u.height)-1);
        int x1 = clamp(x0+1,             0, int(u.width)-1);
        int y1 = clamp(y0+1,             0, int(u.height)-1);
        float fx = px - floor(px), fy = py - floor(py);
        float4 c00 = readPix(inBuf, uint(y0)*u.pitch + uint(x0), is16f);
        float4 c10 = readPix(inBuf, uint(y0)*u.pitch + uint(x1), is16f);
        float4 c01 = readPix(inBuf, uint(y1)*u.pitch + uint(x0), is16f);
        float4 c11 = readPix(inBuf, uint(y1)*u.pitch + uint(x1), is16f);
        color = mix(mix(c00,c10,fx), mix(c01,c11,fx), fy);
    }
    writePix(outBuf, gid.y*u.pitch + gid.x, color, is16f);
}
)METAL";

struct WarpUniforms { uint32_t width, height, pitch, is16f; };

enum { kMaxDevices = 12 };
static id<MTLComputePipelineState> sPipelineCache[kMaxDevices] = {};

static const PrTime kFallbackTicksPerSecond = 254016000000LL;


// ── Walk a node's inputs/operators to find a Media node's instance string ─────
// PREFERS a .braw path: when a clip has a proxy, the graph may expose a proxy media path
// too, so we keep searching past any non-.braw media node and remember it only as a
// fallback (returned by the caller if no .braw is found anywhere).
static std::string FindMediaPath(PrSDKVideoSegmentSuite* vs, PrSDKMemoryManagerSuite* mem,
                                 csSDK_int32 nodeID, int depth, std::string& fallback)
{
    if (!vs || depth > 16) return {};
    char nodeType[256] = {};
    prPluginID hash; csSDK_int32 flags = 0;
    if (vs->GetNodeInfo(nodeID, nodeType, &hash, &flags) != suiteError_NoError) return {};

    if (std::strstr(nodeType, "Media")) {
        PrMemoryPtr val = nullptr;
        if (vs->GetNodeProperty(nodeID, kVideoSegmentProperty_Media_InstanceString, &val)
                == suiteError_NoError && val) {
            std::string s(val);
            if (mem && mem->PrDisposePtr) mem->PrDisposePtr(val);
            if (!s.empty()) {
                if (LooksLikeBraw(s)) return s;               // the original .braw — take it
                if (fallback.empty())  fallback = s;          // e.g. a proxy path — remember only
            }
        }
    }
    csSDK_int32 opCount = 0;
    if (vs->GetNodeOperatorCount(nodeID, &opCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < opCount; ++i) {
            csSDK_int32 c = 0;
            if (vs->AcquireOperatorNodeID(nodeID, i, &c) == suiteError_NoError) {
                std::string r = FindMediaPath(vs, mem, c, depth+1, fallback);
                vs->ReleaseVideoNodeID(c);
                if (!r.empty()) return r;
            }
        }
    csSDK_int32 inCount = 0;
    if (vs->GetNodeInputCount(nodeID, &inCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < inCount; ++i) {
            csSDK_int32 c = 0; PrTime off = 0;
            if (vs->AcquireInputNodeID(nodeID, i, &off, &c) == suiteError_NoError) {
                std::string r = FindMediaPath(vs, mem, c, depth+1, fallback);
                vs->ReleaseVideoNodeID(c);
                if (!r.empty()) return r;
            }
        }
    return {};
}

// Convenience wrapper: prefer a .braw path, else fall back to any media path found.
static std::string FindMediaPath(PrSDKVideoSegmentSuite* vs, PrSDKMemoryManagerSuite* mem,
                                 csSDK_int32 nodeID, int depth)
{
    std::string fallback;
    std::string p = FindMediaPath(vs, mem, nodeID, depth, fallback);
    return p.empty() ? fallback : p;
}

// Collect EVERY .braw media path under a node (a stacked-track composite exposes several).
static void CollectBrawPaths(PrSDKVideoSegmentSuite* vs, PrSDKMemoryManagerSuite* mem,
                             csSDK_int32 nodeID, int depth, std::vector<std::string>& out)
{
    if (!vs || depth > 16) return;
    char nodeType[256] = {};
    prPluginID hash; csSDK_int32 flags = 0;
    if (vs->GetNodeInfo(nodeID, nodeType, &hash, &flags) != suiteError_NoError) return;
    if (std::strstr(nodeType, "Media")) {
        PrMemoryPtr val = nullptr;
        if (vs->GetNodeProperty(nodeID, kVideoSegmentProperty_Media_InstanceString, &val)
                == suiteError_NoError && val) {
            std::string s(val);
            if (mem && mem->PrDisposePtr) mem->PrDisposePtr(val);
            if (LooksLikeBraw(s)) out.push_back(s);
        }
    }
    csSDK_int32 opCount = 0;
    if (vs->GetNodeOperatorCount(nodeID, &opCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < opCount; ++i) {
            csSDK_int32 c = 0;
            if (vs->AcquireOperatorNodeID(nodeID, i, &c) == suiteError_NoError) {
                CollectBrawPaths(vs, mem, c, depth+1, out);
                vs->ReleaseVideoNodeID(c);
            }
        }
    csSDK_int32 inCount = 0;
    if (vs->GetNodeInputCount(nodeID, &inCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < inCount; ++i) {
            csSDK_int32 c = 0; PrTime off = 0;
            if (vs->AcquireInputNodeID(nodeID, i, &off, &c) == suiteError_NoError) {
                CollectBrawPaths(vs, mem, c, depth+1, out);
                vs->ReleaseVideoNodeID(c);
            }
        }
}


// Like FindMediaPath, but for the matched .braw media node also reads its trimmed in/out
// points (source media time, in Premiere ticks). -1 if unavailable.
static std::string FindMediaInfo(PrSDKVideoSegmentSuite* vs, PrSDKMemoryManagerSuite* mem,
                                 csSDK_int32 nodeID, int depth,
                                 long long& inTicks, long long& outTicks)
{
    if (!vs || depth > 16) return {};
    char nodeType[256] = {};
    prPluginID hash; csSDK_int32 flags = 0;
    if (vs->GetNodeInfo(nodeID, nodeType, &hash, &flags) != suiteError_NoError) return {};
    if (std::strstr(nodeType, "Media")) {
        PrMemoryPtr val = nullptr;
        if (vs->GetNodeProperty(nodeID, kVideoSegmentProperty_Media_InstanceString, &val)
                == suiteError_NoError && val) {
            std::string s(val);
            if (mem && mem->PrDisposePtr) mem->PrDisposePtr(val);
            if (LooksLikeBraw(s)) {
                auto readTicks = [&](const char* key) -> long long {
                    PrMemoryPtr v = nullptr; long long t = -1;
                    if (vs->GetNodeProperty(nodeID, key, &v) == suiteError_NoError && v) {
                        t = std::strtoll((const char*)v, nullptr, 10);
                        if (mem && mem->PrDisposePtr) mem->PrDisposePtr(v);
                    }
                    return t;
                };
                inTicks  = readTicks(kVideoSegmentProperty_Media_InPointMediaTimeAsTicks);
                outTicks = readTicks(kVideoSegmentProperty_Media_OutPointMediaTimeAsTicks);
                return s;
            }
        }
    }
    csSDK_int32 opCount = 0;
    if (vs->GetNodeOperatorCount(nodeID, &opCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < opCount; ++i) {
            csSDK_int32 c = 0;
            if (vs->AcquireOperatorNodeID(nodeID, i, &c) == suiteError_NoError) {
                std::string r = FindMediaInfo(vs, mem, c, depth+1, inTicks, outTicks);
                vs->ReleaseVideoNodeID(c);
                if (!r.empty()) return r;
            }
        }
    csSDK_int32 inCount = 0;
    if (vs->GetNodeInputCount(nodeID, &inCount) == suiteError_NoError)
        for (csSDK_int32 i = 0; i < inCount; ++i) {
            csSDK_int32 c = 0; PrTime off = 0;
            if (vs->AcquireInputNodeID(nodeID, i, &off, &c) == suiteError_NoError) {
                std::string r = FindMediaInfo(vs, mem, c, depth+1, inTicks, outTicks);
                vs->ReleaseVideoNodeID(c);
                if (!r.empty()) return r;
            }
        }
    return {};
}

// ── Shared per-clip gyro cache ────────────────────────────────────────────────
// Premiere creates a fresh filter instance (→ Initialize) for every render thread
// AND again on every parameter change. Re-opening the .braw clip, re-reading 11,699
// gyro samples and re-decoding frame 0 each time is both slow (the "wait a minute"
// stalls) and dangerous: tearing the old instance down joins its focal-read thread,
// which can be stuck in BRAW's FlushJobs → a hard deadlock that wedges Premiere.
//
// So the heavy load is done ONCE per media path and shared. Instances hold a
// shared_ptr; the reader/integrator stay alive (and its single focal thread runs to
// completion) for the whole session. Instance teardown just drops the pointer — no
// clip re-open, no per-instance focal thread, no join-on-a-deadlocked-thread.
struct GyroClip {
    std::unique_ptr<BRAWReader>     reader;
    std::unique_ptr<GyroIntegrator> gyro;
};
static std::mutex                                       sClipMutex;
static std::map<std::string, std::shared_ptr<GyroClip>> sClipCache;

static std::shared_ptr<GyroClip> GetOrLoadClip(const std::string& path)
{
    std::lock_guard<std::mutex> lk(sClipMutex);
    auto it = sClipCache.find(path);
    if (it != sClipCache.end()) {
        GLog("GPU clip cache HIT: '%s'", path.c_str());
        return it->second;
    }
    auto clip = std::make_shared<GyroClip>();
    clip->reader = std::make_unique<BRAWReader>();
    if (!clip->reader->Open(path)) {
        GLog("GPU clip cache: Open FAILED '%s'", path.c_str());
        return nullptr;
    }
    clip->gyro = std::make_unique<GyroIntegrator>();
    clip->gyro->LoadFromBRAW(*clip->reader);
    // gyroSpan (capture time) vs clipDur (playback time) reveals off-speed/conformed footage:
    // when they differ the render→gyro time scale compensates (see sp.gyroTimeScale).
    GLog("GPU clip cache MISS → loaded: '%s' samples=%d gyroSpan=%.2fs clipDur=%.2fs",
         path.c_str(), (int)clip->reader->GetGyroSamples().size(),
         clip->reader->GetGyroDurationSec(), clip->reader->GetClipDurationSec());
    sClipCache[path] = clip;
    return clip;
}

// ──────────────────────────────────────────────────────────────────────────────
class GyroStabGPU : public PrGPUFilterBase
{
public:
    virtual ~GyroStabGPU() = default;

    static prSuiteError Startup(piSuitesPtr, csSDK_int32 inIndex)
    {
        GLog("GPU Startup called (index=%d) — host discovered xGPUFilterEntry", (int)inIndex);
        return suiteError_NoError;
    }

    prSuiteError Initialize(PrGPUFilterInstance* ioInstanceData) override
    {
        PrGPUFilterBase::Initialize(ioInstanceData);
        GLog("GPU Initialize: deviceIdx=%u framework=%d (Metal=%d)",
             (unsigned)mDeviceIndex, (int)mDeviceInfo.outDeviceFramework, (int)PrGPUDeviceFramework_Metal);

        if (mDeviceIndex >= kMaxDevices ||
            mDeviceInfo.outDeviceFramework != PrGPUDeviceFramework_Metal) {
            GLog("GPU Initialize: not Metal / device out of range → fail (CPU fallback)");
            return suiteError_Fail;
        }

        // Compile + cache the warp pipeline for this device.
        if (!sPipelineCache[mDeviceIndex]) @autoreleasepool {
            id<MTLDevice> device = (id<MTLDevice>)mDeviceInfo.outDeviceHandle;
            NSError* err = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:@(kGyroWarpKernelSrc)
                                                      options:nil error:&err];
            if (!lib) return suiteError_Fail;
            id<MTLFunction> fn = [lib newFunctionWithName:@"gyro_warp"];
            if (!fn) { [lib release]; return suiteError_Fail; }
            sPipelineCache[mDeviceIndex] = [device newComputePipelineStateWithFunction:fn error:&err];
            [fn release];                 // +1 owned (ARC off) — pipeline keeps its own ref
            [lib release];
            if (!sPipelineCache[mDeviceIndex]) return suiteError_Fail;
        }

        // Ticks-per-second for clip-time → seconds conversion.
        PrSDKTimeSuite* timeSuite = nullptr;
        mSuites->utilFuncs->getSPBasicSuite()->AcquireSuite(
            kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, (const void**)&timeSuite);
        mTicksPerSecond = kFallbackTicksPerSecond;
        if (timeSuite) {
            timeSuite->GetTicksPerSecond(&mTicksPerSecond);
            mSuites->utilFuncs->getSPBasicSuite()->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
        }

        // NOTE: the .braw media is resolved lazily on the first Render, by matching the
        // render's sequence time to its timeline segment (see ResolveClip). Node-ID based
        // matching failed (the segment graph uses a different node-ID space than mNodeID),
        // so segment scanning alone always returned the FIRST clip's media on a multi-clip
        // timeline — every clip got the same (wrong) gyro. Sequence-time matching is exact.
        return suiteError_NoError;
    }

    // Resolve which clip THIS render belongs to by its timeline position. Each clip is a
    // segment [startTime, endTime) in sequence time; the segment containing inSequenceTime
    // is our clip. Falls back to the first media only if no segment matches.
    void ResolveClip(PrTime sequenceTime)
    {
        PrSDKVideoSegmentSuite*  vs  = mVideoSegmentSuite;
        PrSDKMemoryManagerSuite* mem = mMemoryManagerSuite;
        auto useIfBraw = [&](std::string p) -> bool {
            if (!LooksLikeBraw(p)) return false;
            if (p.rfind("file://", 0) == 0) p.erase(0, 7);
            mStatusKey = statusstore::keyOf(p);
            mClip = GetOrLoadClip(p);
            return true;
        };

        // (1) From THIS effect operator, get the clip node it's applied to (its owner), then
        //     that clip's media. This is the correct clip even with stacked tracks / multicam,
        //     where a timeline-segment scan would grab the wrong (e.g. underlying) clip. We also
        //     capture the clip's trimmed in/out so the crop only considers the used range.
        {
            csSDK_int32 owner = 0;
            if (vs->AcquireOperatorOwnerNodeID(mNodeID, &owner) == suiteError_NoError && owner) {
                long long inT = -1, outT = -1;
                std::string p = FindMediaInfo(vs, mem, owner, 0, inT, outT);   // .braw or ""
                // Even when there's no .braw, get SOME media file name (e.g. the h265) so we
                // can key this clip's status — that's how the UI looks up its own clip.
                std::string media = !p.empty() ? p : FindMediaPath(vs, mem, owner, 0);
                vs->ReleaseVideoNodeID(owner);
                mStatusKey = statusstore::keyOf(media);
                if (inT >= 0 && outT > inT && mTicksPerSecond > 0) {
                    mClipInSec  = (double)inT  / (double)mTicksPerSecond;
                    mClipOutSec = (double)outT / (double)mTicksPerSecond;
                }
                GLog("GPU ResolveClip: node=%d ownerMedia='%s' key='%s' in/out=%.2f..%.2fs",
                     (int)mNodeID, media.c_str(), mStatusKey.c_str(), mClipInSec, mClipOutSec);
                if (useIfBraw(p)) return;
                // Owner clip found but it has NO .braw media (FindMediaInfo only ever returns
                // a .braw, even through a proxy). So THIS effect is on a non-BRAW clip — stop
                // here. Don't fall through to the segment scan, which would otherwise grab a
                // neighbouring clip's .braw and mislabel this as a supported clip.
                GLog("GPU ResolveClip: node=%d owner clip has no BRAW → unsupported", (int)mNodeID);
                mClip = nullptr;
                return;
            }
        }

        // (2) The effect's own clip file name. If it's already a full .braw path, use it; else
        //     keep the basename to disambiguate among the composite's media below.
        std::string effName;
        {
            PrMemoryPtr val = nullptr;
            if (vs->GetNodeProperty(mNodeID, kVideoSegmentProperty_Effect_FileName, &val)
                    == suiteError_NoError && val) {
                effName = std::string(val);
                if (mem && mem->PrDisposePtr) mem->PrDisposePtr(val);
            }
            GLog("GPU ResolveClip: node=%d effectFileName='%s'", (int)mNodeID, effName.c_str());
            if (useIfBraw(effName)) return;
        }
        std::string effBase = BaseName(effName);

        // (3) Walk the timeline segments. Collect EVERY .braw in each (a composite has several),
        //     and prefer the one whose basename matches the effect's own clip; else the segment
        //     under the render's sequence time; else the first .braw found.
        std::string nameMatch, timePath, firstPath;
        csSDK_int32 segsID = 0;
        if (vs->AcquireVideoSegmentsID(mTimelineID, &segsID) == suiteError_NoError) {
            csSDK_int32 segCount = 0;
            vs->GetSegmentCount(segsID, &segCount);
            for (csSDK_int32 i = 0; i < segCount; ++i) {
                PrTime st = 0, et = 0, off = 0; prPluginID hash;
                if (vs->GetSegmentInfo(segsID, i, &st, &et, &off, &hash) != suiteError_NoError) continue;
                csSDK_int32 nodeID = 0;
                if (vs->AcquireNodeID(segsID, &hash, &nodeID) != suiteError_NoError) continue;
                std::vector<std::string> paths;
                CollectBrawPaths(vs, mem, nodeID, 0, paths);
                vs->ReleaseVideoNodeID(nodeID);
                bool covers = (sequenceTime >= st && sequenceTime < et);
                for (const auto& p : paths) {
                    GLog("  seg[%d]%s media='%s'", (int)i, covers ? "*" : "", p.c_str());
                    if (firstPath.empty()) firstPath = p;
                    if (covers && timePath.empty()) timePath = p;
                    if (!effBase.empty() && nameMatch.empty() && BaseName(p) == effBase) nameMatch = p;
                }
            }
            vs->ReleaseVideoSegmentsID(segsID);
        }
        std::string path = !nameMatch.empty() ? nameMatch
                         : !timePath.empty()  ? timePath
                         :                       firstPath;
        if (path.rfind("file://", 0) == 0) path.erase(0, 7);
        GLog("GPU ResolveClip: node=%d seqTime=%lld effBase='%s' → '%s'",
             (int)mNodeID, (long long)sequenceTime, effBase.c_str(), path.c_str());
        if (!path.empty()) {
            if (mStatusKey.empty()) mStatusKey = statusstore::keyOf(path);
            mClip = GetOrLoadClip(path);
        }
    }

    prSuiteError Render(const PrGPUFilterRenderParams* inRenderParams,
                        const PPixHand* inFrames, csSDK_size_t inFrameCount,
                        PPixHand* outFrame) override
    {
        if (inFrameCount < 1) return suiteError_Fail;

        // Update debug-logging state first so the clip resolve + focal scan below can log.
        dbg::setEnabled(GetParam(kParamDebugLog, inRenderParams->inClipTime).mBool != 0);

        statusstore::setRenderPath("GPU (Metal)");

        static bool sLoggedOnce = false;
        if (!sLoggedOnce) { sLoggedOnce = true; GLog("GPU Render invoked (GPU path active)"); }

        // Resolve this instance's clip on first render, using the render's sequence time
        // to pick the right timeline segment (multi-clip timelines need the per-render
        // position; it isn't available in Initialize).
        std::call_once(mResolveOnce, [&]{ ResolveClip(inRenderParams->inSequenceTime); });

        // Publish THIS clip's status (keyed by its media file) so the Effect Controls UI
        // can look up its own clip. The GPU is the authority: it resolves the right clip
        // and knows if it stabilizes. Keyed so several clips don't share one value.
        if (!mStatusKey.empty()) {
            if (!mClip) {
                statusstore::put(mStatusKey, statusstore::Unsupported, nullptr);
            } else if (mClip->reader && mClip->reader->GetGyroSampleCount() > 0) {
                statusstore::put(mStatusKey, statusstore::Ok, mClip->reader->GetCameraName().c_str());
            } else {
                statusstore::put(mStatusKey, statusstore::NoGyro, nullptr);
            }
        }

        void* inData = nullptr;  mGPUDeviceSuite->GetGPUPPixData(inFrames[0], &inData);
        void* outData = nullptr; mGPUDeviceSuite->GetGPUPPixData(*outFrame, &outData);
        if (!inData || !outData) return suiteError_Fail;

        PrPixelFormat fmt = PrPixelFormat_Invalid;
        mPPixSuite->GetPixelFormat(*outFrame, &fmt);
        prRect bounds = {}; mPPixSuite->GetBounds(*outFrame, &bounds);
        int width  = bounds.right - bounds.left;
        int height = bounds.bottom - bounds.top;
        csSDK_int32 rowBytes = 0; mPPixSuite->GetRowBytes(*outFrame, &rowBytes);

        {   // one-time: compare input vs output frame geometry
            static bool once = false;
            if (!once) { once = true;
                prRect ib = {}; mPPixSuite->GetBounds(inFrames[0], &ib);
                csSDK_int32 irb = 0; mPPixSuite->GetRowBytes(inFrames[0], &irb);
                PrPixelFormat ifmt = PrPixelFormat_Invalid; mPPixSuite->GetPixelFormat(inFrames[0], &ifmt);
                GLog("GEOM out: %dx%d rb=%d fmt=%d | in: %dx%d rb=%d fmt=%d | inData=%p outData=%p SAME=%d",
                     width, height, (int)rowBytes, (int)fmt,
                     ib.right-ib.left, ib.bottom-ib.top, (int)irb, (int)ifmt,
                     inData, outData, (int)(inData == outData));
            }
        }
        int bpp   = GetGPUBytesPerPixel(fmt);          // 16 (32f) or 8 (16f)
        int pitch = rowBytes / bpp;                    // pixels per row
        int is16f = (fmt != PrPixelFormat_GPU_BGRA_4444_32f) ? 1 : 0;

        // Compute the warp map for this frame (CPU, cheap).
        WarpMap wm;
        if (mClip && mClip->gyro) {
            BRAWReader*     reader = mClip->reader.get();
            GyroIntegrator* gyro   = mClip->gyro.get();
            double timeSec = (double)inRenderParams->inClipTime / (double)mTicksPerSecond;
            // Float-slider params come back in mFloat64 (mFloat32 is garbage); checkboxes in mBool.
            PrTime t = inRenderParams->inClipTime;
            StabilizationParams sp;
            sp.smoothingWindowSec = SmoothingPctToSigma((float)GetParam(kParamSmoothingStrength, t).mFloat64);
            bool  scaleToFill     = GetParam(kParamScaleToFill,  t).mBool != 0;
            float rsOverride      = (float)GetParam(kParamRollingShutter,    t).mFloat64;
            float focalParam      = (float)GetParam(kParamFocalLength,       t).mFloat64;
            float horizonAmt      = (float)GetParam(kParamHorizonAmount,     t).mFloat64;
            float horizonTilt     = (float)GetParam(kParamHorizonTilt,       t).mFloat64;
            float scaleFollows    = (float)GetParam(kParamScaleFollowsZoom,  t).mFloat64;
            float extraScale      = (float)GetParam(kParamExtraScale,        t).mFloat64;

            sp.zoomToFill       = scaleToFill;
            sp.horizonStrength  = horizonAmt / 100.0f;   // 0 = off (no separate toggle)
            sp.horizonTiltRad   = horizonTilt * 0.01745329252f;   // degrees → radians
            sp.zoomSteadiness   = 0.0f;   // stabilization border-crop is pure adaptive; the
                                          // "Scale Follows Zoom" control now drives zoomEaseScale below
            sp.extraCropFrac    = extraScale / 100.0f;
            sp.clipInSec        = mClipInSec;    // restrict the crop scan to the trimmed range
            sp.clipOutSec       = mClipOutSec;
            // Off-speed footage: map render(playback) time → gyro(capture) time.
            {
                double gd = reader->GetGyroDurationSec(), cd = reader->GetClipDurationSec();
                sp.gyroTimeScale = (gd > 0.1 && cd > 0.1) ? gd / cd : 1.0;
            }
            sp.focalLengthMM    = (focalParam > 0.0f) ? focalParam : reader->GetFocalLength(timeSec);
            // "Scale Follows Zoom" (0..100) eases the lens's hard zoom start/stop by digitally
            // zooming so the apparent focal follows a smoothed curve. Off when focal is a manual
            // fixed override (no zoom to ease).
            sp.zoomEaseScale    = (focalParam > 0.0f) ? 1.0f
                                                     : reader->GetZoomEaseScale(timeSec, 1.2, scaleFollows / 100.0f);
            sp.maxFocalLengthMM = (focalParam > 0.0f) ? focalParam : reader->GetMaxFocalLength();
            sp.photositePitchUm = reader->GetPhotositePitchUm();
            sp.rollingShutterMs = (rsOverride > 0.0f) ? rsOverride : reader->GetRollingShutterMs();
            wm = gyro->ComputeWarpMap(timeSec, sp, width, height);

            // Diagnostic: log whenever the Smoothing slider value reaching the GPU
            // render changes (and the first few frames). If `smooth=` tracks the slider
            // and `tx` grows with it, the param flows and smoothing is working; if
            // `smooth=` never changes, GetParam isn't seeing the UI value.
            static float sLastSmooth = -999.f; static int sLogCount = 0;
            if ((sp.smoothingWindowSec != sLastSmooth || sLogCount < 8)
                    && wm.IsValid() && wm.height > 1) {
                sLastSmooth = sp.smoothingWindowSec; ++sLogCount;
                const float* T = &wm.rowHomographies[0];                 // top row
                const float* M = &wm.rowHomographies[(wm.height/2)*9];   // middle row
                const float* B = &wm.rowHomographies[(wm.height-1)*9];   // bottom row
                GLog("WARP smooth=%.3f focal=%.1f zoom=%d rs=%.1f | tx top/mid/bot=%.1f/%.1f/%.1f",
                     sp.smoothingWindowSec, sp.focalLengthMM, (int)sp.zoomToFill,
                     sp.rollingShutterMs, T[2], M[2], B[2]);
            }
        }

        // No valid warp (not a .braw, no gyro, or load failed) → pass the frame through.
        // In-place (in == out) needs nothing; if the host gave separate buffers, copy in→out
        // so we never emit garbage.
        if (!wm.IsValid()) {
            if (inData != outData) @autoreleasepool {
                id<MTLCommandQueue> queue = (id<MTLCommandQueue>)mDeviceInfo.outCommandQueueHandle;
                size_t frameSize = 0; mGPUDeviceSuite->GetGPUPPixSize(*outFrame, &frameSize);
                id<MTLCommandBuffer> cmd = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                [blit copyFromBuffer:(id<MTLBuffer>)inData sourceOffset:0
                            toBuffer:(id<MTLBuffer>)outData destinationOffset:0 size:frameSize];
                [blit endEncoding]; [cmd commit];
            }
            return suiteError_NoError;
        }

        @autoreleasepool {
            id<MTLDevice>       device = (id<MTLDevice>)mDeviceInfo.outDeviceHandle;
            id<MTLCommandQueue> queue  = (id<MTLCommandQueue>)mDeviceInfo.outCommandQueueHandle;
            id<MTLBuffer> inBuf  = (id<MTLBuffer>)inData;    // warp the INPUT (correct even if in != out)
            id<MTLBuffer> outBuf = (id<MTLBuffer>)outData;

            // The warp samples arbitrary source locations, so it cannot read the output
            // buffer it writes. Copy the input into a private scratch buffer first.
            //
            // The scratch buffer is allocated PER CALL (not a shared member). Premiere
            // calls Render concurrently from several playback threads, and it changes the
            // preview resolution after a parameter edit. A shared/reallocated scratch was
            // being freed on one thread while another thread's in-flight GPU command still
            // read it → a Metal command-queue fault that wedged ALL preview rendering until
            // Premiere was restarted (scrubbing is single-threaded so it survived; playback
            // is multi-threaded so it hung). A per-call buffer is bound to its own command
            // buffer, which keeps it alive until the GPU finishes, then the autorelease pool
            // frees it — no cross-thread sharing, no use-after-free.
            size_t frameSize = 0;
            mGPUDeviceSuite->GetGPUPPixSize(*outFrame, &frameSize);
            id<MTLBuffer> scratch = [device newBufferWithLength:frameSize
                                                       options:MTLResourceStorageModePrivate];

            id<MTLBuffer> homoBuf = [device newBufferWithBytes:wm.rowHomographies.data()
                                       length:wm.rowHomographies.size() * sizeof(float)
                                      options:MTLResourceStorageModeShared];
            WarpUniforms uni = { (uint32_t)width, (uint32_t)height, (uint32_t)pitch, (uint32_t)is16f };

            id<MTLCommandBuffer> cmd = [queue commandBuffer];

            // 1) input → scratch  (the warp samples arbitrary source pixels, so it can't read
            //    the buffer it writes; copy the INPUT in first)
            id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
            [blit copyFromBuffer:inBuf sourceOffset:0 toBuffer:scratch destinationOffset:0 size:frameSize];
            [blit endEncoding];

            // 2) warp scratch → output (in place)
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:sPipelineCache[mDeviceIndex]];
            [enc setBuffer:scratch offset:0 atIndex:0];
            [enc setBuffer:outBuf   offset:0 atIndex:1];
            [enc setBuffer:homoBuf  offset:0 atIndex:2];
            [enc setBytes:&uni length:sizeof(uni) atIndex:3];
            MTLSize tg = MTLSizeMake(16, 16, 1);
            MTLSize grid = MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
            [cmd commit];

            // ARC is OFF: `newBufferWith…` returns +1 OWNED objects, NOT autoreleased,
            // so the @autoreleasepool does not free them — without these releases we
            // leak a full-frame scratch buffer (+ the homography buffer) EVERY frame,
            // i.e. gigabytes per minute of playback. The command buffer keeps its own
            // retain on both until the GPU finishes, so releasing our reference now is
            // safe. This was the runaway "very high memory usage".
            [homoBuf release];
            [scratch release];
        }
        return suiteError_NoError;
    }

    // MatchName/PluginCount/Startup inherit PrGPUFilterBase defaults; an empty
    // match name makes the host link this GPU filter to our effect via its PiPL.
    static prSuiteError Shutdown(piSuitesPtr, csSDK_int32 inIndex)
    {
        @autoreleasepool {
            if (inIndex >= 0 && inIndex < kMaxDevices && sPipelineCache[inIndex]) {
                sPipelineCache[inIndex] = nil;
            }
        }
        return suiteError_NoError;
    }

private:
    std::shared_ptr<GyroClip>       mClip;   // shared, loaded once per media path
    std::once_flag                  mResolveOnce;
    PrTime                          mTicksPerSecond = kFallbackTicksPerSecond;
    double                          mClipInSec  = 0.0;     // this instance's trimmed in/out
    double                          mClipOutSec = 1e12;    // (source seconds) for the crop scan
    std::string                     mStatusKey;            // clip key for StatusStore (keyOf media file)
};

DECLARE_GPUFILTER_ENTRY(PrGPUFilterModule<GyroStabGPU>)
