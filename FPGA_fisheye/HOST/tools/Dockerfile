FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config python3 ffmpeg \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libsdl2-dev \
    qt6-base-dev libopencv-core-dev libopencv-imgproc-dev \
    libopencv-imgcodecs-dev libopencv-videoio-dev libminizip-dev \
    libdc1394-25 libusb-1.0-0 libglib2.0-0 \
    libgl1 libegl1 libglvnd0 libglx0 libopengl0 libvulkan1 \
    libxcb-cursor0 libxkbcommon-x11-0 ca-certificates fontconfig fonts-noto-cjk \
    && fc-cache -f && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends libqt6svg6 tzdata \
    && rm -rf /var/lib/apt/lists/*
ENV QT_QPA_PLATFORM=xcb QT_X11_NO_MITSHM=1
WORKDIR /project
