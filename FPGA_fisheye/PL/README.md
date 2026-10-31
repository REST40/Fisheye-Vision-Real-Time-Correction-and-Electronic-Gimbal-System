# FPGA 逻辑（PL）

PL 在 `xczu7ev-ffvc1156-2-i` 上实现双路 JPEG 解码、源图缓存、鱼眼校正与融合、全景 JPEG 编码和 DMA 写回。开发工具为 Vivado 2022.2，完整全景输出尺寸为 2656×1328。

## 源码结构

| 路径 | 内容 |
|---|---|
| `design/mjpeg_system.bd` | PS、编解码器、校正 IP、DMA 和 AXI 互连配置 |
| `fisheye_ip/ip_repo/fisheye_cort_1_0/src` | 反向映射读取、源行缓存、四点插值和融合 |
| `mjpeg/rtl` | AXI 包装、解码写回、编码输入和存储辅助模块 |
| `mjpeg/adapted` | 适配项目尺寸、数据格式及接口的编解码模块 |
| `mjpeg/third_party` | 第三方原始源码、说明和许可证 |
| `scripts/create_project.tcl` | Vivado 工程创建入口 |

第三方版本由 [mjpeg/upstream_lock.json](mjpeg/upstream_lock.json) 固定。实际编译文件列表位于工程创建脚本，校正 IP 的文件引用位于 `component.xml`。

## 创建工程

在仓库根目录执行：

```bash
vivado -mode batch -source PL/scripts/create_project.tcl
```

默认创建 `PL/build/fisheye_mjpeg.xpr`，也可通过环境变量 `FPGA_RACE_PL_BUILD` 指定生成目录。已有工程应直接打开；创建另一份工程时需使用新的目录。

脚本会导入 RTL 和 Block Design、注册本地 IP 仓库、生成顶层包装器，并校验系统连接。打开工程后，按顺序运行综合、实现和位流生成。Block Design 中的 PS DDR、时钟和外设参数对应当前板卡；迁移到其他板卡时，应根据其硬件修改这些配置。

## 导出硬件平台

位流生成后，在 Vivado 中选择 **File → Export → Export Hardware**，包含 bitstream，并将硬件平台保存为：

```text
PL/hardware_export/pl_project_snapshot.xsa
```

导出前创建 `PL/hardware_export` 目录。该目录及其中的 XSA、位流和初始化文件均为本地生成文件，不纳入版本控制。[PS 平台脚本](../PS/scripts/create_platform.tcl) 默认读取上述路径。更改 PL 配置后，应重新导出 XSA，并重新生成对应的 PS 平台和应用。

