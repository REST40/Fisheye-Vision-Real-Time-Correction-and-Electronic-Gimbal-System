#!/usr/bin/env python3
"""VS Code入口；窗口和仿真由C++ Qt原生程序运行。"""
import os
from pathlib import Path
import signal
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent
    env = os.environ.copy()
    if not env.get('DISPLAY'):
        for socket in sorted(Path('/tmp/.X11-unix').glob('X*')):
            if socket.stat().st_uid == os.getuid():
                env['DISPLAY'] = ':' + socket.name[1:]
                break
    if not env.get('XAUTHORITY'):
        for candidate in (Path('/run/user') / str(os.getuid()) / 'gdm/Xauthority',
                          Path.home() / '.Xauthority'):
            if candidate.is_file():
                env['XAUTHORITY'] = str(candidate)
                break
    print('正在构建并启动 C++ Qt 双鱼眼上位机。', flush=True)
    child = subprocess.Popen(['bash', str(root / 'tools/run_qt.sh')] + sys.argv[1:], env=env)
    def stop(signum, _frame):
        if child.poll() is None:
            child.send_signal(signum)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    return child.wait()


if __name__ == '__main__':
    sys.exit(main())
