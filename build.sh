#!/usr/bin/env bash
# Build BBD Reverb as a macOS .clap bundle using only clang (no CMake needed).
# Produces build/"BBD Reverb.clap" and, on request, installs it for the user.
#
#   ./build.sh            # build universal (arm64 + x86_64) bundle
#   ./build.sh install    # build, then copy to ~/Library/Audio/Plug-Ins/CLAP
#   ./build.sh test       # build, then build & run the smoke-test host

set -euo pipefail
cd "$(dirname "$0")"

CLAP_TAG="1.2.10"
NAME="BBD Reverb"
BUNDLE_ID="com.danielhuebert.bbd-reverb"
VERSION="1.0.0"

# 1. Fetch the CLAP headers if they are not vendored yet.
if [[ ! -f deps/clap/include/clap/clap.h ]]; then
  echo ">> fetching CLAP ${CLAP_TAG} headers"
  git clone --depth 1 --branch "${CLAP_TAG}" \
    https://github.com/free-audio/clap.git deps/clap
fi

# 2. Lay out the bundle.
BUNDLE="build/${NAME}.clap"
rm -rf "${BUNDLE}"
mkdir -p "${BUNDLE}/Contents/MacOS"

echo ">> compiling (arm64 + x86_64)"
clang++ -std=c++17 -O2 -Wall -Wextra \
  -Ideps/clap/include \
  -dynamiclib -arch arm64 -arch x86_64 \
  src/bbd_reverb.cpp \
  -o "${BUNDLE}/Contents/MacOS/${NAME}"

cat > "${BUNDLE}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>English</string>
  <key>CFBundleExecutable</key><string>${NAME}</string>
  <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
  <key>CFBundleName</key><string>${NAME}</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleSignature</key><string>????</string>
</dict>
</plist>
PLIST

printf 'BNDL????' > "${BUNDLE}/Contents/PkgInfo"

codesign --force --sign - "${BUNDLE}" 2>/dev/null || \
  echo ">> (codesign skipped — not required for local testing)"

echo ">> built ${BUNDLE}"
lipo -archs "${BUNDLE}/Contents/MacOS/${NAME}"

# 3a. Optional: build & run the smoke-test host (native arch only).
if [[ "${1:-}" == "test" ]]; then
  echo ">> building test host"
  clang++ -std=c++17 -O2 -Wall -Wextra -Ideps/clap/include \
    test/test_host.cpp -o build/test_host
  echo ">> running test host"
  ./build/test_host "${BUNDLE}/Contents/MacOS/${NAME}"
  exit 0
fi

# 3b. Optional install for the current user.
if [[ "${1:-}" == "install" ]]; then
  DEST="${HOME}/Library/Audio/Plug-Ins/CLAP"
  mkdir -p "${DEST}"
  rm -rf "${DEST}/${NAME}.clap"
  cp -R "${BUNDLE}" "${DEST}/"
  echo ">> installed to ${DEST}/${NAME}.clap"
fi
