#!/usr/bin/env bash
set -euo pipefail
host_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if ! command -v docker >/dev/null; then echo '缺少Docker，请先配置本机Docker运行环境。' >&2; exit 1; fi
if ! docker info >/dev/null 2>&1; then
  if python3 -c 'import grp,os,pwd,sys;g=grp.getgrnam("docker");sys.exit(0 if pwd.getpwuid(os.getuid()).pw_name in g.gr_mem and g.gr_gid not in os.getgroups() else 1)' 2>/dev/null; then
    command_text="$(python3 -c 'import shlex,sys;print(" ".join(shlex.quote(v) for v in sys.argv[1:]))' bash "$host_root/tools/run_qt.sh" "$@")"
    export DF_LAUNCH_GID="${DF_LAUNCH_GID:-$(id -g)}"
    exec sg docker -c "$command_text"
  fi
  echo "无法访问Docker。请在本机终端配置：sudo usermod -aG docker $(id -un)" >&2; exit 1
fi
# The dedicated image only contains build/runtime dependencies. Project code
# remains mounted here, so saving C++ files and rerunning performs an incremental build.
if ! docker image inspect double-fisheye-qt >/dev/null 2>&1; then
  docker build -t double-fisheye-qt -f "$host_root/tools/Dockerfile" "$host_root"
fi
args=(--rm --init --network host --user "$(id -u):${DF_LAUNCH_GID:-$(id -g)}"
  -e HOME=/tmp -e TZ=Asia/Shanghai -e "DF_HOST_ROOT=$host_root"
  -e "DF_HOST_PERSISTENT_ROOT=$host_root" -w "$host_root"
  -v "$host_root:/project" -v "$host_root:$host_root" -e DISPLAY="${DISPLAY:-:0}"
  -v /tmp/.X11-unix:/tmp/.X11-unix:ro)
if [[ -n "${XAUTHORITY:-}" && -f "$XAUTHORITY" ]]; then args+=(-e XAUTHORITY=/tmp/qt.xauth -v "$XAUTHORITY:/tmp/qt.xauth:ro"); fi
if [[ -d /dev/bus/usb ]]; then args+=(-v /dev/bus/usb:/dev/bus/usb --device-cgroup-rule 'c 189:* rmw'); fi
if docker info --format '{{json .Runtimes}}' | python3 -c 'import json,sys;sys.exit(0 if "nvidia" in json.load(sys.stdin) else 1)' && nvidia-smi >/dev/null 2>&1; then
  args+=(--runtime=nvidia --gpus all -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics,video,display)
fi
exec docker run "${args[@]}" --entrypoint bash double-fisheye-qt /project/tools/container_entry.sh "$@"
