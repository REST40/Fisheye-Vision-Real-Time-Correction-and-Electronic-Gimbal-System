"""Shared camera ownership and SDK validation for CLI and workbench."""
import fcntl
import ctypes
import json
import os
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
_libc=ctypes.CDLL(None,use_errno=True)

def x5_usb_permission_denied():
    """A detected Android-mode X5 needs read/write access to its USB node."""
    for path in Path('/sys/bus/usb/devices').glob('*'):
        try:
            if (path/'idVendor').read_text().strip()=='2e1a' and (path/'idProduct').read_text().strip()=='0002':
                bus=int((path/'busnum').read_text());device=int((path/'devnum').read_text())
                if not os.access(f'/dev/bus/usb/{bus:03}/{device:03}',os.R_OK|os.W_OK):return True
        except (OSError,ValueError):continue
    return False

def parent_death_signal(signum):
    """Linux: native workers stop even if the supervising process crashes."""
    parent=os.getppid()
    if _libc.prctl(1,int(signum),0,0,0):raise OSError(ctypes.get_errno(),'prctl PR_SET_PDEATHSIG')
    if os.getppid()!=parent:os.kill(os.getpid(),signum)

class CameraLock:
    def __init__(self):
        self.file=None
    def __enter__(self):
        directory=ROOT/'.runtime';directory.mkdir(exist_ok=True,mode=0o700)
        self.file=(directory/'camera.lock').open('a+')
        try:fcntl.flock(self.file,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            self.file.close();self.file=None
            raise RuntimeError('相机正在被另一个录制实例使用，请先停止该实例。')
        self.file.seek(0);self.file.truncate();self.file.write(str(os.getpid()));self.file.flush()
        return self
    def __exit__(self,*args):
        if self.file:
            fcntl.flock(self.file,fcntl.LOCK_UN);self.file.close();self.file=None

def sdk_config(path=None):
    cfg=json.loads((Path(path) if path else ROOT/'config/sdk.json').read_text())
    if not cfg.get('verified_sdk_headers') or cfg.get('codec') not in ('h264','hevc') or type(cfg.get('timestamp_hz')) is not int or cfg['timestamp_hz']<=0:
        raise RuntimeError('SDK 配置未通过核验，请检查 config/sdk.json。')
    for key in ('camera_root','media_root','models'):
        value=Path(cfg.get(key,''))
        cfg[key]=str(value if value.is_absolute() else ROOT/value)
        if not Path(cfg.get(key,'')).is_dir():raise RuntimeError('SDK 或模型目录不存在：'+key)
    for binary in ('x5_recorder','x5_sdk_bridge'):
        if not (ROOT/'build'/binary).is_file():raise RuntimeError('缺少录制程序，请先运行构建脚本。')
    return cfg

def atomic_json(path,data):
    path=Path(path);temp=path.with_name(path.name+'.tmp')
    temp.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n');temp.replace(path)

def recover_x5_usb():
    """Reset exactly one Android-mode X5, while holding CameraLock."""
    devices=[]
    for path in Path('/sys/bus/usb/devices').iterdir():
        try:
            if (path/'idVendor').read_text().strip()=='2e1a' and (path/'idProduct').read_text().strip()=='0002':
                bus=int((path/'busnum').read_text());device=int((path/'devnum').read_text())
                devices.append(Path(f'/dev/bus/usb/{bus:03}/{device:03}'))
        except (OSError,ValueError):continue
    if len(devices)!=1:raise RuntimeError('恢复 USB 通信需要恰好一台处于 Android 模式的 X5。')
    descriptor=os.open(devices[0],os.O_WRONLY)
    try:fcntl.ioctl(descriptor,0x5514,0)  # Linux USBDEVFS_RESET, scoped to this X5.
    finally:os.close(descriptor)
    time.sleep(0.7)
