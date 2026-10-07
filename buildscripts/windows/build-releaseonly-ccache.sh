#!/bin/bash
# Windows release build through ccache, for CI runs that keep .ccache between builds (build-windows-fast.yml):
# only the files a change touches are compiled again.
set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd "$SCRIPT_DIR/../.."

if ! command -v ccache > /dev/null; then
    apt-get update -qq > /dev/null && apt-get install -y -qq ccache > /dev/null
fi
export CCACHE_DIR="$PWD/.ccache"
export CCACHE_BASEDIR="$PWD"
export CCACHE_MAXSIZE=2G
export CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime
ccache -z > /dev/null

VCPKG_ROOT=/vcpkg
cmake -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
    "-DVCPKG_HOST_TRIPLET=x86-windows-ftl" \
    "-DVCPKG_TARGET_TRIPLET=x86-windows-ftl" \
    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/toolchains/x86-windows-ftl.cmake" \
    "-DCMAKE_BUILD_TYPE=Release" \
    "-DSTEAM_1_6_13_BUILD=OFF" \
    "-DENABLE_TESTING=OFF" \
    "-DCMAKE_C_COMPILER_LAUNCHER=ccache" \
    "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache" \
    -S . -B build-windows-release -G Ninja

ninja -C build-windows-release
ccache -s | grep -iE "hits|misses" || true
