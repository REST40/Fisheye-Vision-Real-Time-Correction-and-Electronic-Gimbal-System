#!/usr/bin/env bash
set -euo pipefail
x5_capture=ON
for sdk_file in \
  dependencies/CameraSDK-2.1.1/include/camera/camera.h \
  dependencies/CameraSDK-2.1.1/include/camera/device_discovery.h \
  dependencies/CameraSDK-2.1.1/lib/libCameraSDK.so \
  dependencies/MediaSDK-3.1.1/include/ins_realtime_stitcher.h \
  dependencies/MediaSDK-3.1.1/include/ins_stitcher.h \
  dependencies/MediaSDK-3.1.1/lib/libMediaSDK.so; do
  if [[ ! -f "/project/$sdk_file" ]]; then
    echo "缺少 X5 SDK 文件：$sdk_file" >&2
    x5_capture=OFF
  fi
done
if [[ "$x5_capture" == OFF ]]; then
  echo '本次构建主界面与软件测试，X5 实时采集不可用；安装方式见 HOST/dependencies/README.md。' >&2
fi
cmake -S /project -B /project/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE="$x5_capture"
cmake --build /project/build --parallel 4
if [[ "${1:-}" == check ]]; then
  ctest --test-dir /project/build --output-on-failure
elif [[ "${1:-}" == stream-check ]]; then
  export QT_QPA_PLATFORM=offscreen
  exec /project/build/x5_stream_probe
else
  exec /project/build/double_fisheye_host "$@"
fi
