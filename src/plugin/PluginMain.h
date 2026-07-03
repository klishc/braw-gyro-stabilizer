// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// PluginMain.h
#pragma once

// Core Premiere SDK headers — these exist in the 26.0 SDK without AE deps
#include "PrSDKTypes.h"
#include "PrSDKEntry.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKStructs.h"

// AE-compatible effect SDK. The PF_InData / PF_OutData / PF_ParamDef / PF_LayerDef
// structs, the PF_Cmd_* selectors, the PF_ADD_* parameter macros and the suite
// definitions all come from the real After Effects SDK headers. The AE SDK is wired
// onto the include path by CMake (see AE_SDK in CMakeLists.txt) — these must be the
// genuine headers, since their struct layouts are the binary ABI the Premiere host
// fills in. Do NOT substitute a hand-written stub. Include order mirrors Adobe's own
// GPUVideoFilter sample (Vignette.h).
#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"   // PF_ParamUtilsSuite3 (PF_UpdateParamUI) for the Status row
#include "PrSDKAESupport.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "SPBasic.h"           // full SPBasicSuite (AE_Effect.h only forward-declares it)

#define kMatchName  "io.nk.brawgyrostabilizer"
#define kEffectName "BRAW Gyro Stabilizer"

// CRITICAL: this enum is the SINGLE source of truth for param indices, used by BOTH
// the AE PF_ADD_* order in ParamsSetup AND the GPU path's VideoSegmentSuite::GetParam.
// Index 0 is the input layer (Adobe convention, see Vignette's VIG_INPUT=0); params
// follow in PF_ADD order. Reordering is OK *only* if the PF_ADD order in ParamsSetup is
// changed to match exactly — the GPU reads by these enum values, so the two MUST agree.
// (A mismatch silently misreads every value: crop still runs but rotation is garbage.)
enum {
    kParamStatus            = 1,   // top-level STATUS row (the no-gyro notification): its label
                                   // shows the camera / "No gyro data in clip"; button refreshes
    kParamSmoothingStrength = 2,   // Smoothing %, mapped non-linearly to a seconds-sigma
    kParamFocalLength       = 3,   // focal length override (mm), 0 = auto — important, near top
    kParamHorizonGroup      = 4,   // "Horizon" group header (topic; UI-only, occupies an index)
    kParamHorizonAmount     = 5,   // horizon leveling %, 0 = off
    kParamHorizonTilt       = 6,   // target horizon angle (degrees); 0 = level
    kParamHorizonGroupEnd   = 7,   // group end (UI-only, occupies an index)
    kParamScaleGroup        = 8,   // "Scale" group header (topic; UI-only)
    kParamScaleToFill       = 9,   // "Scale to Fill" — digital crop is computed automatically
    kParamScaleFollowsZoom  = 10,  // "Scale Follows Zoom %": digital zoom-MOTION easing (0 = off)
    kParamExtraScale        = 11,  // "Extra Scale %" — manual digital zoom on top of the auto-crop
    kParamScaleGroupEnd     = 12,  // group end (UI-only)
    kParamDebugGroup        = 13,  // "Debug" group header (topic, ships COLLAPSED; UI-only)
    kParamDebugLog          = 14,  // write /tmp/brawgyro.log when on (off = no logging)
    kParamRenderPath        = 15,  // "Check" button; label shows GPU vs CPU render path (UI-only)
    kParamCheckUpdate       = 16,  // "Check" button; queries GitHub for a newer release (UI-only;
                                   // network ONLY on click, async — never blocks the UI thread)
    kParamDebugGroupEnd     = 17,  // group end (UI-only)
    kParamSaveDefault       = 18,  // "Save settings" button; writes current values to defaults.ini (UI-only)
    // NOTE: the Rolling Shutter Override param was removed from the UI — the override now
    // lives only in defaults.ini ("rolling_shutter", ms, 0 = auto) and is applied inside
    // BRAWReader::GetRollingShutterMs().
};

// Longest status text fits PF_MAX_EFFECT_PARAM_NAME_LEN (31). Keep messages short.
static constexpr char kStatusDefault[]      = "Press Check to detect file";
static constexpr char kRenderPathDefault[]  = "Render: press Check";
static constexpr char kCheckUpdateDefault[] = "Updates: press Check";
// Plugin version, compared against the newest GitHub release tag by the update check.
// Keep in sync with CMakeLists project(VERSION) / package.sh VERSION.
static constexpr char kPluginVersionStr[]   = "1.1.0";
// Smoothing slider (%) → Gaussian sigma in seconds. Square curve so the perceptually busy
// low end is spread across the slider. Max sigma 1.25s at 100% — the old 5s top turned a
// deliberate camera move into "shake" and demanded huge crops; the usable range in practice
// was the old 0–50, which is now the full dial.
inline float SmoothingPctToSigma(float pct) {
    float p = (pct < 0.f ? 0.f : (pct > 100.f ? 100.f : pct)) / 100.f;
    return 1.25f * p * p;
}

static constexpr float kDefaultSmoothing      = 50.0f;  // % (→ ~0.31s sigma)
static constexpr float kDefaultRollingShutter = 0.0f;   // ms, 0 = auto (BRAW metadata)
static constexpr int   kDefaultScaleToFill    = 1;
static constexpr float kDefaultFocalLength    = 0.0f;   // mm, 0 = auto (read from clip)
static constexpr float kDefaultHorizonAmount  = 0.0f;   // horizon leveling %, 0 = off
static constexpr float kDefaultHorizonTilt    = 0.0f;   // target horizon angle (degrees)
static constexpr float kDefaultScaleFollowsZoom = 0.0f;  // % — zoom-motion easing OFF by default (~25 is a good working value)
static constexpr float kDefaultExtraScale     = 0.0f;   // % extra digital zoom on top of the auto-crop
static constexpr int   kDefaultDebugLog       = 0;      // debug logging off by default

extern "C" {
    DllExport PREMPLUGENTRY EffectMain(
        csSDK_int32     selector,
        PF_InData*      in_data,
        PF_OutData*     out_data,
        PF_ParamDef*    params[],
        PF_LayerDef*    output,
        void*           extra);
}
