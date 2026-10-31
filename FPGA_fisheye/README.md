# FPGA 双鱼眼全景图像处理系统

基于 Zynq UltraScale+ 的双鱼眼图像处理系统，包含 FPGA 逻辑、ARM 板端程序和 Linux Qt 上位机。系统对同一帧的前后鱼眼图像进行几何校正、双线性插值和重叠区域融合，输出全景图像，并提供球面浏览与电子云台交互。

输入为两幅 1328×1328 鱼眼图像，完整全景输出为 2656×1328。相机通过 USB 接入上位机，上位机与 FPGA 板卡通过以太网通信。

## 功能

- **几何校正**：根据 Mei 全向相机模型和 Brown 畸变参数生成反向映射表，使用 Q6 定点坐标定位源像素。
- **插值与融合**：FPGA 对四邻点进行双线性插值，在前后镜头重叠区域使用 Q8 互补权重融合。
- **硬件图像链路**：双路 JPEG 解码、源行缓存、校正融合、全景 JPEG 编码与 DMA 写回。
- **板端调度**：映射表生成、UDP 分片重组、帧槽管理、处理调度和遥测。
- **交互显示**：全景预览、球面视图、电子云台方向和视场角调节。
- **分析与测试**：输入及回图帧率、分阶段延迟、同帧 PSNR / SSIM 质量分析。

## 数据流程

```text
双鱼眼相机 / 离线录像
         │
         ▼
Linux Qt 上位机：同帧拆分、双路 JPEG 上传
         │ UDP
         ▼
PS：协议与帧槽调度、映射表生成
         │ DDR / AXI
         ▼
PL：双路解码 → 四点插值与融合 → 全景编码 → DMA
         │ PS / UDP 回传
         ▼
Qt 上位机：全景显示、球面与电子云台投影
```

观看方向和视场角在上位机本地调整；全景模式下无需随视角变化重建板端映射表。

## 目录

| 目录 | 内容 |
|---|---|
| [PL](PL/README.md) | RTL、Block Design、自定义校正 IP 和 Vivado 工程脚本 |
| [PS](PS/README.md) | Cortex-A53 standalone 应用、BSP 配置和 Vitis 平台脚本 |
| [HOST](HOST/README.md) | Qt 上位机、软件参考实现、采集适配器和测试 |
| [tools/verify_repository.py](tools/verify_repository.py) | 源文件与工程引用检查 |
| [tools/environment](tools/environment/README.md) | FFmpeg 及部分多媒体运行库的 DEB 安装材料 |

仓库包含源码、工程配置、界面资源、测试、文档和环境安装材料。相机 SDK、录像、构建目录、日志、位流及 XSA 等生成文件不随源码分发。

## 开发环境

| 部分 | 环境 |
|---|---|
| PL | Vivado 2022.2；器件 `xczu7ev-ffvc1156-2-i` |
| PS | Vitis / XSCT 2022.2；Cortex-A53 64-bit standalone；lwIP |
| HOST | Linux；C++17；CMake 3.20+；Qt 6.4+；OpenCV；FFmpeg |

## 快速开始

以下命令均从仓库根目录执行。只体验界面和软件参考时，可先构建不依赖相机 SDK 的上位机：

```bash
cmake -S HOST -B HOST/build -DCMAKE_BUILD_TYPE=Release -DBUILD_X5_CAPTURE=OFF
cmake --build HOST/build --parallel 4
python3 HOST/tools/generate_demo.py
./HOST/build/double_fisheye_host
```

首次生成的演示输入是合成测试图像。可在界面的“参数设置 → 软件仿真”中加载软件参考模式；板卡链路的实际回图需要连接并运行 PS/PL。

构建完整系统的顺序：

1. 按 [PL 构建说明](PL/README.md) 创建 Vivado 工程、生成位流并导出 XSA。
2. 按 [PS 构建说明](PS/README.md) 创建 Vitis 平台和板端应用。
3. 按 [HOST 使用说明](HOST/README.md) 配置网卡、输入来源与相机 SDK。

## 网络连接

默认电脑地址为 `192.168.10.100`，板端地址为 `192.168.10.10`，两端有线网卡应处于同一子网。

| UDP 端口 | 用途 |
|---|---|
| 5500 | 控制与遥测 |
| 5501 | 同帧双路 JPEG 输入 |
| 5502 | 全景 JPEG 回传 |

链路使用 FPTZ v3 协议。修改网络参数时，应同步检查 HOST 配置入口与 PS 源码中的对应设置。

## 检查与测试

```bash
python3 tools/verify_repository.py
python3 HOST/tools/quality/verify_reference.py
QT_QPA_PLATFORM=offscreen ctest --test-dir HOST/build --output-on-failure
```

第一项只使用 Python 标准库，检查工程引用、IP 文件和关键配置；第二项需要 GCC，对比 HOST 软件参考与 PS 生成的完整映射表。Qt 测试需先完成上位机构建并生成演示输入。

软件测试覆盖协议、双路帧配对、投影、延迟统计和图像质量计算。PSNR / SSIM 使用同帧软件参考全景与板端 JPEG 回图对照，包含图像处理和编解码影响；实际帧率与延迟取决于输入内容、构建配置及运行环境。

## 第三方组件与许可

第三方源码版本、许可证和项目适配范围见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。相机 SDK 的获取和安装见 [HOST/dependencies/README.md](HOST/dependencies/README.md)。自有代码尚未指定统一开源许可证。
