#!/usr/bin/env bash
# Cross-build the Windows host (UCRT64, GCC 16, static link) on Linux.
#
# usage: scripts/ci/xc/build.sh <prefix dir> <build dir> [cmake args...]
#   Fedora's ucrt64 cross compiler brings GCC and binutils; everything else the Windows job installs with pacman (headers, CRT,
#   winpthreads, curl, MinHook, miniupnpc, libvpl, openssl, opus, cppwinrt, nlohmann-json) is unpacked from the MSYS2 UCRT64
#   repository, so the headers and import libraries are those of the Windows build.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$here/../../.." && pwd)"
prefix="${1:?prefix dir}"; build="${2:?build dir}"; shift 2
step() { echo "== $* ($(date +%T))"; }
t() { local s; s=$(date +%s); "$@"; echo "[time] $1 $(( $(date +%s) - s )) s"; }

step "MSYS2 packages"
t python3 "$here/msys2-sysroot.py" "$prefix/msys2" curl-winssl MinHook miniupnpc nlohmann-json libvpl openssl cppwinrt opus crt headers winpthreads
step "case-insensitive names (Windows.h, -lWindowsapp, ...)"
python3 "$here/casefix-includes.py" "$prefix/casefix" "$src/src" "$src/tools" "$src/tests" "$src/third-party/ViGEmClient" \
  "$src/third-party/moonlight-common-c" "$src/third-party/nvapi" -- "$prefix/msys2/ucrt64/include"
mkdir -p "$prefix/casefix-lib"
export MSYS2_ROOT="$prefix/msys2" CASEFIX_DIR="$prefix/casefix" CASEFIX_LIB_DIR="$prefix/casefix-lib"
export PKG_CONFIG_SYSROOT_DIR="$MSYS2_ROOT"
export PKG_CONFIG_LIBDIR="$MSYS2_ROOT/ucrt64/lib/pkgconfig:$MSYS2_ROOT/ucrt64/share/pkgconfig"
export BRANCH=ci-lab BUILD_VERSION=lab.zayu COMMIT=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa SUNSHINE_BUILD_COMMIT=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
export SUNSHINE_BUILD_BRANCH=ci-lab SUNSHINE_BUILD_CHANNEL=dev SUNSHINE_BUILD_BUILDER=ci
step "configure"
t cmake -B "$build" -G Ninja -S "$src" -DCMAKE_TOOLCHAIN_FILE="$here/toolchain-ucrt64.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DOCS=OFF -DSUNSHINE_RTX_HDR=OFF -DBUILD_TESTS=ON -DBUILD_TRAY_TESTS=ON -DBUILD_WEB_UI=OFF -DSUNSHINE_ASSETS_DIR=assets \
  -DDRIVER_DEPS_REQUIRED=OFF -DSUNSHINE_BUILD_MAT_CAPTURE=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$@"
# The link lines only exist now: map the mixed-case -l names (Windowsapp, D3DCompiler, ...) onto the lower-case import libraries.
python3 "$here/casefix-libs.py" "$CASEFIX_LIB_DIR" "$build/build.ninja" "$MSYS2_ROOT/ucrt64/lib" /usr/x86_64-w64-mingw32ucrt/sys-root/mingw/lib
step "compile and link"
t ninja -C "$build"
ccache --show-stats
