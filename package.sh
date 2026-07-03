#!/bin/bash
# Copyright (c) 2026 Niko Klishchenko. All rights reserved.
# Package the compiled plug-in into a double-click .pkg installer with a
# branded Welcome/Finish screen (logo + what's being installed).
#
#   ./build.sh        # compile first
#   ./package.sh      # produces dist/BRAW-Gyro-Stabilizer-<ver>.pkg
#
# The .pkg installs the plug-in system-wide into Premiere's MediaCore folder
# (asks for an admin password during install). Premiere picks it up on next launch.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="1.1.0"
IDENTIFIER="io.nk.brawgyrostabilizer.installer"
PLUGIN="${SCRIPT_DIR}/build/BRAWGyroStabilizer.plugin"
TARGET="Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
OUT="${SCRIPT_DIR}/dist/BRAW-Gyro-Stabilizer-${VERSION}.pkg"

if [[ ! -d "${PLUGIN}" ]]; then
    echo "✗ Compiled plug-in not found at ${PLUGIN}"
    echo "  Run ./build.sh first."
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
ROOT="${WORK}/root"
RES="${WORK}/resources"
mkdir -p "${ROOT}/${TARGET}" "${RES}"

echo "→ Staging payload..."
cp -R "${PLUGIN}" "${ROOT}/${TARGET}/"

echo "→ Writing preinstall cleanup script..."
# Remove any previously-installed copy BEFORE extracting the new payload. A pkg install
# MERGES into an existing bundle (stale files linger if a future version renames or
# drops one), so a clean delete-then-install guarantees an exact replacement. Also
# clears the legacy bundle name some early testers have.
mkdir -p "${WORK}/scripts"
cat > "${WORK}/scripts/preinstall" <<'SH'
#!/bin/bash
MC="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
rm -rf "${MC}/BRAWGyroStabilizer.plugin" \
       "${MC}/GyroStabV4.plugin"
exit 0
SH
chmod +x "${WORK}/scripts/preinstall"

echo "→ Building component package..."
pkgbuild \
    --root "${ROOT}" \
    --identifier "${IDENTIFIER}" \
    --version "${VERSION}" \
    --install-location "/" \
    --scripts "${WORK}/scripts" \
    "${WORK}/component.pkg"

echo "→ Writing installer UI (logo + info)..."
# The Installer renders welcome/conclusion HTML through the limited NSAttributedString
# importer (not a browser): tables linearize, block backgrounds become per-line bars, and
# only distribution-referenced files get bundled. So we keep the copy as plain flowing
# text using the installer's own (appearance-adaptive) default label color, add colour
# only for mid-tone accents that read on both light and dark, and deliver the logo via
# the distribution <background> element — the reliable way to show an image in a .pkg.
rsvg-convert -w 132 -h 132 -o "${RES}/background.png" "${SCRIPT_DIR}/assets/logo.svg"

cat > "${RES}/welcome.html" <<'HTML'
<!DOCTYPE html><html><head><meta charset="utf-8"></head>
<body style="font-family:-apple-system,Helvetica,Arial,sans-serif;font-size:13px;line-height:1.5;">
  <p style="font-size:20px;margin:0 0 2px;"><b>BRAW Gyro Stabilizer</b></p>
  <p style="font-size:12px;margin:0 0 10px;"><span style="color:#8a8a8a;">Version __VERSION__ &middot; Premiere Pro effect</span></p>
  <p style="margin:0 0 12px;">Real-time gyroscopic stabilization for Blackmagic RAW footage. It reads
     the motion the camera recorded inside each <span style="color:#e08a3c;">.braw</span> and
     counter-steers every frame on the GPU &mdash; no calibration, no analysis pass, no round-trip.</p>
  <p style="margin:0 0 12px;">
     <b>Installs</b>&nbsp;&nbsp;BRAWGyroStabilizer.plugin<br>
     <b>Location</b>&nbsp;&nbsp;/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore<br>
     <b>Find it in</b>&nbsp;&nbsp;<span style="color:#1aa88f;">Video Effects &rarr; BRAW Tools &rarr; BRAW Gyro Stabilizer</span><br>
     <b>Requires</b>&nbsp;&nbsp;Apple-silicon Mac &middot; Premiere Pro 26 &middot; admin password</p>
  <p style="margin:0;">Click Continue, then Install. Restart Premiere Pro afterwards so it loads the effect.</p>
</body></html>
HTML
/usr/bin/sed -i '' "s/__VERSION__/${VERSION}/g" "${RES}/welcome.html"

cat > "${RES}/conclusion.html" <<'HTML'
<!DOCTYPE html><html><head><meta charset="utf-8"></head>
<body style="font-family:-apple-system,Helvetica,Arial,sans-serif;font-size:13px;line-height:1.6;">
  <p style="font-size:18px;margin:0 0 8px;"><b>Installed</b></p>
  <p style="margin:0 0 10px;"><b>Restart Premiere Pro</b> to load the plug-in.</p>
  <p style="margin:0;">Then apply <span style="color:#1aa88f;"><b>Video Effects &rarr; BRAW Tools &rarr; BRAW Gyro Stabilizer</b></span>
     to a Blackmagic RAW clip and press play &mdash; it stabilizes on its own. Use the
     <b>Check support</b> button in the effect to confirm a clip has gyro data.</p>
</body></html>
HTML

cat > "${WORK}/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>BRAW Gyro Stabilizer ${VERSION}</title>
    <background file="background.png" mime-type="image/png" alignment="bottomleft" scaling="none"/>
    <background-darkAqua file="background.png" mime-type="image/png" alignment="bottomleft" scaling="none"/>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="false" rootVolumeOnly="true"/>
    <choices-outline>
        <line choice="default"><line choice="${IDENTIFIER}"/></line>
    </choices-outline>
    <choice id="default"/>
    <choice id="${IDENTIFIER}" visible="false">
        <pkg-ref id="${IDENTIFIER}"/>
    </choice>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
XML

echo "→ Building product installer..."
mkdir -p "${SCRIPT_DIR}/dist"
productbuild \
    --distribution "${WORK}/distribution.xml" \
    --resources "${RES}" \
    --package-path "${WORK}" \
    "${OUT}"

echo ""
echo "✓ Installer: ${OUT}"
echo "  Double-click to install (it will ask for an admin password),"
echo "  then restart Premiere Pro."
