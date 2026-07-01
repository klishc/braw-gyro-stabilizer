// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// GyroIntegrator.h
// Converts raw IMU gyro samples into per-frame stabilization warp maps.
//
// Pipeline:
//   1. Integrate angular velocity → absolute orientation quaternions
//   2. Compute Gaussian-smoothed "desired" orientation (what we want)
//   3. correction = desired · actual⁻¹
//   4. Project correction rotation through pinhole model → 3×3 homography
//   5. Per scanline: compute at its own RS-corrected exposure time
#pragma once

#include "../braw/BRAWReader.h"
#include "WarpMap.h"
#include <vector>
#include <array>
#include <mutex>

// ── Quaternion (w, x, y, z) ──────────────────────────────────────────────────
struct Quat {
    float w = 1.f, x = 0.f, y = 0.f, z = 0.f;

    Quat  operator*(const Quat& b) const noexcept;
    Quat  conjugate()              const noexcept;
    Quat  normalized()             const noexcept;
    float dot(const Quat& b)       const noexcept;
};

Quat slerp(Quat a, Quat b, float t) noexcept;

// ── Stabilization parameters ─────────────────────────────────────────────────
struct StabilizationParams {
    float smoothingWindowSec = 0.5f;    // Gaussian σ — controls smoothness vs. correction

    // Camera optics (from BRAW metadata)
    float focalLengthMM      = 24.0f;
    float rollingShutterMs   = 16.0f;   // full-frame readout duration
    float photositePitchUm   = 0.0f;    // sensor photosite pitch (µm); if >0, used for fx/fy

    // Sensor geometry — fallback only, used when photositePitchUm is unknown
    float sensorWidthMM      = 17.3f;
    float sensorHeightMM     = 13.0f;

    float horizonStrength    = 0.0f;    // 0 = no horizon lock, 1 = fully level
    float horizonTiltRad     = 0.0f;    // target horizon angle (rad); 0 = level, ± = tilted
    bool  zoomToFill         = true;    // when true, zoom to hide the exposed borders
    float maxFocalLengthMM   = 0.0f;    // clip's longest focal (for the constant-zoom mode)
    float zoomSteadiness     = 1.0f;    // 1 = constant zoom, 0 = follows the lens zoom
    float extraCropFrac      = 0.0f;    // manual extra zoom (fraction); scales the crop depth
    double clipInSec         = 0.0;     // trimmed in/out (source seconds); the crop scan is
    double clipOutSec        = 1e12;    // limited to this range so untrimmed motion is ignored
    double gyroTimeScale     = 1.0;     // render-time → gyro(capture)-time factor for off-speed
                                        // footage (= gyroDuration / clipPlaybackDuration; 1 = normal)
};

// ── GyroIntegrator ───────────────────────────────────────────────────────────
class GyroIntegrator {
public:
    GyroIntegrator()  = default;
    ~GyroIntegrator() = default;

    // Load gyro data from an opened BRAWReader and integrate.
    void LoadFromBRAW(const BRAWReader& reader);

    // Compute the full per-row warp map for one frame, at the RENDER resolution
    // (renderWidth/Height; pass <=0 to use the clip's native resolution).
    WarpMap ComputeWarpMap(double frameCenterTimeSec,
                           const StabilizationParams& params,
                           int renderWidth,
                           int renderHeight) const;

private:
    // ── Integration ──────────────────────────────────────────────────────
    void   IntegrateGyro();
    void   DumpDiagnostics() const;   // one-time stats → /tmp/brawgyro.log
    Quat   OrientationAt(double timeSec) const;

    // ── Smoothing ─────────────────────────────────────────────────────────
    Quat   SmoothedOrientation(double timeSec, float windowSec) const;

    // ── Projection ────────────────────────────────────────────────────────
    std::array<float,9> CorrectionToHomography(
        const Quat& actual,
        const Quat& desired,
        float fx, float fy,
        float cx, float cy) const;

    // ── Adaptive zoom ─────────────────────────────────────────────────────
    // Crop fraction needed to hide the borders the stabilization exposes at one
    // instant (1.0 = none). More camera motion → smaller value → more zoom.
    float RequiredCrop(double timeSec, const Quat& desired, double rsDuration,
                       float fx, float fy, float cx, float cy,
                       int renderWidth, int renderHeight) const;

    // Gyro samples (borrowed from BRAWReader — valid as long as reader lives)
    const std::vector<GyroSample>* mSamples = nullptr;

    // Integrated absolute orientations (one per gyro sample)
    std::vector<Quat>   mOrientations;
    std::vector<double> mTimestamps;

    // Clip geometry
    float mFrameRate   = 24.f;
    int   mFrameWidth  = 0;
    int   mFrameHeight = 0;

    // Clip-wide CONSTANT zoom-to-fill crop, cached. A per-frame adaptive crop "breathes"
    // with micro-shake (visible jitter); a single crop for the clip never changes →
    // no breathing. Recomputed when the inputs that affect it change.
    mutable std::mutex mCropMutex;
    mutable float      mCachedDepthPerFocal = 0.f;  // clip-wide worst border per mm of focal
    mutable bool       mCropValid      = false;
    mutable float      mKeySmooth      = -1.f;
    mutable float      mKeyHorizon     = -1.f;
    mutable double     mKeyIn          = -1.0;
    mutable double     mKeyOut         = -1.0;
};
