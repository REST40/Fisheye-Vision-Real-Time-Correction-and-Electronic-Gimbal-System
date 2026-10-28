#!/usr/bin/env bash
set -euo pipefail
cmake -S /project -B /project/build -DCMAKE_BUILD_TYPE=Release
cmake --build /project/build --parallel 4
if [[ "${1:-}" == check ]]; then
  ctest --test-dir /project/build --output-on-failure
elif [[ "${1:-}" == stream-check ]]; then
  export QT_QPA_PLATFORM=offscreen
  exec /project/build/x5_stream_probe
else
  exec /project/build/double_fisheye_host "$@"
fi
