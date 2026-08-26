#!/bin/bash
# Build a .deb package for the orthanc-jxl plugin.
# Run from the repo root: ./deploy/build-deb.sh [version]
#
# Mirrors the kaitake house deb pattern (hikaru-gateway/contrast
# deploy/build-deb.sh): imperative dpkg-deb with inline control/postinst,
# version from `git describe --tags --long`. Motivation (2026-08-25 incident):
# the hand-copied plugin .so silently stopped loading when prod switched from
# Debian's orthanc packages to upstream's (different plugin dir), and every
# ingest since stored untranscoded. A deb gives the plugin an owned path, a
# dependency edge on orthanc + libjxl, upgrade ordering, and a postinst that
# CHECKS the "Plugins" configuration and restarts Orthanc — so it can never
# again vanish without something complaining.
#
#   - builds via meson/ninja into deploy/build-release (fresh configure each
#     run; the dev ./build dir is left alone)
#   - installs ONE file: /usr/share/orthanc/plugins/libOrthancJxl.so (the
#     upstream orthanc packages' conventional plugin dir)
#   - Depends: orthanc (>= 1.12.10) — the SDK header this build pins;
#     libjxl (>= 0.12) — the SONAME the plugin links (libjxl.so.0.12)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# House version scheme: <release>-<count>-g<hash>, leading 'v' stripped; a
# tagless checkout reshapes `describe --always`'s bare SHA to 0.0.0-<n>-g<sha>.
raw_ver="$(git -C "${ROOT}" describe --tags --long --always 2>/dev/null || true)"
[ -n "$raw_ver" ] || raw_ver="0.0.0"
ver="${raw_ver#v}"
case "$ver" in
  *.*)  : ;;
  *)    count="$(git -C "${ROOT}" rev-list --count HEAD 2>/dev/null || echo 0)"
        ver="0.0.0-${count}-g${ver}" ;;
esac
VERSION="${1:-$ver}"
ARCH="amd64"
PKG="orthanc-jxl_${VERSION}_${ARCH}"
STAGE="${ROOT}/deploy/${PKG}"

echo "=== Building orthanc-jxl ${VERSION} ==="
cd "${ROOT}"

# 1. Fresh release build (meson fetches/verifies the pinned Orthanc SDK header
# at configure time; the SDK is forward-compatible with newer servers).
rm -rf deploy/build-release
meson setup deploy/build-release --buildtype=release >/dev/null
ninja -C deploy/build-release
SO="deploy/build-release/src/libOrthancJxl.so"
[ -f "$SO" ] || { echo "build produced no ${SO}" >&2; exit 1; }

# 2. Stage.
rm -rf "${STAGE}"
install -D -m 644 "$SO" "${STAGE}/usr/share/orthanc/plugins/libOrthancJxl.so"
mkdir -p "${STAGE}/DEBIAN"

cat > "${STAGE}/DEBIAN/control" <<EOF
Package: orthanc-jxl
Version: ${VERSION}
Section: science
Priority: optional
Architecture: ${ARCH}
Depends: orthanc (>= 1.12.10), libjxl (>= 0.12)
Maintainer: Kaitake Radiology Systems <ryan@testtoast.com>
Description: JPEG-XL transcoding plugin for Orthanc
 Registers the JPEG XL transfer syntax (1.2.840.10008.1.2.4.110) with
 Orthanc: encodes on ingest transcoding (IngestTranscoding), decodes JXL
 and compressed classic sources (JPEG, JPEG-LS) on retrieval transcoding.
 Requires the host Orthanc's "Plugins" configuration to include
 /usr/share/orthanc/plugins (the upstream packages' default).
EOF

cat > "${STAGE}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# The 2026-08-25 failure mode this package exists to prevent: a plugin file
# present on disk but never named by the "Plugins" configuration loads
# nothing, silently. Check and say so.
if ! grep -rqs '"Plugins"' /etc/orthanc/; then
    echo "orthanc-jxl: WARNING — no \"Plugins\" key found in /etc/orthanc/*.json." >&2
    echo "orthanc-jxl: Orthanc will NOT load this plugin until the configuration" >&2
    echo "orthanc-jxl: contains e.g.:  \"Plugins\" : [ \"/usr/share/orthanc/plugins\" ]" >&2
elif ! grep -rqs '/usr/share/orthanc/plugins\|libOrthancJxl' /etc/orthanc/; then
    echo "orthanc-jxl: WARNING — a \"Plugins\" key exists but does not reference" >&2
    echo "orthanc-jxl: /usr/share/orthanc/plugins — verify Orthanc scans this dir." >&2
fi
if [ -d /run/systemd/system ] && systemctl is-active --quiet orthanc; then
    echo "orthanc-jxl: restarting orthanc to load the plugin"
    systemctl restart orthanc || true
    echo "orthanc-jxl: verify with  curl -s http://localhost:8042/plugins"
else
    echo "orthanc-jxl: orthanc not running — it will load the plugin on next start"
fi
exit 0
EOF
chmod 755 "${STAGE}/DEBIAN/postinst"

# 3. Pack.
dpkg-deb --build --root-owner-group "${STAGE}" "deploy/${PKG}.deb" >/dev/null
rm -rf "${STAGE}" deploy/build-release
echo "=== Built: ${ROOT}/deploy/${PKG}.deb ==="
echo "Install: sudo apt install ./$( basename "deploy/${PKG}.deb" )"
