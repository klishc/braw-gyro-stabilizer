#!/bin/bash
# Copyright (c) 2026 Niko Klishchenko. All rights reserved.
# Package the compiled plug-in into a double-click .pkg installer.
#
#   ./build.sh        # compile first
#   ./package.sh      # produces dist/BRAW-Gyro-Stabilizer-<ver>.pkg
#
# The .pkg installs the plug-in system-wide into Premiere's MediaCore folder
# (asks for an admin password during install). Premiere picks it up on next launch.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="1.0.0"
PLUGIN="${SCRIPT_DIR}/build/GyroStabV4.plugin"
TARGET="Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
OUT="${SCRIPT_DIR}/dist/BRAW-Gyro-Stabilizer-${VERSION}.pkg"

if [[ ! -d "${PLUGIN}" ]]; then
    echo "✗ Compiled plug-in not found at ${PLUGIN}"
    echo "  Run ./build.sh first."
    exit 1
fi

echo "→ Staging payload..."
ROOT="$(mktemp -d)"
trap 'rm -rf "${ROOT}"' EXIT
mkdir -p "${ROOT}/${TARGET}"
cp -R "${PLUGIN}" "${ROOT}/${TARGET}/"

echo "→ Building installer..."
mkdir -p "${SCRIPT_DIR}/dist"
pkgbuild \
    --root "${ROOT}" \
    --identifier "io.nk.gyrostabv4.installer" \
    --version "${VERSION}" \
    --install-location "/" \
    "${OUT}"

echo ""
echo "✓ Installer: ${OUT}"
echo "  Double-click to install (it will ask for an admin password),"
echo "  then restart Premiere Pro."
