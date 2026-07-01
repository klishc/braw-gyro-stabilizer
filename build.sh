#!/bin/bash
# Copyright (c) 2026 Niko Klishchenko. All rights reserved.
# build.sh — Build and install BRAW Gyro Stabilizer for Premiere Pro
# Metal shader is compiled at runtime — no Metal toolchain needed at build time.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/build"
BUNDLE_NAME="BRAWGyroStabilizer.plugin"
PR_PLUGINS="${HOME}/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
INSTALL_PATH="${PR_PLUGINS}/${BUNDLE_NAME}"
# Premiere blacklists ("Ignore") plugin identities that failed a past scan. The early
# builds were also the WRONG bundle type (.bundle/BNDL); effects must be .plugin/eFKT.
# Every prior identity got poisoned, so we ship a pristine one and remove the old ones.
OLD_BUNDLES=(
    "${PR_PLUGINS}/BRAWGyroStabilizer.bundle"
    "${PR_PLUGINS}/BRAWGyroStabilizerFX.bundle"
    "${PR_PLUGINS}/GyroStabFX.bundle"
    "${PR_PLUGINS}/GyroStabV2.plugin"
    "${PR_PLUGINS}/GyroStabV3.plugin"
    "${PR_PLUGINS}/GyroStabV4.plugin"
)

if [[ "${1:-}" == "clean" ]]; then
    echo "→ Removing build directory..."
    rm -rf "${BUILD_DIR}"
    echo "✓ Clean done"
    exit 0
fi

if [[ "${1:-}" == "uninstall" ]]; then
    rm -rf "${INSTALL_PATH}"
    echo "✓ Uninstalled"
    exit 0
fi

echo "═══════════════════════════════════════════════════"
echo "  BRAW Gyro Stabilizer — Build Script"
echo "═══════════════════════════════════════════════════"

# ── Xcode ─────────────────────────────────────────────────────────────────────
XCODE_DEV="/Applications/Xcode.app/Contents/Developer"
if [[ ! -d "${XCODE_DEV}" ]]; then
    echo "✗ Xcode.app not found — install from the App Store"
    exit 1
fi
if [[ "$(xcode-select -p 2>/dev/null)" != "${XCODE_DEV}" ]]; then
    echo "⚠  Switching to Xcode developer tools..."
    sudo xcode-select -s "${XCODE_DEV}"
fi
echo "✓ Xcode: ${XCODE_DEV}"

# ── CMake ─────────────────────────────────────────────────────────────────────
if ! command -v cmake &>/dev/null; then
    echo "✗ CMake not found — run: brew install cmake"; exit 1
fi
echo "✓ CMake: $(cmake --version | head -1)"

# ── SDK root ──────────────────────────────────────────────────────────────────
# All SDKs live under one root (default ~/SDKs). Override with the SDK_ROOT
# environment variable, or point each SDK individually (PR_SDK / AE_SDK).
SDK_ROOT="${SDK_ROOT:-${HOME}/SDKs}"

# ── Premiere SDK ──────────────────────────────────────────────────────────────
PR_SDK="${SDK_ROOT}/Premiere Pro 26.0 C++ SDK/Examples/Headers"
if [[ ! -f "${PR_SDK}/PrSDKEntry.h" ]]; then
    echo "✗ Premiere SDK not found at: ${PR_SDK}"; exit 1
fi
echo "✓ Premiere SDK found"

# ── BRAW SDK ──────────────────────────────────────────────────────────────────
BRAW_SDK="/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac"
if [[ ! -f "${BRAW_SDK}/Include/BlackmagicRawAPI.h" ]]; then
    echo "✗ BRAW SDK not found at: ${BRAW_SDK}"; exit 1
fi
echo "✓ BRAW SDK found"

# ── After Effects SDK (required for the AE-compatible effect API) ─────────────
AE_SDK="${AE_SDK_BASE_PATH:-${SDK_ROOT}/After Effects SDK}"
if [[ ! -f "${AE_SDK}/Examples/Headers/AE_Effect.h" ]]; then
    echo "✗ After Effects SDK not found at: ${AE_SDK}"
    echo "    Premiere effect plugins compile against the FREE After Effects SDK"
    echo "    (PrSDKAESupport.h pulls in AE_Effect.h / AE_EffectCB.h, which are not"
    echo "    in the Premiere SDK). Download it from the Adobe developer site, unzip"
    echo "    it to '${SDK_ROOT}/After Effects SDK', or set AE_SDK_BASE_PATH."
    exit 1
fi
echo "✓ After Effects SDK found"

mkdir -p "${PR_PLUGINS}"
echo "✓ Plugin folder ready"
echo ""

# ── Quit Premiere if running ──────────────────────────────────────────────────
# Match the actual app name ("Adobe Premiere Pro 2026"); the bare "Adobe Premiere Pro"
# never matched, so Premiere kept running with a stale plugin cache and never rescanned.
PR_APP="$(pgrep -fl 'Adobe Premiere Pro [0-9].*/Contents/MacOS/Adobe Premiere Pro' 2>/dev/null | grep -oE 'Adobe Premiere Pro [0-9]+' | head -1 || true)"
if [[ -n "${PR_APP}" ]]; then
    echo "⚠  Quitting ${PR_APP}..."
    osascript -e "quit app \"${PR_APP}\"" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8; do
        pgrep -f "${PR_APP}.app/Contents/MacOS/${PR_APP}\$" &>/dev/null || break
        sleep 1
    done
    # Force-quit if it ignored the polite request (it can be unresponsive to AppleEvents).
    pkill -f "${PR_APP}.app/Contents/MacOS/${PR_APP}\$" 2>/dev/null || true
    sleep 1
fi

# ── Compile PiPL resource ─────────────────────────────────────────────────────
echo "→ Compiling PiPL resource..."
mkdir -p "${BUILD_DIR}"
xcrun Rez \
    -o "${BUILD_DIR}/BRAWGyroStabilizer.rsrc" \
    -useDF \
    -i "${PR_SDK}" \
    -i "${AE_SDK}/Examples/Headers" \
    -i "${AE_SDK}/Examples/Resources" \
    -d PiPLVer2p3 \
    "${SCRIPT_DIR}/pipl/BRAWGyroStabilizer.r"
# -useDF is essential: without it Rez writes the PiPL into the file's RESOURCE FORK,
# leaving a 0-byte data fork. cmake -E copy only copies the data fork (and codesign's
# xattr strip would drop a resource fork anyway), so Premiere would see an empty PiPL
# and never list the effect. -useDF writes the resources to the data fork.
if [[ ! -s "${BUILD_DIR}/BRAWGyroStabilizer.rsrc" ]]; then
    echo "✗ PiPL compile produced an empty .rsrc — Premiere won't discover the effect"; exit 1
fi
echo "✓ PiPL compiled ($(stat -f%z "${BUILD_DIR}/BRAWGyroStabilizer.rsrc") bytes)"
echo ""

# ── CMake configure + build ───────────────────────────────────────────────────
echo "→ Configuring..."
cmake -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DPREBUILT_RSRC="${BUILD_DIR}/BRAWGyroStabilizer.rsrc" \
    -DSDK_ROOT="${SDK_ROOT}" \
    -DAE_SDK="${AE_SDK}" \
    -S "${SCRIPT_DIR}"
echo ""

echo "→ Building..."
cmake --build "${BUILD_DIR}" --config Release --parallel
echo ""

echo "→ Installing..."
for ob in "${OLD_BUNDLES[@]}"; do
    if [[ -d "${ob}" ]]; then
        echo "  removing old (Ignore-flagged) bundle: ${ob}"
        rm -rf "${ob}"
    fi
done
cmake --install "${BUILD_DIR}"
echo ""

# ── Verify ────────────────────────────────────────────────────────────────────
if [[ -d "${INSTALL_PATH}" ]]; then
    echo "✓ Installed: ${INSTALL_PATH}"
    echo "  Bundle contents:"
    find "${INSTALL_PATH}" -type f | sed 's|.*/||' | sort | sed 's/^/    /'
    echo ""
    echo "══════════════════════════════════════════════════"
    echo "  ✓ Done! Open Premiere Pro"
    echo "  → Video Effects → BRAW Tools → BRAW Gyro Stabilizer"
    echo "══════════════════════════════════════════════════"
else
    echo "✗ Install failed — bundle not found at ${INSTALL_PATH}"
    exit 1
fi
