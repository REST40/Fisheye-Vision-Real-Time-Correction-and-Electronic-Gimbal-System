# 第三方来源与项目适配

| 项目 | 固定提交 | 许可证 | 本项目用途 |
|---|---|---|---|
| [ultraembedded/core_jpeg](https://github.com/ultraembedded/core_jpeg) | bb03cce45d0b7459d395486e9e1db3de1b416bd2 | Apache-2.0 | JPEG 解码运算核心 |
| [ultraembedded/core_jpeg_decoder](https://github.com/ultraembedded/core_jpeg_decoder) | 6c81abeba4596b038a27f8258620d3cd91dfbff6 | Apache-2.0 | DDR/AXI 解码封装基础 |
| [lulinchen/jpeg_open](https://github.com/lulinchen/jpeg_open) | 4c268067a64b07352b0698647cdb31c58b46f355 | MIT | JPEG 编码核心与相关编码表 |

第三方源码与许可证位于 `PL/mjpeg/third_party`，版本信息见 `PL/mjpeg/upstream_lock.json`。

`PL/mjpeg/adapted` 为项目适配模块，文件中的原始版权声明适用于相应第三方代码。解码部分适配 RGBX 写回、输入预取、突发写入和行进度反馈；编码部分适配项目图像尺寸、像素输入、缓存、握手和输出码流组织。外围自定义电路位于 `PL/mjpeg/rtl`。

鱼眼校正融合源码位于 `PL/fisheye_ip/ip_repo/fisheye_cort_1_0/src`；PS 调度与网络逻辑及 Host 应用位于各自源码目录。第三方组件分别遵循其原始许可证；自有代码的许可范围不由第三方许可证覆盖，本仓库尚未指定自有代码的统一开源许可证。

AMD/Xilinx 工具及 BSP 由对应开发环境提供。Qt、OpenCV、FFmpeg、minizip 等通过开发环境安装；Insta360 SDK 单独获取，见 `HOST/dependencies/README.md`。
