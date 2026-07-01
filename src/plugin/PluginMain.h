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

#define kMatchName  "io.nk.GyroStabV4"
#define kEffectName "BRAW Gyro Stabilizer"

// CRITICAL: this enum is the SINGLE source of truth for param indices, used by BOTH
// the AE PF_ADD_* order in ParamsSetup AND the GPU path's VideoSegmentSuite::GetParam.
// Index 0 is the input layer (Adobe convention, see Vignette's VIG_INPUT=0); params
// follow in PF_ADD order. Reordering is OK *only* if the PF_ADD order in ParamsSetup is
// changed to match exactly — the GPU reads by these enum values, so the two MUST agree.
// (A mismatch silently misreads every value: crop still runs but rotation is garbage.)
enum {
    kParamStatus            = 1,   // "Check support" button; its label shows camera / unsupported (UI-only, GPU ignores)
    kParamSmoothingStrength = 2,   // Smoothing %, mapped non-linearly to a seconds-sigma
    kParamFocalLength       = 3,   // focal length override (mm), 0 = auto — important, near top
    kParamHorizonAmount     = 4,   // horizon leveling %, 0 = off
    kParamHorizonTilt       = 5,   // target horizon angle (degrees); 0 = level
    kParamScaleToFill       = 6,   // "Scale to Fill" — digital crop is computed automatically
    kParamScaleFollowsZoom  = 7,   // "Scale Follows Zoom %": 100 = scale tracks the lens zoom (adaptive), 0 = constant
    kParamExtraScale        = 8,   // "Extra Scale %" — manual digital zoom on top of the auto-crop
    kParamRollingShutter    = 9,   // rolling-shutter readout override (ms), 0 = auto
    kParamDebugLog          = 10,  // write /tmp/brawgyro.log when on (off = no logging)
    kParamRenderPath        = 11,  // "Check" button; label shows GPU vs CPU render path (UI-only)
};

// Longest status text fits PF_MAX_EFFECT_PARAM_NAME_LEN (31). Keep messages short.
static constexpr char kStatusDefault[]     = "Press Check to detect file";
static constexpr char kRenderPathDefault[] = "Render: press Check";
// Smoothing slider (%) → Gaussian sigma in seconds. Square curve so the perceptually busy
// low end is spread across the slider and 5–10s dead-zone is gone (max ~5s at 100%).
inline float SmoothingPctToSigma(float pct) {
    float p = (pct < 0.f ? 0.f : (pct > 100.f ? 100.f : pct)) / 100.f;
    return 5.0f * p * p;
}

static constexpr float kDefaultSmoothing      = 20.0f;  // % (→ ~0.2s sigma via the curve)
static constexpr float kDefaultRollingShutter = 0.0f;   // ms, 0 = auto (BRAW metadata)
static constexpr int   kDefaultScaleToFill    = 1;
static constexpr float kDefaultFocalLength    = 0.0f;   // mm, 0 = auto (read from clip)
static constexpr float kDefaultHorizonAmount  = 0.0f;   // horizon leveling %, 0 = off
static constexpr float kDefaultHorizonTilt    = 0.0f;   // target horizon angle (degrees)
static constexpr float kDefaultScaleFollowsZoom = 100.0f; // % — 100 = scale tracks the lens zoom (adaptive)
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
