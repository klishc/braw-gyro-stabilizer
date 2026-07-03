// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// BRAWReader.h
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <utility>

class IBlackmagicRawFactory;
class IBlackmagicRaw;
class IBlackmagicRawClip;

struct GyroSample {
    double timestampSec;
    float  gx, gy, gz;
    float  ax, ay, az;
};

class BRAWReader {
public:
    BRAWReader();
    ~BRAWReader();

    bool Open(const std::string& filePath);
    void Close();

    const std::vector<GyroSample>& GetGyroSamples() const { return mGyroSamples; }

    float GetFocalLength(double timeSec) const;   // per-frame, interpolated from the scan
    // Symmetric (no-lag) temporal smoothing of the focal track, for the digital-scale
    // follow only — rounds the lens's hard zoom start/stop so the auto-crop eases in/out.
    float GetFocalLengthSmoothed(double timeSec, double windowSec) const;
    // Extra digital zoom (>= 1) that eases the lens zoom, confined to ~leadFrames around each
    // zoom (crop before a zoom-in, during/after a zoom-out) and 1.0 elsewhere. leadFrames is
    // driven by "Scale Follows Zoom" (~%/10); leadFrames < 0.5 (or no track) returns 1.0 (off).
    float GetZoomEaseScale(double timeSec, float leadFrames) const;
    float GetMaxFocalLength()    const;           // longest focal across the clip
    float GetRollingShutterMs()  const;   // metadata value, or the defaults.ini
                                          // "rolling_shutter" override when > 0 (hot-reloaded)
    float GetPhotositePitchUm()  const { return mPhotositePitchUm; }  // 0 if unknown
    float GetFrameRate()         const { return mFrameRate; }
    int   GetFrameCount()        const { return mFrameCount; }
    int   GetWidth()             const { return mWidth; }
    int   GetHeight()            const { return mHeight; }

    // Detected lens (best-effort, "" if unknown) and whether the focal scan finished.
    std::string GetLensName()    const { return mLensName; }
    bool        IsFocalReady()   const { return mFocalReady.load(); }

    // Camera model from clip metadata (best-effort, "" if unknown), e.g.
    // "Pocket Cinema Camera 6K Pro".
    std::string GetCameraName()  const { return mCameraName; }
    int         GetGyroSampleCount() const { return (int)mGyroSamples.size(); }

    // Real (capture-time) length of the gyro stream in seconds. For off-speed/conformed
    // footage (e.g. shot 48 fps, played 24 fps) this is the SHOOTING duration, which is
    // shorter than the clip's playback duration — used to map render time → gyro time.
    double GetGyroDurationSec() const {
        return mGyroSamples.empty() ? 0.0 : mGyroSamples.back().timestampSec;
    }
    // Playback (timeline) duration from frame metadata.
    double GetClipDurationSec() const {
        return (mFrameRate > 0.f) ? (double)mFrameCount / (double)mFrameRate : 0.0;
    }

private:
    void ExtractGyroStream();
    void ExtractClipMetadata();
    void ExtractRollingShutter();
    void ScanFocalTrack();              // background: sample focal across the clip
    bool LoadSidecar();                 // read cached focal track next to the .braw
    void WriteSidecar() const;          // write it after a fresh scan
    void SmoothFocalTrack();            // de-quantize the integer-mm steps

    std::string            mPath;            // clip path (key for the shared focal cache)
    IBlackmagicRawFactory* mFactory = nullptr;
    IBlackmagicRaw*        mCodec   = nullptr;
    IBlackmagicRawClip*    mClip    = nullptr;

    std::vector<GyroSample> mGyroSamples;

    // Focal length is scanned across the clip on a background thread (the BRAW frame-
    // decode FlushJobs deadlocks if run on Premiere's render thread). The track is built
    // fully, then mFocalReady is published — readers only touch mFocalTrack once ready,
    // so no per-read lock is needed. mFocalLengthMM is the single-value fallback.
    std::atomic<float>  mFocalLengthMM{35.f};
    std::thread         mFocalThread;
    std::vector<std::pair<double,float>> mFocalTrack;   // (timeSec, focalMM), sorted by time
    std::atomic<bool>   mFocalReady{false};
    // Cached per-sample ease ratio (appFocal/actual) over the whole track, keyed by
    // blend+window. The zoom-ease normalizes LOCALLY (min ratio within a window around each
    // frame) so the crop is confined to the zoom and flat regions stay at 1.0.
    mutable std::mutex  mZoomEaseMutex;
    mutable bool        mZoomEaseValid   = false;
    mutable float       mZoomEaseBlend   = -1.f;
    mutable float       mZoomEaseWindow  = -1.f;
    mutable std::vector<std::pair<double,float>> mZoomEaseTrack;  // (timeSec, smoothed ease scale >=1)
    std::string         mLensName;
    std::string         mCameraName;     // camera model (clip metadata, best effort)
    float mSensorCropFactor = 1.f;
    float mRollingShutterMs = 16.f;
    float mPhotositePitchUm = 0.f;   // sensor photosite pitch (µm); 0 = unknown
    float mFrameRate  = 24.f;
    int   mFrameCount = 0;
    int   mWidth      = 0;
    int   mHeight     = 0;
};
