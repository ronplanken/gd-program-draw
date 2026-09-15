#!/bin/bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "$0")/.." && pwd)"
build_work="$project_dir/work"
mkdir -p "$build_work"
if [[ ! -d "$build_work/obs-studio/.git" ]]; then
  git clone --depth 1 --branch 32.2.2 https://github.com/obsproject/obs-studio.git "$build_work/obs-studio"
fi
if [[ ! -d "$build_work/simde/.git" ]]; then
  git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git "$build_work/simde"
fi
[[ "$(git -C "$build_work/obs-studio" rev-parse HEAD)" == "ba2f32bdf791005443988a4955e963663e16b1ed" ]] || { echo "Unexpected OBS header revision"; exit 1; }
[[ "$(git -C "$build_work/simde" rev-parse HEAD)" == "71fd833d9666141edcd1d3c109a80e228303d8d7" ]] || { echo "Unexpected SIMDe revision"; exit 1; }
cmake -S "$project_dir" -B "$build_work/build" -G Ninja \
  -DOBS_SOURCE_DIR="$build_work/obs-studio" -DSIMDE_DIR="$build_work/simde" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build "$build_work/build" --parallel 4
ctest --test-dir "$build_work/build" --output-on-failure
echo "Built: $build_work/build/release/obs-program-draw.plugin"
