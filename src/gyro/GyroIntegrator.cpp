// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// GyroIntegrator.cpp
// Full gyro integration → stabilization homography pipeline.

#include "GyroIntegrator.h"
#include "../common/DebugLog.h"

#include <cmath>
#include <algorithm>
#include <numeric>
#include <cassert>

// ═════════════════════════════════════════════════════════════════════════════
// Quaternion math
// ═════════════════════════════════════════════════════════════════════════════

Quat Quat::operator*(const Quat& b) const noexcept {
    return {
        w*b.w - x*b.x - y*b.y - z*b.z,
        w*b.x + x*b.w + y*b.z - z*b.y,
        w*b.y - x*b.z + y*b.w + z*b.x,
        w*b.z + x*b.y - y*b.x + z*b.w
    };
}

Quat Quat::conjugate() const noexcept {
    return {w, -x, -y, -z};
}

Quat Quat::normalized() const noexcept {
    float n = std::sqrt(w*w + x*x + y*y + z*z);
    if (n < 1e-10f) return {1,0,0,0};
    float inv = 1.f / n;
    return {w*inv, x*inv, y*inv, z*inv};
}

float Quat::dot(const Quat& b) const noexcept {
    return w*b.w + x*b.x + y*b.y + z*b.z;
}

Quat slerp(Quat a, Quat b, float t) noexcept {
    float d = a.dot(b);
    if (d < 0.f) { b = {-b.w,-b.x,-b.y,-b.z}; d = -d; }
    if (d > 0.9995f) {
        return Quat{a.w + t*(b.w-a.w),
                    a.x + t*(b.x-a.x),
                    a.y + t*(b.y-a.y),
                    a.z + t*(b.z-a.z)}.normalized();
    }
    float theta0 = std::acos(d);
    float theta  = theta0 * t;
    float s0     = std::cos(theta) - d * std::sin(theta) / std::sin(theta0);
    float s1     = std::sin(theta) / std::sin(theta0);
    return Quat{s0*a.w + s1*b.w,
                s0*a.x + s1*b.x,
                s0*a.y + s1*b.y,
                s0*a.z + s1*b.z}.normalized();
}

// Integrate angular velocity (wx,wy,wz rad/s) over dt seconds → delta quaternion.
//   axis  = ω/|ω|,  θ = |ω|·dt
//   q     = (cos(θ/2), axis·sin(θ/2)) = (cos(θ/2), ω·sin(θ/2)/|ω|)
static Quat AngVelToQuat(float wx, float wy, float wz, float dt) noexcept
{
    float omega = std::sqrt(wx*wx + wy*wy + wz*wz);   // |ω| (rad/s)
    float theta = omega * dt;                          // rotation this step (rad)
    if (theta < 1e-12f) return {1,0,0,0};
    float s = std::sin(theta * 0.5f) / omega;          // sin(θ/2)/|ω|; ω·s = axis·sin(θ/2)
    return Quat{std::cos(theta * 0.5f),
                wx * s,
                wy * s,
                wz * s}.normalized();
}

// Convert quaternion → 3×3 rotation matrix, row-major R[row][col]
static void QuatToMatrix(const Quat& q, float R[3][3]) noexcept
{
    float w=q.w, x=q.x, y=q.y, z=q.z;
    R[0][0]=1-2*(y*y+z*z); R[0][1]=2*(x*y-z*w); R[0][2]=2*(x*z+y*w);
    R[1][0]=2*(x*y+z*w);   R[1][1]=1-2*(x*x+z*z); R[1][2]=2*(y*z-x*w);
    R[2][0]=2*(x*z-y*w);   R[2][1]=2*(y*z+x*w); R[2][2]=1-2*(x*x+y*y);
}

// ═════════════════════════════════════════════════════════════════════════════
// GyroIntegrator
// ═════════════════════════════════════════════════════════════════════════════

void GyroIntegrator::LoadFromBRAW(const BRAWReader& reader)
{
    mSamples     = &reader.GetGyroSamples();
    mFrameRate   = reader.GetFrameRate();
    mFrameWidth  = reader.GetWidth();
    mFrameHeight = reader.GetHeight();
    IntegrateGyro();
    DumpDiagnostics();
}

// ── One-time diagnostics: units, dominant axis, gravity dir, time coverage ─────
#include <cstdio>
void GyroIntegrator::DumpDiagnostics() const
{
    if (!dbg::enabled()) return;     // skip the whole stats computation unless logging is on
    if (!mSamples || mSamples->empty()) return;
    static bool sDone = false; if (sDone) return; sDone = true;

    FILE* f = dbg::open(); if (!f) return;
    const auto& S = *mSamples; const size_t N = S.size();
    const double dur  = S.back().timestampSec - S.front().timestampSec;
    const double rate = (dur > 0) ? (N - 1) / dur : 0;

    double agx=0,agy=0,agz=0, mgx=0,mgy=0,mgz=0, bgx=0,bgy=0,bgz=0;
    double aax=0,aay=0,aaz=0;
    for (const auto& s : S) {
        agx+=std::fabs(s.gx); agy+=std::fabs(s.gy); agz+=std::fabs(s.gz);
        bgx+=s.gx; bgy+=s.gy; bgz+=s.gz;                       // signed mean = bias
        mgx=std::max(mgx,(double)std::fabs(s.gx));
        mgy=std::max(mgy,(double)std::fabs(s.gy));
        mgz=std::max(mgz,(double)std::fabs(s.gz));
        aax+=s.ax; aay+=s.ay; aaz+=s.az;
    }
    // Net integrated orientation start→end (axis-angle), to see total drift/pan.
    Quat q0 = mOrientations.front(), q1 = mOrientations.back();
    Quat dq = (q1 * q0.conjugate()).normalized();
    double ang = 2.0 * std::acos(std::min(1.0, std::fabs((double)dq.w))) * 180.0 / M_PI;

    std::fprintf(f, "=== GYRO DIAG ===\n");
    std::fprintf(f, "samples=%zu rate=%.1fHz dur=%.2fs | clip %dx%d @%.3ffps (clip dur=%.2fs)\n",
                 N, rate, dur, mFrameWidth, mFrameHeight, mFrameRate, (double)0);
    std::fprintf(f, "gyro mean|w| x/y/z = %.4f/%.4f/%.4f rad/s | max = %.3f/%.3f/%.3f | bias = %.4f/%.4f/%.4f\n",
                 agx/N,agy/N,agz/N, mgx,mgy,mgz, bgx/N,bgy/N,bgz/N);
    std::fprintf(f, "accel mean x/y/z = %.3f/%.3f/%.3f  (gravity ~ which axis points down)\n",
                 aax/N,aay/N,aaz/N);
    std::fprintf(f, "net orientation change start→end = %.1f deg\n", ang);

    // Gyro↔accel consistency: rotate each body-frame accel into world via the integrated
    // orientation. If the gyro axes/signs/integration are correct, gravity is a FIXED world
    // vector, so gWorld must stay ~constant across the clip. If it wanders, the gyro
    // convention is wrong — which makes the warp move but not actually stabilize.
    {
        auto rot = [](const Quat& q, float vx, float vy, float vz, double o[3]) {
            Quat p{0, vx, vy, vz};
            Quat r = (q * p) * q.conjugate();
            o[0] = r.x; o[1] = r.y; o[2] = r.z;
        };
        std::fprintf(f, "gravity-in-world at 6 times (must be ~CONSTANT if gyro axes OK):\n");
        for (int k = 0; k < 6; ++k) {
            size_t i = (N - 1) * k / 5;
            double g[3]; rot(mOrientations[i], S[i].ax, S[i].ay, S[i].az, g);
            std::fprintf(f, "  t=%.2f  gWorld=[%6.2f %6.2f %6.2f]\n",
                         S[i].timestampSec, g[0], g[1], g[2]);
        }
    }
    std::fprintf(f, "=== END GYRO DIAG ===\n");
    std::fclose(f);
}

// ── Integration ───────────────────────────────────────────────────────────────
// Gyro samples arrive at ~200 Hz as angular velocity (rad/s).
// We integrate by chaining delta quaternions: q[i] = q[i-1] * delta(i)
// Result: mOrientations[i] is the absolute camera orientation at mTimestamps[i].
void GyroIntegrator::IntegrateGyro()
{
    if (!mSamples || mSamples->empty()) return;

    const auto& S = *mSamples;
    const size_t N = S.size();
    mOrientations.resize(N);
    mTimestamps.resize(N);

    mOrientations[0] = {1,0,0,0};
    mTimestamps[0]   = S[0].timestampSec;

    for (size_t i = 1; i < N; ++i) {
        double dt = S[i].timestampSec - S[i-1].timestampSec;
        if (dt <= 0.0) dt = 1.0 / 200.0;

        // Use midpoint angular velocity for better accuracy
        float wx = 0.5f * (S[i].gx + S[i-1].gx);
        float wy = 0.5f * (S[i].gy + S[i-1].gy);
        float wz = 0.5f * (S[i].gz + S[i-1].gz);

        Quat delta = AngVelToQuat(wx, wy, wz, static_cast<float>(dt));
        mOrientations[i] = (mOrientations[i-1] * delta).normalized();
        mTimestamps[i]   = S[i].timestampSec;
    }
}

// ── Interpolate orientation at arbitrary time ─────────────────────────────────
Quat GyroIntegrator::OrientationAt(double timeSec) const
{
    if (mOrientations.empty())    return {1,0,0,0};
    if (timeSec <= mTimestamps.front()) return mOrientations.front();
    if (timeSec >= mTimestamps.back())  return mOrientations.back();

    // Binary search for bracketing samples
    auto it = std::lower_bound(mTimestamps.begin(), mTimestamps.end(), timeSec);
    size_t hi = static_cast<size_t>(it - mTimestamps.begin());
    size_t lo = hi - 1;

    double span = mTimestamps[hi] - mTimestamps[lo];
    float  t    = (span > 0.0)
                    ? static_cast<float>((timeSec - mTimestamps[lo]) / span)
                    : 0.f;
    return slerp(mOrientations[lo], mOrientations[hi], t);
}

// ── Gaussian-smoothed orientation ────────────────────────────────────────────
// Represents the "desired" camera path — slow, stable motion without shake.
// Shake = actual − desired → correction = desired · actual⁻¹
Quat GyroIntegrator::SmoothedOrientation(double timeSec, float windowSec) const
{
    if (mOrientations.empty()) return {1,0,0,0};
    // No (or invalid) smoothing window → the desired path equals the actual path.
    if (windowSec <= 1e-4f) return OrientationAt(timeSec);

    const double sigma  = static_cast<double>(windowSec);
    const double range  = 3.0 * sigma;      // sample ±3σ
    // Step is 120 Hz for small windows, but COARSENS for large σ so the iteration count is
    // bounded (~1000 max). A wide Gaussian is a smooth low-pass — coarse sampling doesn't
    // change the result — and this keeps cost flat instead of exploding at high Smoothing.
    const double step   = std::max(1.0 / 120.0, (2.0 * range) / 1000.0);

    // Weighted quaternion average using SLERP chaining.
    // To avoid sign-flip cancellation, we accumulate as a 4-vector
    // and ensure all samples are in the same hemisphere.
    float qSum[4] = {0,0,0,0};
    float totalW  = 0.f;
    Quat  refQ    = OrientationAt(timeSec);  // reference for hemisphere

    for (double dt = -range; dt <= range; dt += step) {
        double t = timeSec + dt;
        if (t < mTimestamps.front() || t > mTimestamps.back()) continue;

        float w = std::exp(static_cast<float>(-0.5 * (dt/sigma) * (dt/sigma)));
        Quat  q = OrientationAt(t);

        // Ensure same hemisphere as reference
        if (refQ.dot(q) < 0.f) q = {-q.w,-q.x,-q.y,-q.z};

        qSum[0] += w * q.w;
        qSum[1] += w * q.x;
        qSum[2] += w * q.y;
        qSum[3] += w * q.z;
        totalW  += w;
    }

    if (totalW < 1e-6f) return refQ;
    return Quat{qSum[0]/totalW, qSum[1]/totalW,
                qSum[2]/totalW, qSum[3]/totalW}.normalized();
}

// ── Correction rotation → 3×3 image-plane homography ─────────────────────────
// H = K · R_correction · K⁻¹
// K = [[fx, 0, cx], [0, fy, cy], [0, 0, 1]]
// This maps output pixels to source pixels via the inverse warp.
std::array<float,9> GyroIntegrator::CorrectionToHomography(
    const Quat& actual,
    const Quat& desired,
    float fx, float fy,
    float cx, float cy) const
{
    // Output→source mapping. Orientations are world-from-body (q rotates body→world),
    // so a ray in the desired (stable) camera maps to the source (actual) camera by
    //   ray_actual = actual⁻¹ · desired · ray_desired   →   R = actual⁻¹ · desired.
    // Using actual·desired⁻¹ (its near-opposite for small angles) warps the image WITH
    // the camera shake instead of against it — confirmed visually: the frame followed
    // the camera and never stabilized.
    Quat corrQ = (actual.conjugate() * desired).normalized();

    float R[3][3];
    QuatToMatrix(corrQ, R);

    // H = K · R · K⁻¹  (explicit expansion, no intermediate allocations)
    // Row 0 of H:
    float h00 = fx * R[0][0] + cx * R[2][0];
    float h01 = fx * R[0][1] + cx * R[2][1];
    float h02 = fx * R[0][2] + cx * R[2][2];
    // Row 1:
    float h10 = fy * R[1][0] + cy * R[2][0];
    float h11 = fy * R[1][1] + cy * R[2][1];
    float h12 = fy * R[1][2] + cy * R[2][2];
    // Row 2 = Row 2 of R:
    float h20 = R[2][0];
    float h21 = R[2][1];
    float h22 = R[2][2];

    // Now multiply on the right by K⁻¹:
    // K⁻¹ col0 = (1/fx, 0, 0), col1 = (0, 1/fy, 0), col2 = (-cx/fx, -cy/fy, 1)
    float ifx = 1.f/fx, ify = 1.f/fy;

    std::array<float,9> out;
    // col 0: multiply by (1/fx, 0, 0)
    out[0] = h00 * ifx;
    out[3] = h10 * ifx;
    out[6] = h20 * ifx;
    // col 1: multiply by (0, 1/fy, 0)
    out[1] = h01 * ify;
    out[4] = h11 * ify;
    out[7] = h21 * ify;
    // col 2: multiply by (-cx/fx, -cy/fy, 1)
    out[2] = h00*(-cx*ifx) + h01*(-cy*ify) + h02;
    out[5] = h10*(-cx*ifx) + h11*(-cy*ify) + h12;
    out[8] = h20*(-cx*ifx) + h21*(-cy*ify) + h22;

    return out;
}

// ── Adaptive zoom: crop needed to hide the exposed border at one instant ──────
// Map the four output corners through the (un-cropped) correction homography. Any
// corner whose source falls outside [0,w]×[0,h] exposes a black border on that
// side. The crop must shrink the sampled region symmetrically about the centre, so
// to hide a border of fraction f we lose 2f total → crop = 1 − 2·f. We evaluate the
// top corners at the top scanline's exposure time and the bottom corners at the
// bottom's, so rolling-shutter skew is accounted for too. Returns 1.0 (no zoom) when
// the frame is already fully covered.
float GyroIntegrator::RequiredCrop(double timeSec, const Quat& desired, double rsDuration,
                                   float fx, float fy, float cx, float cy,
                                   int renderWidth, int renderHeight) const
{
    const float w = static_cast<float>(renderWidth);
    const float h = static_cast<float>(renderHeight);

    auto Htop = CorrectionToHomography(OrientationAt(timeSec - 0.5 * rsDuration),
                                       desired, fx, fy, cx, cy);
    auto Hbot = CorrectionToHomography(OrientationAt(timeSec + 0.5 * rsDuration),
                                       desired, fx, fy, cx, cy);

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    auto acc = [&](const std::array<float,9>& H, float ox, float oy) {
        float sw = H[6]*ox + H[7]*oy + H[8];
        if (std::fabs(sw) < 1e-9f) return;
        float sx = (H[0]*ox + H[1]*oy + H[2]) / sw;
        float sy = (H[3]*ox + H[4]*oy + H[5]) / sw;
        minX = std::min(minX, sx); maxX = std::max(maxX, sx);
        minY = std::min(minY, sy); maxY = std::max(maxY, sy);
    };
    acc(Htop, 0.f, 0.f); acc(Htop, w, 0.f);   // top edge sampled at top exposure time
    acc(Hbot, 0.f, h);   acc(Hbot, w, h);     // bottom edge at bottom exposure time

    float fX = std::max({0.f, -minX / w, (maxX - w) / w});
    float fY = std::max({0.f, -minY / h, (maxY - h) / h});
    float f  = std::max(fX, fY);
    return 1.0f - 2.0f * f;
}

// ── Horizon lock ──────────────────────────────────────────────────────────────
// Level the horizon by rotating ONLY about the optical axis (roll) so the camera's
// pitch/yaw — where it is actually pointing — is preserved. (The old version rebuilt a
// whole canonical orientation, which also changed pitch/yaw and looked like scaling.)
//
// q is world-from-body; columns of R are the body axes (x=right, y=down, z=optical) in
// world. World gravity is +Y (verified via the accelerometer). We rotate the camera
// about its optical axis until its "down" axis lines up with gravity projected into the
// image plane → the horizon is level, view direction unchanged.
static Quat RemoveRoll(const Quat& q) noexcept
{
    float R[3][3];
    QuatToMatrix(q, R);

    // Keep the optical axis (forward) exactly; rebuild right/down so the horizon is level.
    float f[3] = { R[0][2], R[1][2], R[2][2] };          // forward (body z) in world
    float fl = std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    if (fl < 1e-6f) return q; f[0]/=fl; f[1]/=fl; f[2]/=fl;

    const float grav[3] = { 0.f, 1.f, 0.f };             // world gravity (+Y)

    // Down (body y) = gravity projected perpendicular to the optical axis.
    float gf = grav[0]*f[0] + grav[1]*f[1] + grav[2]*f[2];
    float d[3] = { grav[0]-gf*f[0], grav[1]-gf*f[1], grav[2]-gf*f[2] };
    float dl = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if (dl < 1e-4f) return q;                            // looking along gravity: undefined
    d[0]/=dl; d[1]/=dl; d[2]/=dl;

    // Right (body x) = down × forward → guaranteed perpendicular to gravity (level horizon),
    // and right-handed so [right | down | forward] is a valid rotation.
    float r[3] = { d[1]*f[2]-d[2]*f[1], d[2]*f[0]-d[0]*f[2], d[0]*f[1]-d[1]*f[0] };

    // Leveled rotation: columns are the body axes (right, down, forward) in world.
    float Rl[3][3] = {
        { r[0], d[0], f[0] },
        { r[1], d[1], f[1] },
        { r[2], d[2], f[2] }
    };

    // Rotation matrix → quaternion (Shepperd's method).
    float tr = Rl[0][0] + Rl[1][1] + Rl[2][2];
    Quat o;
    if (tr > 0.f) {
        float s = 0.5f / std::sqrt(tr + 1.f);
        o.w = 0.25f / s; o.x = (Rl[2][1]-Rl[1][2])*s; o.y = (Rl[0][2]-Rl[2][0])*s; o.z = (Rl[1][0]-Rl[0][1])*s;
    } else if (Rl[0][0] > Rl[1][1] && Rl[0][0] > Rl[2][2]) {
        float s = 2.f*std::sqrt(1.f+Rl[0][0]-Rl[1][1]-Rl[2][2]);
        o.w = (Rl[2][1]-Rl[1][2])/s; o.x = 0.25f*s; o.y = (Rl[0][1]+Rl[1][0])/s; o.z = (Rl[0][2]+Rl[2][0])/s;
    } else if (Rl[1][1] > Rl[2][2]) {
        float s = 2.f*std::sqrt(1.f+Rl[1][1]-Rl[0][0]-Rl[2][2]);
        o.w = (Rl[0][2]-Rl[2][0])/s; o.x = (Rl[0][1]+Rl[1][0])/s; o.y = 0.25f*s; o.z = (Rl[1][2]+Rl[2][1])/s;
    } else {
        float s = 2.f*std::sqrt(1.f+Rl[2][2]-Rl[0][0]-Rl[1][1]);
        o.w = (Rl[1][0]-Rl[0][1])/s; o.x = (Rl[0][2]+Rl[2][0])/s; o.y = (Rl[1][2]+Rl[2][1])/s; o.z = 0.25f*s;
    }
    return o.normalized();
}

// ── Public: compute full WarpMap for one frame ───────────────────────────────
WarpMap GyroIntegrator::ComputeWarpMap(
    double frameCenterTimeSec,
    const StabilizationParams& params,
    int renderWidth,
    int renderHeight) const
{
    if (renderWidth <= 0)  renderWidth  = mFrameWidth;
    if (renderHeight <= 0) renderHeight = mFrameHeight;
    if (mOrientations.empty())
        return WarpMap::Identity(renderWidth, renderHeight);

    // Off-speed/conformed footage: the render asks for a PLAYBACK time but the gyro is
    // indexed in CAPTURE time. Map render time → gyro time (scale ≈ 1 for normal speed,
    // 0.5 for 48→24 fps, etc.) so the warp aligns with the frame on screen. Applied to
    // both the per-frame lookup and the crop-scan range so they stay in gyro time.
    const double gscale = (params.gyroTimeScale > 1e-6) ? params.gyroTimeScale : 1.0;
    frameCenterTimeSec *= gscale;
    const double clipInGyro  = params.clipInSec  * gscale;
    const double clipOutGyro = params.clipOutSec * gscale;

    // The render buffer may be at a reduced preview resolution (e.g. 1/16); the
    // homography is in PIXEL units, so all intrinsics must be at the render scale —
    // otherwise full-res translations get applied to a tiny buffer and the image
    // flies off-screen.
    const float sx = static_cast<float>(renderWidth)  / static_cast<float>(mFrameWidth);
    const float sy = static_cast<float>(renderHeight) / static_cast<float>(mFrameHeight);

    // Camera intrinsics (full-res), then scaled to the render resolution.
    float fxFull, fyFull;
    if (params.photositePitchUm > 0.f) {
        fxFull = fyFull = params.focalLengthMM * 1000.f / params.photositePitchUm;
    } else {
        fxFull = params.focalLengthMM * static_cast<float>(mFrameWidth)  / params.sensorWidthMM;
        fyFull = params.focalLengthMM * static_cast<float>(mFrameHeight) / params.sensorHeightMM;
    }
    float fx = fxFull * sx;
    float fy = fyFull * sy;
    float cx = static_cast<float>(renderWidth)  * 0.5f;
    float cy = static_cast<float>(renderHeight) * 0.5f;

    {   // One-time: verify the intrinsics. A wrong fx under/over-corrects every frame
        // and makes stabilization look weak no matter the sync. For a full-frame 6K
        // (6048 px / ~36 mm → pitch ~5.95 µm) at 48 mm, fxFull should be ~8000.
        static bool once = false;
        if (!once) { once = true;
            FILE* f = dbg::open();
            if (f) { std::fprintf(f,
                "INTR focal=%.1f pitch=%.3f fxFull=%.1f fyFull=%.1f | render %dx%d frame %dx%d → fx=%.1f fy=%.1f\n",
                params.focalLengthMM, params.photositePitchUm, fxFull, fyFull,
                renderWidth, renderHeight, mFrameWidth, mFrameHeight, fx, fy); std::fclose(f); }
        }
    }

    // Desired (smooth, optionally horizon-leveled) orientation at any time. Used for this
    // frame AND for the zoom window samples — each window sample must use the desired at
    // ITS OWN time. (Using the frame's fixed desired for far samples made the zoom measure
    // the camera's 0.6s motion instead of the per-frame border → over-crop at low smoothing
    // and jumpy zoom.)
    auto desiredAt = [&](double t) -> Quat {
        Quat dq = SmoothedOrientation(t, params.smoothingWindowSec);
        if (params.horizonStrength > 0.f) {
            Quat leveled = RemoveRoll(dq);
            // Optional tilt: roll the leveled horizon by the target angle about the optical
            // axis (so the lock holds a chosen tilt instead of dead level).
            if (params.horizonTiltRad != 0.f) {
                Quat rz{ std::cos(params.horizonTiltRad * 0.5f), 0.f, 0.f,
                         std::sin(params.horizonTiltRad * 0.5f) };
                leveled = (leveled * rz).normalized();
            }
            dq = slerp(dq, leveled, std::min(params.horizonStrength, 1.f));
        }
        return dq;
    };
    Quat desired = desiredAt(frameCenterTimeSec);

    {   // Diagnostic: per-frame correction magnitude (deg) + dominant axis. Logs only
        // when the frame time changes (so playback shows distinct frames, not a paused one).
        static double sLastT = -1e9; static int sDbg = 0;
        if (std::fabs(frameCenterTimeSec - sLastT) > 1e-4 && sDbg < 40) {
            ++sDbg; sLastT = frameCenterTimeSec;
            Quat act = OrientationAt(frameCenterTimeSec);
            Quat cq  = (act.conjugate() * desired).normalized();
            float ang = 2.f*std::acos(std::min(1.f,std::fabs(cq.w)))*180.f/3.14159265f;
            FILE* f = dbg::open();
            if (f) { std::fprintf(f,"CORR t=%.3f corrAngle=%.4f deg axis=[%.3f %.3f %.3f]\n",
                                  frameCenterTimeSec, ang, cq.x, cq.y, cq.z);
                     std::fclose(f); }
        }
    }

    // Rolling shutter duration in seconds
    double rsDuration = static_cast<double>(params.rollingShutterMs) * 1e-3;

    // Note: the stabilization correction is always applied in FULL — we do NOT pull the
    // desired path back toward actual to fit the crop (that made the correction uneven
    // frame-to-frame and gutted stabilization). Borders the correction exposes are hidden
    // by the adaptive zoom below; only motion beyond the hard zoom cap shows any border.

    // Crop/zoom-to-fill: H maps output→source, so to hide the borders the
    // stabilization exposes we pre-scale output coords about the centre by `crop`:
    //   src = correct( center + crop·(out − center) )
    //
    // The zoom is ADAPTIVE to motion rather than a fixed factor: per frame we measure
    // exactly how much border the correction exposes (RequiredCrop) and zoom only that
    // much — so still shots stay at 1.0 (no zoom / full sharpness) and only violent
    // moments crop in. To avoid the zoom "pumping" frame-to-frame we take the worst
    // (smallest) required crop over the clip, giving one steady envelope.
    float crop = 1.0f;
    if (params.zoomToFill) {
        // CONSTANT clip-wide zoom (DaVinci-style): the crop is the deepest zoom ANY frame in
        // the clip needs, applied to every frame. A per-frame adaptive crop "breathes" with
        // micro-shake (the jitter the user saw only with Zoom to Fill on); a single constant
        // crop never changes → zero breathing. Computed once and cached; recomputed only when
        // smoothing / horizon / in-out change. Crop is a resolution-independent fraction, so
        // caching across render resolutions is fine.
        const float  hardMax   = 0.5f;      // never crop below 0.5 (2× max zoom)
        const float  scanFocal = std::max(1.0f, params.focalLengthMM);

        // Only consider the trimmed in→out range (+ a small handle for the smoothing window),
        // so motion outside the used part of the clip doesn't inflate the zoom.
        const double handle = 0.5;
        const double t0 = std::max(mTimestamps.front(), clipInGyro  - handle);
        const double t1 = std::min(mTimestamps.back(),  clipOutGyro + handle);

        std::lock_guard<std::mutex> lk(mCropMutex);
        const bool match = mCropValid
            && mKeySmooth  == params.smoothingWindowSec
            && mKeyHorizon == params.horizonStrength
            && mKeyIn      == t0
            && mKeyOut     == t1;
        if (!match) {
            float minR = 1.0f;
            // Sample the range but CAP at ~120 points, so the scan cost is bounded no matter
            // how long the clip is (120 points still catches the motion peaks).
            const double span = std::max(0.0, t1 - t0);
            const double cropStep = std::max(0.2, span / 120.0);
            for (double ts = t0; ts <= t1; ts += cropStep) {
                float r = RequiredCrop(ts, desiredAt(ts), rsDuration, fx, fy, cx, cy,
                                       renderWidth, renderHeight);
                minR = std::min(minR, r);
            }
            // Worst exposed-border DEPTH per mm of focal. Border ∝ focal, so dividing out the
            // scan focal makes this focal-INDEPENDENT (no need to re-scan when the zoom moves).
            mCachedDepthPerFocal = (1.0f - minR) / scanFocal;
            mCropValid  = true;
            mKeySmooth  = params.smoothingWindowSec;
            mKeyHorizon = params.horizonStrength;
            mKeyIn      = t0;
            mKeyOut     = t1;
        }
        // Extra Scale scales the border DEPTH, so the added zoom is proportional to how much
        // we're already cropping — largest at the deepest-crop (longest-focal / worst) frames,
        // ~nothing where the crop is shallow — instead of a flat pad on every frame.
        const float depthEff = mCachedDepthPerFocal * (1.0f + std::max(0.0f, params.extraCropFrac));

        // Two crops: ADAPTIVE follows this frame's focal (the scale tracks the lens); CONSTANT
        // uses the clip's longest focal so it never changes (steady, but over-zooms the wide
        // end). "Scale Follows Zoom" blends them via zoomSteadiness (only differs on a zoom
        // lens, where focal varies): 0 = adaptive, 1 = constant.
        const float maxFocal = std::max(params.maxFocalLengthMM, params.focalLengthMM);
        const float adaptiveCrop = 1.0f - depthEff * params.focalLengthMM;
        const float constantCrop = 1.0f - depthEff * maxFocal;
        const float s = std::min(1.0f, std::max(0.0f, params.zoomSteadiness));
        crop = adaptiveCrop * (1.0f - s) + constantCrop * s;
        crop = std::min(1.0f, std::max(hardMax, crop));
    }
    else if (params.extraCropFrac > 0.f) {
        // Scale to Fill is off, but Extra Scale still applies as a plain manual zoom (so the
        // control is always meaningful — no need to grey it out).
        crop = std::max(0.5f, 1.0f - params.extraCropFrac);
    }

    {   // Diagnostic: log the computed crop per frame (time-change gated) to see whether
        // the "zoom jitter" is the crop pumping vs. a residual-stabilization/focal issue.
        static double sLastTc = -1e9; static int sCdbg = 0;
        if (std::fabs(frameCenterTimeSec - sLastTc) > 1e-4 && sCdbg < 80) {
            ++sCdbg; sLastTc = frameCenterTimeSec;
            FILE* f = dbg::open();
            if (f) { std::fprintf(f, "CROP t=%.3f crop=%.4f focal=%.1f horizonStr=%.3f\n",
                                  frameCenterTimeSec, crop, params.focalLengthMM,
                                  params.horizonStrength); std::fclose(f); }
        }
    }

    WarpMap wm;
    wm.width  = renderWidth;
    wm.height = renderHeight;
    wm.rowHomographies.resize(static_cast<size_t>(renderHeight) * 9);

    // Rolling-shutter skew varies SMOOTHLY top→bottom, so we don't need a trig-heavy
    // homography per render row (up to thousands). Compute it at a small number of evenly-
    // spaced BANDS (the expensive OrientationAt + CorrectionToHomography), then fill every
    // render row by cheap linear interpolation between bands. ~96 bands keeps the RS skew
    // step well below a pixel even at full res; the per-frame trig cost drops from O(rows)
    // to O(96). Render heights ≤ bands fall back to exactly one band per row (no loss).
    const int numBands = std::min(renderHeight, 96);
    const float tx = (1.0f - crop) * cx;     // central zoom (constant for the whole frame)
    const float ty = (1.0f - crop) * cy;
    std::vector<std::array<float,9>> bands(static_cast<size_t>(numBands));
    for (int b = 0; b < numBands; ++b) {
        double repRow  = (b + 0.5) * renderHeight / numBands;     // band's representative row
        double rowFrac = (repRow / renderHeight) - 0.5;
        double rowTime = frameCenterTimeSec + rowFrac * rsDuration;

        auto H = CorrectionToHomography(OrientationAt(rowTime), desired, fx, fy, cx, cy);
        if (crop != 1.0f) {                  // fold in the zoom: H · scale-about-centre
            std::array<float,9> Z = { crop, 0.f, tx,  0.f, crop, ty,  0.f, 0.f, 1.f };
            std::array<float,9> M;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    M[r*3+c] = H[r*3+0]*Z[0*3+c] + H[r*3+1]*Z[1*3+c] + H[r*3+2]*Z[2*3+c];
            H = M;
        }
        bands[b] = H;
    }

    for (int row = 0; row < renderHeight; ++row) {
        float fb = (row + 0.5f) * numBands / renderHeight - 0.5f;   // row → band coordinate
        fb = std::max(0.f, std::min((float)(numBands - 1), fb));
        int   b0 = (int)fb;
        int   b1 = std::min(b0 + 1, numBands - 1);
        float t  = fb - b0;
        float* out = &wm.rowHomographies[static_cast<size_t>(row) * 9];
        for (int k = 0; k < 9; ++k)
            out[k] = bands[b0][k] * (1.f - t) + bands[b1][k] * t;
    }

    wm.valid = true;
    return wm;
}
