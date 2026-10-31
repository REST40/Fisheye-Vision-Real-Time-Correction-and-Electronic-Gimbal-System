# 环境安装材料

`deb/` 保存 FFmpeg 和部分多媒体运行库的离线安装包，全部为 Linux `amd64`（x86-64）架构。它们用于准备运行环境，应用源码和构建脚本不依赖此目录的位置。

## 安装包

| 软件包 | 版本 | 用途 |
|---|---|---|
| `ffmpeg` | `7:4.2.7-0ubuntu0.1` | 视频转换、拆分、编解码和测试视频生成 |
| `libavdevice58` | `7:4.2.7-0ubuntu0.1` | 音视频设备访问 |
| `libavresample4` | `7:4.2.7-0ubuntu0.1` | 音频重采样 |
| `libdc1394-22` | `2.2.5-2.1` | IEEE 1394 相机设备支持 |
| `libopenal1` | `1:1.19.1-1` | 音频运行库 |
| `libsdl2-2.0-0` | `2.0.10+dfsg1-3` | 多媒体显示和播放支持 |
| `libsndio7.0` | `1.5.0-3` | 音频设备运行库 |

文件名中的 `%3a` 对应版本号中的冒号，保留原始安装包文件名。

## 使用方式

常规构建依赖见 [HOST 使用说明](../../HOST/README.md)，系统依赖列表与 Ubuntu 24.04 镜像配置见 [Dockerfile](../../HOST/tools/Dockerfile)。默认 Docker 构建使用发行版软件源安装依赖，不会安装本目录中的 DEB。

本目录中的包包含 Ubuntu 版本及特定共享库版本，离线使用时需选择版本匹配的系统，并补齐依赖。这 7 个文件不是完整的离线依赖集合：例如 FFmpeg 仍依赖对应版本的 `libavcodec58`、`libavformat58`、`libavfilter7` 和 `libavutil56` 等包。应根据目标系统和各包的 `Depends` 字段准备完整依赖。

在支持 DEB 的系统中可先检查安装包信息：

```bash
dpkg-deb --info tools/environment/deb/ffmpeg_*.deb
```

相机 SDK 按 [SDK 安装说明](../../HOST/dependencies/README.md) 放置于 `HOST/dependencies`，由采集程序的配置和构建脚本读取。
