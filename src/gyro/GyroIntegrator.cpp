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

    // ── Anchor the world frame to TRUE gravity ────────────────────────────────
    // Integration starts at identity, so the "world" above is just the camera's pose at
    // the FIRST sample — and Horizon Lock levels to that frame's +Y. If the clip starts
    // with the camera rolled (e.g. a gimbal already mid-move), the "leveled" horizon
    // inherits that starting tilt for the whole clip. Fix: rotate every accelerometer
    // sample into the provisional world and AVERAGE — motion acceleration mostly cancels
    // over the clip while gravity doesn't — then rotate the whole orientation track so
    // mean gravity lies exactly on +Y (the axis RemoveRoll levels against, matching the
    // previously-verified convention for clips that start level). Stabilization is
    // unaffected: its corrections are RELATIVE rotations, so a global re-anchor cancels.
    {
        double gw[3] = {0,0,0};
        size_t used = 0;
        for (size_t i = 0; i < N; ++i) {
            const float ax = S[i].ax, ay = S[i].ay, az = S[i].az;
            if (ax*ax + ay*ay + az*az < 1e-6f) continue;       // accel stream missing/empty
            Quat p{0, ax, ay, az};
            Quat r = (mOrientations[i] * p) * mOrientations[i].conjugate();
            gw[0] += r.x; gw[1] += r.y; gw[2] += r.z; ++used;
        }
        const double len = std::sqrt(gw[0]*gw[0] + gw[1]*gw[1] + gw[2]*gw[2]);
        if (used > N / 4 && len > 1e-6) {
            gw[0] /= len; gw[1] /= len; gw[2] /= len;
            const double d = gw[1];                            // gw · (0,1,0)
            Quat A{1,0,0,0};
            if (d < -0.99999) {
                A = Quat{0, 1, 0, 0};                          // upside-down start: 180° about X
            } else if (d < 0.99999) {
                double axv[3] = { -gw[2], 0.0, gw[0] };        // gw × (0,1,0)
                const double al = std::sqrt(axv[0]*axv[0] + axv[1]*axv[1] + axv[2]*axv[2]);
                if (al > 1e-9) {
                    const double ang = std::acos(std::max(-1.0, std::min(1.0, d)));
                    const double s   = std::sin(ang * 0.5) / al;
                    A = Quat{ (float)std::cos(ang * 0.5),
                              (float)(axv[0] * s), (float)(axv[1] * s), (float)(axv[2] * s) };
                }
            }
            for (size_t i = 0; i < N; ++i)
                mOrientations[i] = (A * mOrientations[i]).normalized();
        }
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
    float dbgRoll = 0.f, dbgPyMm = 0.f;   // diagnostics for the CROP log line below
    if (params.zoomToFill) {
        // LOCAL zoom-to-fill with plateaus. The old clip-wide constant crop (worst case
        // applied to every frame) over-cropped calm sections badly, and its sparse 0.2s scan
        // MISSED 1–3-frame whips (little black corners on hits). Pipeline:
        //   (1) scan the required border DENSELY (~24/s, catches single-frame spikes),
        //   (2) slope-limit into a peak envelope (crop moves at most ~2%/s),
        //   (3) hysteresis: HOLD the crop flat through sub-1% wiggles (micro-shake made a
        //       tracking crop pump visibly on calm shots, worst at low Smoothing), releasing
        //       only when there is real frame area to win,
        //   (4) light smoothing to round the plateau corners, (5) re-assert coverage.
        //   Per frame, interpolate the cached track. Depth is stored per mm of focal
        //   (border ∝ focal), so the track is focal-independent — no re-scan on zooms.
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
            && mKeyTilt    == params.horizonTiltRad
            && mKeyIn      == t0
            && mKeyOut     == t1;
        if (!match) {
            // Dense scan: ~24 samples/s (fine enough for a 1-frame whip at 24fps), capped at
            // 600 samples so the rebuild stays fast when scrubbing the Smoothing slider —
            // clips up to ~25s get full per-frame density; longer ones degrade gracefully.
            const double span = std::max(0.0, t1 - t0);
            const double cropStep = std::max(0.042, span / 600.0);
            // (1) dense required border, SPLIT into a roll part (in-plane rotation border —
            //     focal-INDEPENDENT; this is what Horizon Lock produces) and the pitch/yaw
            //     remainder (border ∝ focal). A single per-mm model reconstructed the roll
            //     border ∝ focal too, under-cropping the wide end of zoom clips → the black
            //     corners seen with Horizon Lock 100 on a 24–70 clip.
            std::vector<double> tt;
            std::vector<float>  reqRoll, reqPy;
            tt.reserve((size_t)(span / cropStep) + 2);
            reqRoll.reserve(tt.capacity());  reqPy.reserve(tt.capacity());
            const float W = (float)std::max(1, renderWidth), Hh = (float)std::max(1, renderHeight);
            for (double ts = t0; ts <= t1; ts += cropStep) {
                const Quat dq   = desiredAt(ts);
                const float full = 1.0f - RequiredCrop(ts, dq, rsDuration, fx, fy, cx, cy,
                                                       renderWidth, renderHeight);
                // Roll (twist about the optical axis) of the correction, and the border a
                // pure central rotation by that angle exposes (largest same-aspect rect).
                const Quat corr = (OrientationAt(ts).conjugate() * dq).normalized();
                const float tw  = 2.0f * std::atan2(corr.z, corr.w);
                const float ca  = std::fabs(std::cos(tw)), sa = std::fabs(std::sin(tw));
                const float sRoll = std::min(1.0f, std::min(1.0f / (ca + sa * Hh / W),
                                                            1.0f / (sa * W / Hh + ca)));
                const float rollF = std::min(full, 1.0f - sRoll);
                tt.push_back(ts);
                reqRoll.push_back(rollF);
                reqPy.push_back(std::max(0.0f, full - rollF));
            }
            const size_t N = tt.size();
            // (2)-(5) shape each component: slope-limited peak envelope (crop moves ≤ `rate`
            // of the frame per second) → HYSTERESIS plateaus (hold flat through sub-`hys`
            // wiggles — micro-shake made a tracking crop pump visibly on calm footage, worst
            // at low Smoothing; release only when real frame area is recoverable) → light
            // triangular smoothing to round plateau corners → re-assert coverage.
            const float rate = 0.02f;                     // frame-fraction per second
            const float hys  = 0.010f;                    // 1% of frame dead-band
            const int   hw   = std::max(1, (int)std::lround(0.3 / cropStep));
            auto shape = [&](std::vector<float>& v) {
                const std::vector<float> orig = v;
                for (size_t i = 1; i < N; ++i) {
                    const float dt = (float)(tt[i] - tt[i-1]);
                    v[i] = std::max(v[i], v[i-1] - rate * dt);
                }
                for (size_t i = N; i-- > 1; ) {
                    const float dt = (float)(tt[i] - tt[i-1]);
                    v[i-1] = std::max(v[i-1], v[i] - rate * dt);
                }
                std::vector<float> flat(N);
                float P = (N > 0) ? v[0] : 0.f;
                for (size_t i = 0; i < N; ++i) {
                    const float dt = (i > 0) ? (float)(tt[i] - tt[i-1]) : 0.f;
                    if (v[i] >= P)            P = v[i];
                    else if (P - v[i] > hys)  P = std::max(v[i] + 0.5f * hys, P - rate * dt);
                    flat[i] = P;
                }
                for (size_t i = 0; i < N; ++i) {
                    double s = 0.0, w = 0.0;
                    for (int d = -hw; d <= hw; ++d) {
                        const long k = (long)i + d;
                        if (k < 0 || k >= (long)N) continue;
                        const double ww = 1.0 - (double)std::abs(d) / (hw + 1);
                        s += ww * flat[(size_t)k]; w += ww;
                    }
                    v[i] = std::max(orig[i], (float)(w > 0.0 ? s / w : flat[i]));
                }
            };
            shape(reqRoll);
            shape(reqPy);
            mCropT    = std::move(tt);
            mCropRoll = std::move(reqRoll);
            mCropPyPerMm.assign(N, 0.f);
            for (size_t i = 0; i < N; ++i) mCropPyPerMm[i] = reqPy[i] / scanFocal;
            mCropValid  = true;
            mKeySmooth  = params.smoothingWindowSec;
            mKeyHorizon = params.horizonStrength;
            mKeyTilt    = params.horizonTiltRad;
            mKeyIn      = t0;
            mKeyOut     = t1;
        }
        // Interpolate both components at this frame's (gyro-time) position and recombine:
        // roll border is focal-independent; pitch/yaw border scales with the APPARENT
        // (eased) focal — the eased focal is smooth and >= raw, so borders stay hidden
        // while the crop can't inherit the lens's hard stops.
        float rollFrac = 0.f, pyPerMm = 0.f;
        if (!mCropT.empty()) {
            const auto& T = mCropT;
            size_t lo = 0; float a = 0.f;
            if      (frameCenterTimeSec <= T.front()) { lo = 0;            a = 0.f; }
            else if (frameCenterTimeSec >= T.back())  { lo = T.size() - 1; a = 0.f; }
            else {
                size_t hi = T.size() - 1;
                while (hi - lo > 1) { size_t m = (lo + hi) / 2;
                                      if (T[m] <= frameCenterTimeSec) lo = m; else hi = m; }
                const double sp = T[lo+1] - T[lo];
                a = (sp > 1e-9) ? (float)((frameCenterTimeSec - T[lo]) / sp) : 0.f;
            }
            const size_t hi2 = std::min(lo + 1, T.size() - 1);
            rollFrac = mCropRoll[lo]    + a * (mCropRoll[hi2]    - mCropRoll[lo]);
            pyPerMm  = mCropPyPerMm[lo] + a * (mCropPyPerMm[hi2] - mCropPyPerMm[lo]);
        }
        dbgRoll = rollFrac;  dbgPyMm = pyPerMm;
        const float easeS = std::max(1.0f, params.zoomEaseScale);
        float frac = rollFrac + pyPerMm * (params.focalLengthMM * easeS);
        // Extra Scale scales the border DEPTH, so the added zoom is proportional to how much
        // we're already cropping — largest at the deepest-crop (worst) moments, ~nothing where
        // the crop is shallow — instead of a flat pad on every frame.
        frac *= (1.0f + std::max(0.0f, params.extraCropFrac));
        crop  = 1.0f - frac;
        crop  = std::min(1.0f, std::max(hardMax, crop));
    }
    else if (params.extraCropFrac > 0.f) {
        // Scale to Fill is off, but Extra Scale still applies as a plain manual zoom (so the
        // control is always meaningful — no need to grey it out).
        crop = std::max(0.5f, 1.0f - params.extraCropFrac);
    }

    // Zoom-motion easing ("Scale Follows Zoom"): apply EXACTLY the per-frame digital zoom that
    // makes the apparent focal follow the smooth upper envelope of the lens focal — rounding
    // the lens's hard zoom start/stop while preserving the zoom itself. The ease is >= 1 and
    // returns to exactly 1 in flat regions (no cushion), so smooth/slow zooms without hard
    // stops get essentially nothing.
    //
    // Deliberately NO budget cap or soft-knee here: any limiter breaks the apparent-zoom
    // velocity mid-move (the "zooms, stalls, zooms again" pulse). The stabilizer's own 2x
    // (hardMax=0.5) floor applies to the stabilization crop only, NOT to this ease — a crash
    // zoom may momentarily crop past 2x, costing resolution on those few motion-blurred frames,
    // which is the honest price of smoothing it. A 4x floor remains as pure sanity against
    // pathological focal metadata (normal zooms never reach it).
    if (params.zoomEaseScale > 1.0001f) {
        crop = std::max(0.25f, crop / params.zoomEaseScale);
    }

    if (FILE* f = dbg::open()) {   // only when Debug Logging is ON (so the budget below isn't
        // spent while logging is off). Per-frame crop/focal/zoomEase, time-change gated.
        static double sLastTc = -1e9; static int sCdbg = 0;
        if (std::fabs(frameCenterTimeSec - sLastTc) > 1e-4 && sCdbg < 600) {
            ++sCdbg; sLastTc = frameCenterTimeSec;
            std::fprintf(f, "CROP t=%.3f crop=%.4f focal=%.1f zoomEase=%.4f horizonStr=%.3f"
                            " roll=%.4f pyMm=%.5f\n",
                         frameCenterTimeSec, crop, params.focalLengthMM,
                         params.zoomEaseScale, params.horizonStrength, dbgRoll, dbgPyMm);
        }
        std::fclose(f);
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
