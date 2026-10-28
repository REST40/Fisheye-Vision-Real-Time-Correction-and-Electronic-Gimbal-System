# C++ / Qt 上位机

上位机提供 X5 采集、双 JPEG 上传、全景回图、球体/虚拟云台显示、链路分析、延迟分析和同帧图像质量测试。`src/quality/ps_reference` 提供与 PS 映射算法一致的软件参考实现。

## 无 SDK 的软件构建与测试

环境需要 CMake 3.20+、C++17、Qt6 Core/Gui/Widgets/Network/Test、OpenCV core/imgproc/imgcodecs/videoio、minizip、zlib、FFmpeg 命令行。完整依赖列表见 `tools/Dockerfile`。

在仓库根目录执行：

```bash
cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=OFF
cmake --build HOST/build --parallel 4
python3 HOST/tools/generate_demo.py
QT_QPA_PLATFORM=offscreen ctest --test-dir HOST/build --output-on-failure
```

`generate_demo.py` 使用 FFmpeg 生成合成图片及两路同步 MJPEG 视频，每路 1328×1328、30 FPS、300 帧，供软件测试和界面演示使用。生成文件由 .gitignore 排除；脚本拒绝覆盖已有数据文件。相机输入、图像质量与硬件性能测试需使用对应的实机数据。

无 SDK 构建可运行上位机主体与软件测试，X5 实时采集需要下述外部依赖。

## X5 实机依赖

本项目使用 CameraSDK 2.1.1 和 MediaSDK 3.1.1。SDK 头文件、共享库和模型按 [dependencies/README.md](dependencies/README.md) 放置，之后以 `-DBUILD_X5_CAPTURE=ON` 重新配置并构建。

```bash
cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=ON
cmake --build HOST/build --parallel 4
./HOST/build/double_fisheye_host
```

完整构建还需要 FFmpeg 开发库、SDL2、pkg-config。相机连接、USB 权限和显示环境由运行机器配置。

## Docker

Docker 配置与启动入口为 `tools/Dockerfile`、`tools/run_qt.sh` 和 `run_frontend.py`。安装 SDK 后，可执行 `python3 HOST/run_frontend.py` 启动。

也可用已有/自行构建的 `double-fisheye-qt` 镜像离线构建：

```bash
docker build -t double-fisheye-qt -f HOST/tools/Dockerfile HOST
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/repo" -w /repo double-fisheye-qt bash -lc 'cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=OFF && cmake --build HOST/build --parallel 4 && python3 HOST/tools/generate_demo.py && QT_QPA_PLATFORM=offscreen ctest --test-dir HOST/build --output-on-failure'
```

## 目录

`src/app` 为入口；`src/ui` 为界面；`src/fpga/network` 为板卡协议和收发；`src/quality` 为同帧质量测试；`src/x5` 为采集；`tests` 为软件验证；`config` 为项目参数；`resources` 为界面资源。

相机 SDK、构建产物、相机录制素材和运行结果保存在本地，并由 .gitignore 排除。`config/calibration.json` 配置前后镜头的 Mei 模型、Brown 畸变系数和镜头顺序；参数含义与兼容性标志见配置文件。
