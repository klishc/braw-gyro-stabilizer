// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// DebugLog.h — runtime-gated debug logging to /tmp/brawgyro.log.
//
// All logging is off unless the effect's "Debug Logging" checkbox turns it on (the render
// path calls dbg::setEnabled()). When off, dbg::open() returns nullptr so every existing
// `FILE* f = dbg::open(); if (f) { … }` site simply writes nothing — zero overhead beyond
// an atomic load. The flag lives in an inline-function-local static so it's a single shared
// instance across all translation units with no .cpp needed.
#pragma once

#include <atomic>
#include <cstdio>

namespace dbg {

inline std::atomic<bool>& flag() { static std::atomic<bool> e{false}; return e; }

inline void  setEnabled(bool on) { flag().store(on, std::memory_order_relaxed); }
inline bool  enabled()           { return flag().load(std::memory_order_relaxed); }

// Returns an append handle to the log file only when debug logging is enabled, else nullptr.
inline FILE* open() { return enabled() ? std::fopen("/tmp/brawgyro.log", "a") : nullptr; }

} // namespace dbg
