#!/usr/bin/env bash
# Linux counterpart of build_host.cmd: builds the host bridge into build-linux/host-bridge/refract-host-bridge
# and runs the native tests. Needs CMake, Ninja, a C++20 compiler and the OpenXR and Vulkan headers
# (Fedora: openxr-devel vulkan-headers; Debian/Ubuntu: libopenxr-dev libvulkan-dev). Headers elsewhere:
#   REFRACT_OPENXR_HEADERS=/path/include REFRACT_VULKAN_HEADERS=/path/include ./build_host.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cmake -S "$ROOT" -B "$ROOT/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DREFRACT_OPENXR_HEADERS="${REFRACT_OPENXR_HEADERS:-}" -DREFRACT_VULKAN_HEADERS="${REFRACT_VULKAN_HEADERS:-}"
cmake --build "$ROOT/build-linux"
ctest --test-dir "$ROOT/build-linux" --output-on-failure
