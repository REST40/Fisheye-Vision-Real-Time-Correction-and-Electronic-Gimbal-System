# Linux Qt 上位机

上位机负责双鱼眼采集或离线录像输入、同帧双路 JPEG 上传、板端全景回图，以及球面和电子云台显示。同时提供链路状态、帧率、分阶段延迟和同帧图像质量分析。

## 依赖与构建

需要 C++17、CMake 3.20+、Qt 6.4+（Core、Gui、Widgets、Network、Test）、OpenCV（core、imgproc、imgcodecs、videoio）、minizip、zlib 和 FFmpeg 命令行。Linux 依赖安装列表见 [tools/Dockerfile](tools/Dockerfile)。

不使用相机 SDK 时，在仓库根目录执行：

```bash
cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=OFF
cmake --build HOST/build --parallel 4
python3 HOST/tools/generate_demo.py
./HOST/build/double_fisheye_host
```

`generate_demo.py` 首次运行时生成前后镜头测试图片和同步视频：每路 1328×1328、30 FPS、300 帧。这些是合成测试输入，供界面演示和软件测试使用；已有数据不会被覆盖。重复运行时可通过 `--output` 指定新目录。

## 输入来源

### 相机输入

现有 USB 采集适配器面向 Insta360 X5，使用 CameraSDK 2.1.1 和 MediaSDK 3.1.1。按 [SDK 安装说明](dependencies/README.md) 放置头文件、运行库与模型后启用采集构建：

```bash
cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=ON
cmake --build HOST/build --parallel 4
./HOST/build/double_fisheye_host
```

完整构建还需要 pkg-config、FFmpeg 开发库和 SDL2。连接相机前，确认 USB 模式及设备访问权限；采集配置位于 `config/sdk.json`，运行时设置由界面保存至本地。其他相机可以使用离线输入方式，直接接入实时采集时需适配对应接口。

### 离线录像

在主界面点击“选择离线视频”，选择含以下文件的目录：

```text
front.png
rear.png
front.mp4
rear.mp4
```

视频名称也支持同后缀的 `front_*.mp4` / `rear_*.mp4`。每个目录只保留一对输入视频；时间轴可使用 `frames.csv` 中的时间戳，文件名不限制录像时长。

可将一段 2656×1328、左右排列的双鱼眼录像转换为两路输入：

```bash
python3 HOST/tools/prepare_offline_video.py \
  /path/to/fisheye.mp4 HOST/data/captures/data_source
```

脚本需要 FFmpeg 和 ffprobe，左半作为前镜头、右半作为后镜头，保留完整录像与逐帧时间戳，不预先进行几何校正。两路帧数和时间戳检查通过后才生成结果目录，已有目录不会被覆盖。

程序优先查找 `HOST/data/captures/data_source`，没有本地采集目录时使用 `HOST/data/demo`。暂停和时间轴位于“参数设置 → 软件仿真”。离线录像仍通过网络发送至 PS/PL；全景与球面视图等待板卡回图。

### 软件参考模式

在“参数设置 → 软件仿真”中选择“加载软件仿真”，可使用本地参考投影体验界面，不需要板卡。该模式与板卡处理链路分开；进行板端图像质量测试前需退出软件参考模式。

## 板卡连接

电脑有线网卡默认地址为 `192.168.10.100`，板端为 `192.168.10.10`。先启动匹配的 PS/PL 程序，再通过“FPGA接口”连接；UDP 控制、输入和回传端口依次为 `5500`、`5501`、`5502`。

上传采用有界队列和分片 ACK 流控。处理速度不足时会替换等待发送的帧，以控制积压；录像播放帧率与实际板卡回图帧率分别统计，不要求每一帧都经过板卡处理。链路诊断保存在本地 `HOST/output/diagnostics/latest_link.txt`。

“参数设置 → 软件仿真 → 离线视频上传画质”可调整上传 JPEG：

| 模式 | JPEG 质量 | 预处理 |
|---|---|---|
| 原画质（默认） | 85 | 无平滑 |
| 均衡 | 65 | 无平滑 |
| 低细节预览 | 50 | 13×13 高斯平滑，σ=3 |

每路上传尺寸保持 1328×1328。低细节预览会降低纹理细节，回到相机输入后恢复质量 85。

## 显示与质量分析

球面与云台投影从板端返回的 2656×1328 全景采样。默认球体显示为 720×720，云台为 1280×720；随视口放大可分别提高至 1440×1440 和 1920×1080。视角、源尺寸或目标尺寸变化时更新显示映射表，上传和板端处理尺寸保持不变。

进行同帧质量测试时，先加载输入并连接板卡，在“图像质量”中进入独立测试，再选择“采集并分析”。程序固定当前双鱼眼帧，以 JPEG 质量 85 发送至板卡，将匹配回图与 `src/quality/ps_reference` 生成的软件参考全景计算 PSNR / SSIM。测试期间暂停普通视频传输，结果不计入实时性能统计，结束后恢复原播放状态。

前后镜头的内参、Mei 参数和 Brown 畸变系数位于 `config/calibration.json`。替换相机或调整输入裁剪后，应更新参数，并保持 HOST 软件参考与 PS 映射模型一致。

## 测试

先完成不依赖 SDK 的构建并生成演示输入，再执行：

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir HOST/build --output-on-failure
python3 HOST/tools/quality/verify_reference.py
```

Qt 测试覆盖投影、协议、帧配对、离线输入、延迟统计和质量分析。映射表对照脚本需要 GCC。连接相机和板卡的集成测试需要对应硬件，不能由离线测试代替。

## Docker

Linux 桌面环境下，可使用以下入口构建依赖镜像并启动应用：

```bash
python3 HOST/run_frontend.py
```

入口会检查 Docker、图形显示、USB 和 SDK 依赖；未安装 SDK 时构建主体与软件测试。镜像配置位于 `tools/Dockerfile`，启动脚本为 `tools/run_qt.sh` 和 `tools/container_entry.sh`。

仅在容器中进行离线构建与测试：

```bash
docker build -t double-fisheye-qt -f HOST/tools/Dockerfile HOST
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/repo" -w /repo double-fisheye-qt bash -lc \
  'cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=OFF && cmake --build HOST/build --parallel 4 && python3 HOST/tools/generate_demo.py && QT_QPA_PLATFORM=offscreen ctest --test-dir HOST/build --output-on-failure'
```

首次创建镜像需要下载系统依赖；已有依赖镜像可重复使用。生成演示输入的命令只需执行一次。

## 目录

| 路径 | 内容 |
|---|---|
| `src/app`、`src/ui` | 程序入口和界面 |
| `src/core`、`src/media`、`src/simulation` | 投影、成对输入及软件参考模式 |
| `src/fpga/network` | 板端协议、流控和遥测 |
| `src/quality` | 同帧图像质量分析及 PS 参考实现 |
| `src/x5`、`tools/x5` | USB 相机采集与控制 |
| `tests` | 软件测试和板端原生测试 |
| `config`、`resources` | 配置、图标和样式 |
| `tools` | 构建、数据生成、导入及调试工具 |

SDK、采集素材、运行结果、个人采集设置和构建目录均由根目录 `.gitignore` 排除。
