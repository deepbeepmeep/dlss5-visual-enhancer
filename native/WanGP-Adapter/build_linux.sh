#!/usr/bin/env bash
set -euo pipefail

# Build against NVIDIA/DLSS revision 9981c6350dc58c0ce21f024fd65e31e851c31689.
# The SDK and runtime retain NVIDIA's license; no third-party source is vendored.
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo "This worker currently supports Linux x86_64 only." >&2
    exit 1
fi
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash native/WanGP-Adapter/build_linux.sh NVIDIA_DLSS_SDK [OUTPUT_DIRECTORY]" >&2
    exit 1
fi
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk=$(cd -- "$1" && pwd)
destination=${2:-"$source_dir/../../bin/runtime/dlssg"}
runtime=libnvidia-ngx-dlssg.so.310.9.16
for file in include/nvsdk_ngx_helpers_dlssg_vk.h lib/Linux_x86_64/libnvsdk_ngx.a "lib/Linux_x86_64/rel/$runtime" LICENSE.txt; do
    if [[ ! -f "$sdk/$file" ]]; then
        echo "Missing NVIDIA SDK file: $sdk/$file" >&2
        exit 1
    fi
done
mkdir -p -- "$destination"
destination=$(cd -- "$destination" && pwd)
staging=$(mktemp -d "$destination/.build-XXXXXXXX")
trap 'rm -rf -- "$staging"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers \
    -I"$sdk/include" "$source_dir/dlssg_worker_vk.cpp" \
    "$sdk/lib/Linux_x86_64/libnvsdk_ngx.a" -lvulkan -ldl -pthread -o "$staging/dlssg-worker"
install -m 644 -- "$sdk/lib/Linux_x86_64/rel/$runtime" "$staging/$runtime"
install -m 644 -- "$sdk/LICENSE.txt" "$staging/LICENSE-NVIDIA-DLSS-Linux.txt"
install -m 644 -- "$source_dir/LICENSE" "$staging/LICENSE-WanGP-Adapters.txt"
mv -- "$staging/$runtime" "$staging/LICENSE-NVIDIA-DLSS-Linux.txt" "$staging/LICENSE-WanGP-Adapters.txt" "$destination/"
mv -- "$staging/dlssg-worker" "$destination/"
echo "Built Linux x2 worker in $destination"
echo "Run '$destination/dlssg-worker --probe' to check NVIDIA Vulkan/NGX availability."
