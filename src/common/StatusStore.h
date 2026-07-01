// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// StatusStore.h
// Per-clip status, published by the GPU render and read by the Effect Controls UI.
//
// Why: only the GPU render path can reliably tell which clip an effect instance is on
// (AcquireOperatorOwnerNodeID) and whether it stabilizes (BRAW opened + gyro present).
// The UI lives in the separate AE entry point and can't re-derive that. So the GPU
// publishes the result here, keyed by the clip's media file, and the UI looks up its
// OWN clip's entry by the same key — that's what makes the status correct per clip even
// with several clips on the timeline (a single shared value would show the last one
// rendered on every clip).
//
// Key = the media file name reduced to a lowercase stem (basename without extension), so
// the GPU's media path and the UI's GetFileName() agree regardless of folder or case.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <cstring>
#include <cctype>

namespace statusstore {

enum State {
    Unknown     = 0,   // not seen yet
    Ok          = 1,   // BRAW opened, gyro present → stabilizing
    NoGyro      = 2,   // BRAW opened but no gyro stream
    Unsupported = 3,   // not a readable BRAW (wrong format, etc.)
};

struct Info {
    State       state = Unknown;
    std::string camera;          // camera model when state == Ok (may be empty)
};

// "/a/b/A001_C001.braw" or "A001_C001.BRAW" → "a001_c001"
inline std::string keyOf(const std::string& pathOrName)
{
    size_t slash = pathOrName.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? pathOrName : pathOrName.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    for (auto& c : base) c = (char)std::tolower((unsigned char)c);
    return base;
}

inline std::mutex&                  mtx() { static std::mutex m;                return m; }
inline std::map<std::string, Info>& tbl() { static std::map<std::string, Info> t; return t; }

inline void put(const std::string& key, State st, const char* camera)
{
    if (key.empty()) return;
    std::lock_guard<std::mutex> lk(mtx());
    Info& i = tbl()[key];
    i.state  = st;
    i.camera = camera ? camera : "";
}

// Returns false if this clip hasn't been resolved yet (caller shows a neutral hint).
inline bool get(const std::string& key, Info& out)
{
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx());
    auto it = tbl().find(key);
    if (it == tbl().end()) return false;
    out = it->second;
    return true;
}

// ── Active render path ───────────────────────────────────────────────────────
// Which render path last executed. Global (not per-clip): Premiere picks GPU or CPU
// per project, not per clip. The GPU SmartFX path and the CPU/AE-compat Render each
// stamp this when they run; the UI shows it so you can see which path is in use.
inline std::mutex&   pathMtx()  { static std::mutex m;       return m; }
inline std::string&  pathSlot() { static std::string s;      return s; }

inline void setRenderPath(const char* p)
{
    std::lock_guard<std::mutex> lk(pathMtx());
    pathSlot() = p ? p : "";
}

inline std::string getRenderPath()
{
    std::lock_guard<std::mutex> lk(pathMtx());
    return pathSlot();
}

} // namespace statusstore
