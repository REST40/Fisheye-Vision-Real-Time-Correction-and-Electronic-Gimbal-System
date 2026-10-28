# 基于 FPGA 的双鱼眼全景图像处理系统

本项目为 FPGA 创新设计竞赛作品，基于 Zynq UltraScale+ 平台实现双鱼眼图像的畸变校正、全景拼接融合及交互显示。

系统接收 Insta360 X5 的 2656×1328 双鱼眼图像，将同一帧拆分为前后两幅 1328×1328 图像并编码为 JPEG，经网络发送至板端。PL 完成双路 JPEG 解码、校正插值、重叠区域加权融合及全景 JPEG 编码；PS 负责映射表生成、硬件调度和网络收发；Qt 上位机负责采集、全景球面展示、虚拟云台浏览与测试分析。

**关键词：鱼眼畸变校正 · 全景拼接融合 · FPGA 并行流水处理**

## 主要功能

- 基于标定模型的双鱼眼映射与双线性插值。
- 前后镜头重叠区域平滑加权融合，输出 2656×1328 的完整全景。
- 双路并行插值、分 bank 片上缓存与多级流水处理。
- 双路 JPEG 输入、全景 JPEG 输出及网络传输。
- 全景球面显示、虚拟云台方向与视场交互。
- 帧率统计、分阶段延迟分析及同帧 PSNR / SSIM 图像质量评估。

## 系统数据流

1. HOST 采集并拆分同帧双鱼眼图像，上传双路 JPEG。
2. PS 完成分片重组、CRC 校验与帧槽调度。
3. PL 双路解码器写回源图像，并提供已提交行进度。
4. 校正融合模块读取映射坐标、融合权重和源像素，生成全景像素流。
5. JPEG 编码器压缩全景，AXI DMA 将码流写回 DDR，PS 回传结果。
6. HOST 解码回图，生成球面与透视视角显示，并统计测试指标。

## 项目结构

```text
PL/
  design/mjpeg_system.bd             系统 Block Design
  mjpeg/rtl/                         AXI 接口与编解码辅助电路
  mjpeg/adapted/                     项目适配后的编解码模块
  mjpeg/third_party/                 第三方源码与许可证
  fisheye_ip/ip_repo/                鱼眼校正融合 IP
  scripts/create_project.tcl         Vivado 工程创建脚本
  hardware_export/                   PL 硬件平台文件
PS/
  7ev_fish_app/src/                  板端应用源码
  config/                           BSP 与应用构建配置
  scripts/create_platform.tcl        Vitis 平台创建脚本
HOST/
  src/                              Qt 上位机源码
  tests/                            软件测试
  config/                           标定与 SDK 配置
  dependencies/README.md             外部 SDK 安装说明
  tools/                            构建、测试与演示数据工具
tools/verify_repository.py           项目文件完整性检查
```

## 开发环境

| 部分 | 环境 | 使用说明 |
|---|---|---|
| PL | Vivado 2022.2；XCZU7EV-FFVC1156-2-I | [PL/README.md](PL/README.md) |
| PS | Vitis / XSCT 2022.2；Cortex-A53 64-bit standalone；lwIP | [PS/README.md](PS/README.md) |
| HOST | Linux；C++17；CMake；Qt 6.4+；OpenCV；FFmpeg | [HOST/README.md](HOST/README.md) |

PL 硬件平台文件为 `PL/hardware_export/pl_project_snapshot.xsa`。PS 平台根据该文件生成。更新硬件设计后，应重新导出 XSA 并同步更新 PS 平台。

## 检查与测试

在仓库根目录执行：

```bash
python3 tools/verify_repository.py
python3 HOST/tools/quality/verify_reference.py
```

- 第一项核对文件 SHA-256、硬件平台文件、IP 源文件引用及系统连接参数。
- 第二项需要 GCC，用于比较 HOST 软件参考与 PS 映射表生成结果。
- 上位机自动化测试覆盖协议、双路帧配对、延迟统计和图像质量等功能，操作方法见 [HOST/README.md](HOST/README.md)。
- 同帧图像质量分析比较软件参考全景与板端 JPEG 回图，采用 PSNR / SSIM 指标；结果包含图像处理与编解码影响。
- 软件测试环境与结果记录见 [verification.json](verification.json)。硬件测试记录包括构建配置、输入数据、帧率和分阶段耗时等指标。

## 系统连接

默认上位机地址为 `192.168.10.100`，板端地址为 `192.168.10.10`。

| UDP 端口 | 用途 |
|---|---|
| 5500 | 控制与遥测 |
| 5501 | 同帧双路 JPEG 输入 |
| 5502 | 全景 JPEG 回传 |

通信采用 FPTZ v3 协议，完整全景模式输出尺寸为 2656×1328。观看方向由上位机本地渲染调整。

## 第三方组件

编解码器来源、固定版本、许可证及项目适配说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。相机 SDK 获取与安装方法见 [HOST/dependencies/README.md](HOST/dependencies/README.md)。
