// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// PluginMain.cpp
// Premiere Pro / After Effects SDK video filter plugin.
// Selector dispatch for the BRAW Gyro Stabilizer effect.

#include "PluginMain.h"
#include "../braw/BRAWReader.h"
#include "../gyro/GyroIntegrator.h"
#include "../warp/MetalWarpEngine.h"
#include "../common/DebugLog.h"
#include "../common/StatusStore.h"
#include "../common/BrawPath.h"

#include "PrSDKAESupport.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKVideoSegmentSuite.h"
#include "PrSDKVideoSegmentProperties.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKStringSuite.h"

#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <sys/stat.h>

// ── User default parameter values ("Save as default" button) ──────────────────
// A plain, hand-editable INI at ~/Library/Application Support/BRAWGyroStabilizer/
// defaults.ini. It's read in ParamsSetup so every NEWLY-applied instance starts from
// these values (missing keys fall back to the compiled kDefault* constants), and the
// "Save as default" button writes the current clip's values back to it. Existing clips
// are unaffected. Single active profile — for multiple named profiles use Premiere presets.
namespace {
struct ParamDefaults {
    float smoothing      = kDefaultSmoothing;
    float focal          = kDefaultFocalLength;
    float horizonAmt     = kDefaultHorizonAmount;
    float horizonTilt    = kDefaultHorizonTilt;
    int   scaleToFill    = kDefaultScaleToFill;
    float scaleFollows   = kDefaultScaleFollowsZoom;
    float extraScale     = kDefaultExtraScale;
    float rollingShutter = kDefaultRollingShutter;
    int   debugLog       = kDefaultDebugLog;
    float zoomSyncFrames = 1.2f;   // zoom-ease ↔ picture sync offset (frames); read hot by
                                   // BRAWReader::ZoomSyncOffsetFrames, preserved on save
};
std::string DefaultsDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Library/Application Support/BRAWGyroStabilizer";
}
std::string DefaultsFile() { return DefaultsDir() + "/defaults.ini"; }

void LoadDefaults(ParamDefaults& d) {
    FILE* f = std::fopen(DefaultsFile().c_str(), "r");
    if (!f) return;                                   // no file → compiled defaults
    auto clampf = [](double v, float lo, float hi) {
        if (!(v == v)) return lo;                     // NaN guard
        return (float)std::min((double)hi, std::max((double)lo, v));
    };
    char line[256], key[64];
    double val;
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, " %63[a-z_] = %lf", key, &val) != 2) continue;  // skips comments/blanks/junk
        std::string k = key;
        // Clamp every value to its parameter's range — a hand-edited (or corrupted) file
        // must never feed an out-of-range default into a slider.
        if      (k == "smoothing")          d.smoothing      = clampf(val,   0.f, 100.f);
        else if (k == "focal_override")     d.focal          = clampf(val,   0.f, 300.f);
        else if (k == "horizon_lock")       d.horizonAmt     = clampf(val,   0.f, 100.f);
        else if (k == "horizon_tilt")       d.horizonTilt    = clampf(val, -45.f,  45.f);
        else if (k == "scale_to_fill")      d.scaleToFill    = (val != 0.0) ? 1 : 0;
        else if (k == "scale_follows_zoom") d.scaleFollows   = clampf(val,   0.f, 100.f);
        else if (k == "extra_scale")        d.extraScale     = clampf(val,   0.f,  50.f);
        else if (k == "rolling_shutter")    d.rollingShutter = clampf(val,   0.f,  50.f);
        else if (k == "debug_log")          d.debugLog       = (val != 0.0) ? 1 : 0;
        else if (k == "zoom_sync_offset_frames") d.zoomSyncFrames = clampf(val, -10.f, 10.f);
    }
    std::fclose(f);
}
bool SaveDefaults(const ParamDefaults& d) {
    ::mkdir(DefaultsDir().c_str(), 0755);   // ~/Library/Application Support already exists
    FILE* f = std::fopen(DefaultsFile().c_str(), "w");
    if (!f) return false;
    std::fprintf(f,
        "# BRAW Gyro Stabilizer default parameter values.\n"
        "# Applied to each NEW effect instance. Edit by hand, or re-save from the effect.\n"
        "smoothing = %.1f\n"
        "focal_override = %.1f\n"
        "horizon_lock = %.1f\n"
        "horizon_tilt = %.2f\n"
        "scale_to_fill = %d\n"
        "scale_follows_zoom = %.1f\n"
        "extra_scale = %.1f\n"
        "rolling_shutter = %.2f\n"
        "debug_log = %d\n"
        "# Zoom-ease vs picture sync (frames): positive = focal metadata leads the picture.\n"
        "# Hot-reloaded — edit, save, and nudge any effect param to re-render. No restart.\n"
        "zoom_sync_offset_frames = %.1f\n",
        d.smoothing, d.focal, d.horizonAmt, d.horizonTilt, d.scaleToFill,
        d.scaleFollows, d.extraScale, d.rollingShutter, d.debugLog, d.zoomSyncFrames);
    std::fclose(f);
    return true;
}
// ── Update check (GitHub releases) ─────────────────────────────────────────────
// Runs ONLY when the user clicks the button — the plugin never phones home on its own.
// The fetch runs on a DETACHED thread (a synchronous request could hang Premiere's UI
// thread, and Premiere suites must not be touched off-thread), writing its result into
// gUpdLabel; RefreshStatusLabel pushes it into the button label on the next panel
// refresh / click. Uses macOS's bundled curl — no extra dependencies.
std::mutex  gUpdMutex;
std::string gUpdLabel;              // empty until a check has been started
bool        gUpdRunning = false;

bool ParseVer(const char* s, int v[3]) {
    while (*s && !std::isdigit((unsigned char)*s)) ++s;   // skip a leading "v"
    return std::sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

void StartUpdateCheck() {
    {
        std::lock_guard<std::mutex> lk(gUpdMutex);
        if (gUpdRunning) return;
        gUpdRunning = true;
        gUpdLabel   = "Checking...";
    }
    std::thread([] {
        std::string tag;
        FILE* p = ::popen("/usr/bin/curl -s --max-time 5 -H 'User-Agent: BRAWGyroStabilizer' "
                          "https://api.github.com/repos/klishc/braw-gyro-stabilizer/releases/latest",
                          "r");
        if (p) {
            char buf[512]; std::string out;
            while (std::fgets(buf, sizeof buf, p)) { out += buf; if (out.size() > 65536) break; }
            ::pclose(p);
            if (const char* c = std::strstr(out.c_str(), "\"tag_name\"")) {
                c = std::strchr(c + 10, ':');
                if (c) c = std::strchr(c, '"');
                if (c) {
                    const char* e = std::strchr(++c, '"');
                    if (e && e - c < 32) tag.assign(c, e);
                }
            }
        }
        std::string label;
        int rv[3], lv[3];
        if (!tag.empty() && ParseVer(tag.c_str(), rv) && ParseVer(kPluginVersionStr, lv)) {
            const bool newer = (rv[0] != lv[0]) ? rv[0] > lv[0]
                             : (rv[1] != lv[1]) ? rv[1] > lv[1]
                             :                    rv[2] > lv[2];
            label = newer ? (tag + " is out - see GitHub")
                          : ("Up to date (v" + std::string(kPluginVersionStr) + ")");
        } else {
            label = "Check failed (offline?)";
        }
        if (label.size() > 31) label.resize(31);
        std::lock_guard<std::mutex> lk(gUpdMutex);
        gUpdLabel   = label;
        gUpdRunning = false;
    }).detach();
}
} // namespace

// ── Lightweight file logger (Premiere swallows stdout; this gives us ground truth)
static void GyroLog(const char* fmt, ...)
{
    FILE* f = dbg::open();
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-instance state (one per clip the effect is applied to)
// ─────────────────────────────────────────────────────────────────────────────
struct InstanceData {
    std::unique_ptr<BRAWReader>      brawReader;
    std::unique_ptr<GyroIntegrator>  gyroIntegrator;
    std::unique_ptr<MetalWarpEngine> warpEngine;
    bool gyroLoaded        = false;
    bool gyroLoadAttempted = false;
};

// Keyed by sequence_data pointer allocated per instance
static std::unordered_map<void*, std::unique_ptr<InstanceData>> gInstances;
static std::mutex gMutex;

static InstanceData* GetOrCreate(PF_InData* in_data)
{
    void* key = in_data->sequence_data;
    std::lock_guard<std::mutex> lock(gMutex);
    auto& ptr = gInstances[key];
    if (!ptr) ptr = std::make_unique<InstanceData>();
    return ptr.get();
}

static void DestroyInstance(void* key)
{
    std::lock_guard<std::mutex> lock(gMutex);
    gInstances.erase(key);
}

// ── Source media path of the clip this effect is applied to ───────────────────
// PF_UtilitySuite::GetFileName only returns the BASENAME (e.g. "clip.braw"), which
// can't be opened. For the full path we walk the VideoSegment node graph:
//   GetContainingTimelineID(effect_ref) → AcquireVideoSegmentsID → segment node →
//   recurse inputs/operators to the Media node → read its MediaInstanceString.
// For file-based importers (BRAW), that instance string is the full media path.

// Recursively search a node (its operators + inputs) for a Media node and return
// that node's MediaInstanceString.
static std::string FindMediaInstanceString(PrSDKVideoSegmentSuite* vs,
                                           PrSDKMemoryManagerSuite* mem,
                                           csSDK_int32 nodeID, int depth)
{
    if (!vs || depth > 16) return {};
    char nodeType[256] = {};
    prPluginID hash; csSDK_int32 flags = 0;
    if (vs->GetNodeInfo(nodeID, nodeType, &hash, &flags) != suiteError_NoError) return {};

    if (std::strstr(nodeType, "Media") != nullptr) {
        PrMemoryPtr val = nullptr;
        if (vs->GetNodeProperty(nodeID, kVideoSegmentProperty_Media_InstanceString, &val)
                == suiteError_NoError && val) {
            std::string s(val);
            if (mem && mem->PrDisposePtr) mem->PrDisposePtr(val);
            GyroLog("  Media node instanceString='%s'", s.c_str());
            return s;
        }
    }

    csSDK_int32 opCount = 0;
    if (vs->GetNodeOperatorCount(nodeID, &opCount) == suiteError_NoError) {
        for (csSDK_int32 i = 0; i < opCount; ++i) {
            csSDK_int32 child = 0;
            if (vs->AcquireOperatorNodeID(nodeID, i, &child) == suiteError_NoError) {
                std::string r = FindMediaInstanceString(vs, mem, child, depth + 1);
                vs->ReleaseVideoNodeID(child);
                if (!r.empty()) return r;
            }
        }
    }
    csSDK_int32 inCount = 0;
    if (vs->GetNodeInputCount(nodeID, &inCount) == suiteError_NoError) {
        for (csSDK_int32 i = 0; i < inCount; ++i) {
            csSDK_int32 child = 0; PrTime off = 0;
            if (vs->AcquireInputNodeID(nodeID, i, &off, &child) == suiteError_NoError) {
                std::string r = FindMediaInstanceString(vs, mem, child, depth + 1);
                vs->ReleaseVideoNodeID(child);
                if (!r.empty()) return r;
            }
        }
    }
    return {};
}

// Resolve a .braw path for the CPU/AE-compat render fallback only. (The Status row and
// the GPU render path do NOT use this — the GPU resolves its own clip authoritatively
// via AcquireOperatorOwnerNodeID.) Returns the first media node's path on the timeline.
static std::string GetEffectSourcePath(PF_InData* in_data)
{
    if (!in_data || !in_data->pica_basicP || !in_data->effect_ref) return {};
    SPBasicSuite* sp = in_data->pica_basicP;

    PF_UtilitySuite10* util = nullptr;
    sp->AcquireSuite(kPFUtilitySuite, kPFUtilitySuiteVersion10, (const void**)&util);
    if (!util || !util->GetContainingTimelineID) {
        if (util) sp->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion10);
        return {};
    }
    PrTimelineID timelineID = 0;
    PF_Err terr = util->GetContainingTimelineID(in_data->effect_ref, &timelineID);
    sp->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion10);
    if (terr != PF_Err_NONE || timelineID == 0) return {};

    PrSDKVideoSegmentSuite*  vs  = nullptr;
    PrSDKMemoryManagerSuite* mem = nullptr;
    sp->AcquireSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion, (const void**)&vs);
    sp->AcquireSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion, (const void**)&mem);

    std::string found;
    if (vs) {
        csSDK_int32 segsID = 0;
        if (vs->AcquireVideoSegmentsID(timelineID, &segsID) == suiteError_NoError) {
            csSDK_int32 segCount = 0;
            vs->GetSegmentCount(segsID, &segCount);
            for (csSDK_int32 i = 0; i < segCount && found.empty(); ++i) {
                PrTime st = 0, et = 0, off = 0; prPluginID hash;
                if (vs->GetSegmentInfo(segsID, i, &st, &et, &off, &hash) != suiteError_NoError) continue;
                csSDK_int32 nodeID = 0;
                if (vs->AcquireNodeID(segsID, &hash, &nodeID) != suiteError_NoError) continue;
                found = FindMediaInstanceString(vs, mem, nodeID, 0);
                vs->ReleaseVideoNodeID(nodeID);
            }
            vs->ReleaseVideoSegmentsID(segsID);
        }
    }
    if (vs)  sp->ReleaseSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion);
    if (mem) sp->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion);

    if (found.rfind("file://", 0) == 0) found.erase(0, 7);
    return found;
}

// ── Lazily open the .braw and load gyro once we can resolve the clip's path ───
static void EnsureGyroLoaded(InstanceData* inst, PF_InData* in_data)
{
    if (!inst || inst->gyroLoaded || inst->gyroLoadAttempted) return;

    std::string path = GetEffectSourcePath(in_data);
    if (path.empty()) {
        // No clip context resolved yet — leave gyroLoadAttempted false so a later
        // RENDER (which has full context) can retry.
        return;
    }
    inst->gyroLoadAttempted = true;
    GyroLog("EnsureGyroLoaded path='%s'", path.c_str());

    if (!LooksLikeBraw(path)) {
        GyroLog("Source is not a .braw file — passthrough");
        return;
    }

    inst->brawReader = std::make_unique<BRAWReader>();
    if (!inst->brawReader->Open(path)) {
        GyroLog("BRAWReader::Open FAILED");
        inst->brawReader.reset();
        return;
    }
    int nSamples = (int)inst->brawReader->GetGyroSamples().size();
    GyroLog("BRAW opened ok: gyroSamples=%d fps=%.3f %dx%d focal=%.1f rsMs=%.2f",
            nSamples, inst->brawReader->GetFrameRate(),
            inst->brawReader->GetWidth(), inst->brawReader->GetHeight(),
            inst->brawReader->GetFocalLength(0.0),
            inst->brawReader->GetRollingShutterMs());
    if (nSamples <= 0) {
        GyroLog("No gyro samples in clip — leaving as passthrough");
        return;   // keep brawReader for focal/RS, but no warp without samples
    }
    inst->gyroIntegrator = std::make_unique<GyroIntegrator>();
    inst->gyroIntegrator->LoadFromBRAW(*inst->brawReader);
    inst->gyroLoaded = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PF_Cmd_GLOBAL_SETUP
// Called once when Premiere loads the plugin. Register pixel formats we support.
// ─────────────────────────────────────────────────────────────────────────────
static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data)
{
    out_data->my_version = PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 0);

    // Legacy PF_Cmd_RENDER effect (the Metal warp runs inside our render handler).
    // These MUST match the PiPL's AE_Effect_Global_OutFlags / _2 (0x04000440 / 0x0).
    // NOTE: we do not advertise PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 — that opts into
    // the SmartFX/GPU selectors (PF_Cmd_GPU_DEVICE_SETUP, PF_Cmd_SMART_RENDER) which
    // this plugin does not implement.
    // PF_OutFlag_SEND_UPDATE_PARAMS_UI asks the host to call PF_Cmd_UPDATE_PARAMS_UI,
    // where we refresh the read-only Status row with the detected camera / error text.
    // (Must stay in sync with the PiPL's AE_Effect_Global_OutFlags — see the .r file.)
    out_data->out_flags  = PF_OutFlag_PIX_INDEPENDENT |
                           PF_OutFlag_USE_OUTPUT_EXTENT |
                           PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    // PARAM_GROUP_START_COLLAPSED makes Premiere honor PF_ParamFlag_START_COLLAPSED on
    // topic groups (the Debug group ships collapsed). Must match the PiPL's OutFlags_2.
    out_data->out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG;

    // For Premiere, declare the pixel formats we support (AE-compat PF suite).
    if (in_data->appl_id == 'PrMr' && in_data->pica_basicP) {
        PF_PixelFormatSuite1* pfSuite = nullptr;
        in_data->pica_basicP->AcquireSuite(
            kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1,
            (const void**)&pfSuite);
        if (pfSuite) {
            (*pfSuite->ClearSupportedPixelFormats)(in_data->effect_ref);
            (*pfSuite->AddSupportedPixelFormat)(
                in_data->effect_ref,
                PrPixelFormat_BGRA_4444_32f);
            in_data->pica_basicP->ReleaseSuite(
                kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        }
    }
    return PF_Err_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// PF_Cmd_PARAMS_SETUP
// Declare parameters that appear in Effect Controls panel.
// ─────────────────────────────────────────────────────────────────────────────
static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;   // referenced by name inside the PF_ADD_* macros

    // Load user defaults (if any) so each newly-applied instance starts from the saved
    // values. Missing file / keys → the compiled kDefault* constants (the struct's inits).
    ParamDefaults ud;
    LoadDefaults(ud);

    // ── "Check support" button (FIRST, so it sits at the top of Effect Controls) ──
    // A momentary button. Its left-hand LABEL (the param name) shows the detected camera
    // or "Unsupported file" / "No gyro data". Clicking the button fires
    // PF_Cmd_USER_CHANGED_PARAM — a RELIABLE callback (unlike the flaky auto
    // UPDATE_PARAMS_UI) — at which point the parked frame has already rendered, so the
    // status the GPU published is current. PF_ParamFlag_SUPERVISE is required to receive
    // that click. The GPU render path never reads this param, but it occupies index 1, so
    // the controls below are indices 2..10 (the enum in PluginMain.h is kept in lockstep).
    PF_ADD_BUTTON(kStatusDefault, "Check", 0, PF_ParamFlag_SUPERVISE, kParamStatus);

    // ── Main controls ────────────────────────────────────────────────────
    // Param 2: Smoothing (%). Mapped to a seconds-sigma by a square curve (SmoothingPctToSigma)
    // so sensitivity is even across the slider (the old 0–10s scale crammed all the change into
    // 0–0.5s and was dead 5–10s). 0 = none (rolling-shutter only), 100 = very smooth.
    // NOTE: Premiere truncates param names (hard cap 31 chars, display even less) and has NO
    // static-text rows — every row is a control. So names stay short; behaviour details
    // (e.g. "0 = rolling-shutter fix only") are documented in the README's controls table.
    PF_ADD_FLOAT_SLIDERX("Smoothing (%)",
                         0.0f, 100.0f, 0.0f, 100.0f, ud.smoothing,
                         PF_Precision_INTEGER, 0, 0, kParamSmoothingStrength);

    // Param 3: Focal Length Override (mm) — important, so it's near the top. 0 = auto (read
    // per-frame from the .braw, tracks zoom). Set a value only if the auto focal is wrong.
    PF_ADD_FLOAT_SLIDERX("Focal Override (mm) [0=auto]",
                         0.0f, 300.0f, 0.0f, 135.0f, ud.focal,
                         PF_Precision_INTEGER, 0, 0, kParamFocalLength);

    // ── Horizon group (collapsible header in Effect Controls) ──────────────
    // The topic start/end are params too and occupy indices 4 and 7 — the GPU path reads
    // params by position, so the enum in PluginMain.h counts them.
    PF_ADD_TOPIC("Horizon", kParamHorizonGroup);

    // Param 5: Lock (%) — leveling strength; 0 = off, 100 = fully level.
    PF_ADD_FLOAT_SLIDERX("Lock (%) [0=off]",
                         0.0f, 100.0f, 0.0f, 100.0f, ud.horizonAmt,
                         PF_Precision_INTEGER, 0, 0, kParamHorizonAmount);

    // Param 6: Tilt (°) — angle the horizon lock holds; 0 = level, ± tilts it.
    // Only has effect when Lock > 0.
    PF_ADD_FLOAT_SLIDERX("Tilt (deg)",
                         -45.0f, 45.0f, -45.0f, 45.0f, ud.horizonTilt,
                         PF_Precision_TENTHS, 0, 0, kParamHorizonTilt);

    PF_END_TOPIC(kParamHorizonGroupEnd);

    // ── Scale group (expanded by default — no START_COLLAPSED flag) ─────────
    PF_ADD_TOPIC("Scale", kParamScaleGroup);

    // Param 9: Scale to Fill — auto DIGITAL scaling to hide the borders the stabilization
    // exposes. The crop amount is computed automatically.
    PF_ADD_CHECKBOXX("Scale to Fill (auto)", ud.scaleToFill, 0, kParamScaleToFill);

    // Param 10: Scale Follows Zoom (%) — ONLY affects zoom-lens footage (focal changes
    // mid-shot). Digitally eases the zoom MOTION so a jerky or hard-stopping hand-zoom looks
    // smooth: the apparent focal follows the smooth upper envelope of the lens focal (drives
    // sp.zoomEaseScale, fed by BRAWReader::GetZoomEaseScale). 0 = off (default); ~25 is a good
    // working value; the lead is capped so high values can't reach the pulsing zone.
    PF_ADD_FLOAT_SLIDERX("Scale Follows Zoom (%)",
                         0.0f, 100.0f, 0.0f, 100.0f, ud.scaleFollows,
                         PF_Precision_INTEGER, 0, 0, kParamScaleFollowsZoom);

    // Param 11: Extra Scale (%) — manual digital zoom on top of the (exact) auto-crop. It
    // scales with how deep the crop already is, so it adds at the worst/peak frames rather
    // than flat everywhere. Works even with Scale to Fill off (then it's a plain manual zoom).
    PF_ADD_FLOAT_SLIDERX("Extra Scale (%)",
                         0.0f, 50.0f, 0.0f, 50.0f, ud.extraScale,
                         PF_Precision_INTEGER, 0, 0, kParamExtraScale);

    PF_END_TOPIC(kParamScaleGroupEnd);

    // (Rolling Shutter Override was removed from the UI — set "rolling_shutter" in
    // defaults.ini instead; BRAWReader::GetRollingShutterMs applies it. 0 = auto.)

    // ── Debug group (ships COLLAPSED — needs PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG,
    //    set in GlobalSetup and the PiPL) ─────────────────────────────────────
    PF_ADD_TOPICX("Debug", PF_ParamFlag_START_COLLAPSED, kParamDebugGroup);

    // Param 14: Debug Logging — when on, writes /tmp/brawgyro.log (off = nothing written).
    PF_ADD_CHECKBOXX("Debug Logging", ud.debugLog, 0, kParamDebugLog);

    // Param 15: Render-path row (read-only): a "Check" button whose left LABEL shows which
    // render path is active (GPU Metal vs CPU software). UI-only — the GPU never reads it.
    PF_ADD_BUTTON(kRenderPathDefault, "Check", 0, PF_ParamFlag_SUPERVISE, kParamRenderPath);

    // Param 16: Update check — on click (and ONLY on click; the plugin never phones home on
    // its own) an async thread asks GitHub for the latest release tag; the label shows
    // "Checking…" then the result on the next panel refresh.
    PF_ADD_BUTTON(kCheckUpdateDefault, "Check", 0, PF_ParamFlag_SUPERVISE, kParamCheckUpdate);

    PF_END_TOPIC(kParamDebugGroupEnd);

    // Param 17: "Save as default" button — writes the CURRENT clip's values to defaults.ini so
    // every newly-applied instance starts from them. UI-only (the GPU never reads it); appended
    // LAST so it can't shift the indices the GPU reads.
    PF_ADD_BUTTON("Save settings", "Save", 0, PF_ParamFlag_SUPERVISE, kParamSaveDefault);

    out_data->num_params = 19;   // input + 18 above (incl. the three groups' start/end rows)
    return err;
}

// The file name of the clip THIS effect instance is on (per-instance, e.g. "A001.braw"
// or "DJI_0001.MP4"). Used only to key into the StatusStore — the GPU keys the same way.
static std::string GetEffectOwnFileName(SPBasicSuite* sp, PF_ProgPtr effect_ref)
{
    if (!sp || !effect_ref) return {};
    PF_UtilitySuite10* util = nullptr;
    sp->AcquireSuite(kPFUtilitySuite, kPFUtilitySuiteVersion10, (const void**)&util);
    std::string out;
    if (util && util->GetFileName) {
        PrSDKString s; std::memset(&s, 0, sizeof(s));
        if (util->GetFileName(effect_ref, &s) == PF_Err_NONE) {
            PrSDKStringSuite* ss = nullptr;
            sp->AcquireSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion, (const void**)&ss);
            if (ss) {
                char buf[1024]; csSDK_uint32 n = (csSDK_uint32)sizeof(buf);
                if (ss->CopyToUTF8String(&s, (prUTF8Char*)buf, &n) == suiteError_NoError)
                    out = buf;
                ss->DisposeString(&s);
                sp->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion);
            }
        }
    }
    if (util) sp->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion10);
    return out;
}

// Set one button param's left-hand LABEL (cosmetic only — no undo, no re-render).
static void SetButtonLabel(PF_InData* in_data, PF_ParamDef* params[], int idx, const char* text)
{
    if (!in_data->pica_basicP || !params || !params[idx]) return;
    PF_ParamUtilsSuite3* pu = nullptr;
    in_data->pica_basicP->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3,
                                       (const void**)&pu);
    if (pu && pu->PF_UpdateParamUI) {
        PF_ParamDef copy = *params[idx];
        copy.uu.id = idx;
        PF_STRNNCPY(copy.PF_DEF_NAME, text, sizeof(copy.PF_DEF_NAME));
        pu->PF_UpdateParamUI(in_data->effect_ref, idx, &copy);
    }
    if (pu) in_data->pica_basicP->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Refresh both read-only button labels:
//  • "Check support" (param 1): the GPU render's authoritative, PER-CLIP result
//    (StatusStore), keyed by THIS effect's own clip file so it reflects the clip you're
//    viewing, not whatever rendered last. The UI never re-resolves the clip itself.
//  • "Render path" (param 11): which render path (GPU/CPU) last executed (global).
// Called from the reliable PF_Cmd_USER_CHANGED_PARAM (either button) and, best-effort,
// from PF_Cmd_UPDATE_PARAMS_UI.
// ─────────────────────────────────────────────────────────────────────────────
static void RefreshStatusLabel(PF_InData* in_data, PF_ParamDef* params[])
{
    if (!in_data->pica_basicP || !params || !params[kParamStatus]) return;

    if (params[kParamDebugLog])
        dbg::setEnabled(params[kParamDebugLog]->u.bd.value != 0);

    // (1) Per-clip support status.
    std::string own = GetEffectOwnFileName(in_data->pica_basicP, in_data->effect_ref);
    std::string key = statusstore::keyOf(own);
    statusstore::Info st;
    bool found = statusstore::get(key, st);

    const char* text;
    if (!found)                                 text = "Play clip, then Check";
    else switch (st.state) {
        case statusstore::Ok:          text = st.camera.empty() ? "BRAW gyro ready" : st.camera.c_str(); break;
        case statusstore::NoGyro:      text = "!! NO GYRO DATA IN CLIP !!"; break;
        case statusstore::Unsupported: text = "!! UNSUPPORTED FILE !!"; break;
        default:                       text = kStatusDefault; break;
    }
    SetButtonLabel(in_data, params, kParamStatus, text);

    // (2) Active render path (global).
    std::string path = statusstore::getRenderPath();
    std::string pathLabel = path.empty() ? kRenderPathDefault : ("Render: " + path);
    if (pathLabel.size() > 31) pathLabel.resize(31);
    SetButtonLabel(in_data, params, kParamRenderPath, pathLabel.c_str());

    // (3) Update-check result (only once a check has been started).
    {
        std::lock_guard<std::mutex> lk(gUpdMutex);
        if (!gUpdLabel.empty())
            SetButtonLabel(in_data, params, kParamCheckUpdate, gUpdLabel.c_str());
    }

    GyroLog("RefreshStatusLabel: own='%s' key='%s' found=%d state=%d status='%s' render='%s'",
            own.c_str(), key.c_str(), (int)found, found ? (int)st.state : -1, text, path.c_str());
}

// PF_Cmd_UPDATE_PARAMS_UI (auto, best-effort) and PF_Cmd_USER_CHANGED_PARAM (button click).
static PF_Err UpdateParamsUI(PF_InData* in_data, PF_OutData* /*out_data*/,
                             PF_ParamDef* params[])
{
    RefreshStatusLabel(in_data, params);
    return PF_Err_NONE;
}

// "Save as default" button: capture the current clip's parameter values into defaults.ini.
// New instances start from these (loaded in ParamsSetup); existing clips are untouched.
static PF_Err SaveDefaultsFromParams(PF_InData* in_data, PF_ParamDef* params[])
{
    if (!params) return PF_Err_NONE;
    ParamDefaults d;   // start from the EXISTING file (preserves zoom_sync_offset_frames,
    LoadDefaults(d);   // which has no UI param), then overwrite with the live values
    if (params[kParamSmoothingStrength]) d.smoothing      = params[kParamSmoothingStrength]->u.fs_d.value;
    if (params[kParamFocalLength])       d.focal          = params[kParamFocalLength]->u.fs_d.value;
    if (params[kParamHorizonAmount])     d.horizonAmt     = params[kParamHorizonAmount]->u.fs_d.value;
    if (params[kParamHorizonTilt])       d.horizonTilt    = params[kParamHorizonTilt]->u.fs_d.value;
    if (params[kParamScaleToFill])       d.scaleToFill    = params[kParamScaleToFill]->u.bd.value;
    if (params[kParamScaleFollowsZoom])  d.scaleFollows   = params[kParamScaleFollowsZoom]->u.fs_d.value;
    if (params[kParamExtraScale])        d.extraScale     = params[kParamExtraScale]->u.fs_d.value;
    if (params[kParamDebugLog])          d.debugLog       = params[kParamDebugLog]->u.bd.value;
    // (rolling_shutter has no UI param — LoadDefaults above preserved the file's value)
    const bool ok = SaveDefaults(d);
    GyroLog("Saved defaults.ini: ok=%d smoothing=%.1f focal=%.1f scaleFollows=%.1f scaleToFill=%d",
            (int)ok, d.smoothing, d.focal, d.scaleFollows, d.scaleToFill);
    SetButtonLabel(in_data, params, kParamSaveDefault,
                   ok ? "Saved (restart to apply)" : "Save FAILED (permissions?)");
    return PF_Err_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// PF_Cmd_SEQUENCE_SETUP / RESETUP
// Called when the effect is first applied or the sequence is opened.
// This is where we load BRAW gyro data — once per clip, not per frame.
// ─────────────────────────────────────────────────────────────────────────────
static PF_Err SequenceSetup(PF_InData* in_data, PF_OutData* out_data)
{
    InstanceData* inst = GetOrCreate(in_data);
    if (!inst) return PF_Err_OUT_OF_MEMORY;

    // Allocate Metal warp engine
    if (!inst->warpEngine) {
        inst->warpEngine = std::make_unique<MetalWarpEngine>();
        if (!inst->warpEngine->Init()) {
            inst->warpEngine.reset();
        }
    }

    // Resolve the clip's .braw path and load its gyro data (no-op if already done,
    // retried from Render if the clip context isn't resolvable here yet).
    EnsureGyroLoaded(inst, in_data);

    return PF_Err_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// PF_Cmd_RENDER
// Called for every frame. This is the hot path.
// ─────────────────────────────────────────────────────────────────────────────
static PF_Err Render(
    PF_InData*   in_data,
    PF_OutData*  out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    InstanceData* inst = GetOrCreate(in_data);
    statusstore::setRenderPath("CPU (software)");

    // Update debug-logging state BEFORE loading (so the focal scan etc. can log if enabled).
    dbg::setEnabled(params && params[kParamDebugLog] && params[kParamDebugLog]->u.bd.value != 0);

    // Ensure gyro is loaded (Render has full clip context if SEQUENCE_SETUP didn't).
    EnsureGyroLoaded(inst, in_data);

    // ── Read effect parameters (null-safe; fall back to defaults) ──────────
    auto fparam = [&](int idx, float dflt) -> float {
        return (params && params[idx]) ? params[idx]->u.fs_d.value : dflt;
    };
    auto bparam = [&](int idx, int dflt) -> bool {
        return ((params && params[idx]) ? params[idx]->u.bd.value : dflt) != 0;
    };
    float smoothingPct = fparam(kParamSmoothingStrength, kDefaultSmoothing);
    bool  scaleToFill  = bparam(kParamScaleToFill,       kDefaultScaleToFill);
    float focalParam   = fparam(kParamFocalLength,       kDefaultFocalLength);
    float horizonAmt   = fparam(kParamHorizonAmount,     kDefaultHorizonAmount);
    float horizonTilt  = fparam(kParamHorizonTilt,       kDefaultHorizonTilt);
    float scaleFollows = fparam(kParamScaleFollowsZoom,  kDefaultScaleFollowsZoom);
    float extraScale   = fparam(kParamExtraScale,        kDefaultExtraScale);

    // ── Current frame time in seconds ────────────────────────────────────
    double timeSec = static_cast<double>(in_data->current_time) /
                     static_cast<double>(in_data->time_scale);

    // ── Compute warp map ─────────────────────────────────────────────────
    WarpMap warpMap;
    if (inst && inst->gyroIntegrator) {
        StabilizationParams sp;
        sp.smoothingWindowSec = SmoothingPctToSigma(smoothingPct);
        // Horizon Lock (%) is the sole control: 0 = off, 100 = fully level.
        sp.horizonStrength    = horizonAmt / 100.0f;
        sp.horizonTiltRad     = horizonTilt * 0.01745329252f;   // degrees → radians
        sp.zoomToFill         = scaleToFill;
        sp.zoomSteadiness     = 0.0f;   // border-crop pure adaptive; "Scale Follows Zoom" drives zoomEaseScale
        sp.extraCropFrac      = extraScale / 100.0f;
        if (inst->brawReader) {   // off-speed footage: render(playback) time → gyro(capture) time
            double gd = inst->brawReader->GetGyroDurationSec();
            double cd = inst->brawReader->GetClipDurationSec();
            sp.gyroTimeScale = (gd > 0.1 && cd > 0.1) ? gd / cd : 1.0;
        }
        // Focal length: user override (param > 0) else auto (from the .braw frame 0).
        sp.focalLengthMM      = (focalParam > 0.0f)
                                    ? focalParam
                                    : (inst->brawReader
                                        ? inst->brawReader->GetFocalLength(timeSec)
                                        : 35.0f);
        // "Scale Follows Zoom" (0..100) eases the lens's hard zoom start/stop via a digital
        // zoom so the apparent focal follows a smoothed curve. Off under a manual fixed focal.
        float leadF = scaleFollows * 0.1f;               // %/10 frames of lead
        if (leadF > 5.0f) leadF = 5.0f;                  // cap: pre/post easing beyond ~5 frames reads as pulsing
        sp.zoomEaseScale      = (focalParam > 0.0f || scaleFollows <= 0.0f || !inst->brawReader)
                                    ? 1.0f               // 0 = off, no calc
                                    : inst->brawReader->GetZoomEaseScale(timeSec, leadF);
        sp.maxFocalLengthMM   = (focalParam > 0.0f)
                                    ? focalParam
                                    : (inst->brawReader
                                        ? inst->brawReader->GetMaxFocalLength()
                                        : 35.0f);
        sp.photositePitchUm   = inst->brawReader
                                    ? inst->brawReader->GetPhotositePitchUm()
                                    : 0.0f;
        sp.rollingShutterMs   = inst->brawReader
                                    ? inst->brawReader->GetRollingShutterMs()   // incl. ini override
                                    : 16.0f;
        warpMap = inst->gyroIntegrator->ComputeWarpMap(
            timeSec, sp, output ? output->width : 0, output ? output->height : 0);
    }

    // ── Get source and destination pixel buffers ──────────────────────────
    PF_LayerDef* input = (params && params[0]) ? &params[0]->u.ld : nullptr;
    if (!output) return PF_Err_NONE;

    int width       = output->width;
    int height      = output->height;
    auto* dst       = reinterpret_cast<uint8_t*>(output->data);
    int   dstRowBytes = output->rowbytes;
    auto* src       = input ? reinterpret_cast<uint8_t*>(input->data) : nullptr;
    int   srcRowBytes = input ? input->rowbytes : 0;

    // Guard: bail to a no-op if anything looks wrong rather than crashing the host.
    if (!dst || !src || width <= 0 || height <= 0 ||
        dstRowBytes == 0 || srcRowBytes == 0) {
        return PF_Err_NONE;
    }

    // ── Apply Metal warp ──────────────────────────────────────────────────
    if (inst && inst->warpEngine && warpMap.IsValid()) {
        inst->warpEngine->ApplyWarp(
            src, srcRowBytes,
            dst, dstRowBytes,
            width, height,
            warpMap);
    } else {
        // Passthrough: copy input to output. rowbytes can be negative (bottom-up
        // worlds), so copy |rowbytes| worth per row and honour each sign separately.
        size_t copyBytes = static_cast<size_t>(
            std::min(std::abs(srcRowBytes), std::abs(dstRowBytes)));
        for (int row = 0; row < height; ++row) {
            std::memcpy(dst + row * dstRowBytes,
                        src + row * srcRowBytes,
                        copyBytes);
        }
    }

    return PF_Err_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// PF_Cmd_SEQUENCE_FLATTEN / SETDOWN
// ─────────────────────────────────────────────────────────────────────────────
static PF_Err SequenceSetdown(PF_InData* in_data)
{
    DestroyInstance(in_data->sequence_data);
    return PF_Err_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main entry point
// ─────────────────────────────────────────────────────────────────────────────
DllExport PREMPLUGENTRY EffectMain(
    csSDK_int32  selector,
    PF_InData*   in_data,
    PF_OutData*  out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void*        extra)
{
    PF_Err err = PF_Err_NONE;

    try {
    switch (selector) {
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data);
            break;

        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data);
            break;

        case PF_Cmd_SEQUENCE_SETUP:
        case PF_Cmd_SEQUENCE_RESETUP:
            err = SequenceSetup(in_data, out_data);
            break;

        case PF_Cmd_SEQUENCE_FLATTEN:
        case PF_Cmd_SEQUENCE_SETDOWN:
            err = SequenceSetdown(in_data);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;

        case PF_Cmd_UPDATE_PARAMS_UI:       // auto refresh (best-effort)
            err = UpdateParamsUI(in_data, out_data, params);
            break;

        case PF_Cmd_USER_CHANGED_PARAM: {   // a button was clicked (reliable)
            PF_UserChangedParamExtra* uc = reinterpret_cast<PF_UserChangedParamExtra*>(extra);
            if (uc && uc->param_index == kParamSaveDefault) {
                err = SaveDefaultsFromParams(in_data, params);   // "Save settings"
            } else {
                if (uc && uc->param_index == kParamCheckUpdate)
                    StartUpdateCheck();                          // async; label set via refresh
                err = UpdateParamsUI(in_data, out_data, params); // refresh all status labels
            }
            break;
        }

        default:
            break;
    }
    } catch (const std::exception& e) {
        GyroLog("EXCEPTION in selector=%d: %s", (int)selector, e.what());
    } catch (...) {
        GyroLog("UNKNOWN EXCEPTION in selector=%d", (int)selector);
    }

    return err;
}
