#!/usr/bin/env bash
# Reproducible x64 MinGW build from Linux. Only small source dependencies are fetched.
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
deps_dir=${DRONEID_WINDOWS_DEPS:-"$repo_dir/build/windows-deps"}
jobs_count=${DRONEID_BUILD_JOBS:-4}
mkdir -p "$deps_dir"
fetch_source() {
  local name=$1 url=$2 digest=$3
  if [[ ! -f "$deps_dir/$name.tar.gz" ]]; then
    curl -fL --retry 2 "$url" -o "$deps_dir/$name.tar.gz.partial"
    mv "$deps_dir/$name.tar.gz.partial" "$deps_dir/$name.tar.gz"
  fi
  echo "$digest  $deps_dir/$name.tar.gz" | sha256sum --check -
  if [[ ! -d "$deps_dir/$name" ]]; then tar -xf "$deps_dir/$name.tar.gz" -C "$deps_dir"; fi
}
fetch_source glfw-3.4 https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
fetch_source fftw-3.3.10 https://www.fftw.org/fftw-3.3.10.tar.gz 56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467
command -v x86_64-w64-mingw32-gcc-posix >/dev/null
command -v x86_64-w64-mingw32-g++-posix >/dev/null
mkdir -p "$deps_dir/fftw-build"
(
  cd "$deps_dir/fftw-build"
  CC=x86_64-w64-mingw32-gcc-posix "$deps_dir/fftw-3.3.10/configure" \
    --host=x86_64-w64-mingw32 --prefix="$deps_dir/fftw-install" \
    --enable-float --enable-sse2 --with-our-malloc --disable-shared --enable-static
  make -j"$jobs_count"
  make install
)
cmake -S "$repo_dir" -B "$repo_dir/build/windows" \
  -DCMAKE_TOOLCHAIN_FILE="$repo_dir/cmake/mingw64.cmake" \
  -DDRONEID_WITH_UI=ON -DDRONEID_WITH_UHD=ON -DDRONEID_UHD_DYNAMIC=ON \
  -DDRONEID_GLFW_SOURCE="$deps_dir/glfw-3.4" -DDRONEID_FFTW_ROOT="$deps_dir/fftw-install"
cmake --build "$repo_dir/build/windows" -j"$jobs_count"
