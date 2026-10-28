#!/usr/bin/env python3
"""Supervise isolated SDK/recorder processes; newline JSON control for the GUI."""
import argparse
import fcntl
import datetime
import json
import os
import pathlib
import select
import shutil
import signal
import subprocess
import sys
import time
from collections import deque
from zoneinfo import ZoneInfo
from common import ROOT, CameraLock, sdk_config, atomic_json, recover_x5_usb, parent_death_signal, x5_usb_permission_denied

def emit(event,**data):
    print(json.dumps(dict(event=event,**data),ensure_ascii=False),flush=True)

def choose_encoder(requested,width,height):
    if requested=='libx264':return 'libx264','CPU 编码'
    cmd=['ffmpeg','-nostdin','-v','error','-f','lavfi','-i','testsrc2=size=2656x1328:rate=30',
         '-f','lavfi','-i',f'testsrc2=size={width}x{height}:rate=30,format=rgba']
    for index in (0,1):cmd+=['-map',f'{index}:v','-frames:v','6','-c:v','h264_nvenc','-preset','p4','-tune','ll','-bf','0','-pix_fmt','yuv420p' if index==0 else 'rgba','-rgb_mode','yuv420','-f','null',os.devnull]
    try:
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=15)
        if result.returncode==0:return 'h264_nvenc','NVIDIA 显卡编码'
        detail=result.stderr[-1500:]
    except (OSError,subprocess.TimeoutExpired) as e:detail=str(e)
    if requested=='h264_nvenc':raise RuntimeError('显卡编码检查未通过；请选择自动或 CPU 编码。 '+detail)
    return 'libx264','显卡编码不可用，已切换 CPU 编码'

def session(args):
    cfg=sdk_config(args.sdk_config)
    if args.recover_usb:recover_x5_usb()
    base=args.output_root.expanduser().resolve();base.mkdir(parents=True,exist_ok=True)
    if shutil.disk_usage(base).free<1024**3:raise RuntimeError('保存位置剩余空间不足 1 GB，请选择其他位置。')
    encoder,note=choose_encoder(args.encoder,args.width,args.height)
    stamp=datetime.datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y%m%d_%H%M%S_%f')
    directory=base/('X5_'+stamp);directory.mkdir(mode=0o700)
    native=directory/'stream';preview=directory/'preview.bin';status_file=directory/'live_status.json';metadata=directory/'parameters';metadata.mkdir()
    exposure=dict(mode=args.exposure_mode,iso=args.iso,shutter_seconds=1/args.shutter,shutter_denominator=args.shutter,ev_stops=args.ev,ev_bias_raw=round(args.ev*20))
    settings=dict(width=args.width,height=args.height,encoder=encoder,requested_encoder=args.encoder,bitrate_mbps=args.bitrate,record_seconds=args.seconds,exposure=exposure)
    atomic_json(directory/'session_config.json',dict(sdk=cfg,settings=settings))
    atomic_json(metadata/'capture_settings.json',dict(schema_version=1,session_created_at=datetime.datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
        settings=settings,timestamp_hz=cfg['timestamp_hz'],nominal_fps=30,sensors='front_and_rear',usb_mode='Android',
        preview_requested_resolution=[1920,960],preview_bitrate_requested_bps=12*1024*1024,actual_resolution_source='recording_parameters.json observed',
        camera_sdk_version='2.1.1',media_sdk_version='3.1.1',fisheye_data='ISP-processed encoded preview, not Bayer RAW',
        correction='official Insta360 Media SDK, equirectangular 360',
        correction_flow_state=cfg.get('flow_state',True),exposure_locked_in_workbench_while_connected=True))
    emit('session',path=str(directory),preview=str(preview),encoder=encoder,encoder_note=note,settings=settings)
    read_fd=write_fd=control_r=control_w=-1;capture=recorder=None;stopping=False;problem='';forced=[]
    status={};last_pairs=0;last_progress=time.monotonic();last_emit=0;last_disk=0;last_status=None;record_start=None;auto_stopped=False
    pending=b'';actions=deque();requested_action=None;metadata_sequence=0;metadata_seen=None;dataset_destination=None
    def stop_signal(*_):
        nonlocal stopping
        stopping=True
    old={s:signal.signal(s,stop_signal) for s in (signal.SIGINT,signal.SIGTERM)}
    def control(command):
        if control_w>=0:os.write(control_w,(command+'\n').encode())
    try:
        read_fd,write_fd=os.pipe();control_r,control_w=os.pipe()
        try:fcntl.fcntl(write_fd,fcntl.F_SETPIPE_SZ,1024*1024)
        except OSError:pass
        env=os.environ.copy();env.pop('LD_LIBRARY_PATH',None)
        sdk_env=env.copy();sdk_env['LD_LIBRARY_PATH']=':'.join(str(pathlib.Path(cfg[k])/'lib') for k in ('camera_root','media_root'))
        command=[str(ROOT/'build/x5_recorder'),'--output',str(native),'--timestamp-hz',str(cfg['timestamp_hz']),
                 '--encoder',encoder,'--bitrate-mbps',str(args.bitrate),'--interactive','--preview-latest','--control-fd',str(control_r),
                 '--status-file',str(status_file),'--preview-file',str(preview),'--metadata-dir',str(metadata)]
        with (directory/'recorder.log').open('wb') as rlog,(directory/'sdk.log').open('wb') as slog:
            recorder=subprocess.Popen(command,stdin=read_fd,pass_fds=(control_r,),stdout=rlog,stderr=rlog,env=env,start_new_session=True)
            os.close(read_fd);read_fd=-1;os.close(control_r);control_r=-1
            capture=subprocess.Popen([str(ROOT/'build/x5_sdk_bridge'),'--ipc-fd',str(write_fd),'--codec',cfg['codec'],'--models',cfg['models'],
                                      '--video-delay-ms',str(cfg.get('video_delay_ms',-1)),
                                      '--flow-state','1' if cfg.get('flow_state',True) else '0',
                                      '--output-width',str(args.width),'--output-height',str(args.height),'--metadata-dir',str(metadata),
                                      '--exposure-mode',args.exposure_mode,'--iso',str(args.iso),'--shutter-seconds',str(1/args.shutter),'--ev-raw',str(round(args.ev*20)),
                                      '--exposure-function',str(cfg.get('exposure_function_mode',7)),'--restore-exposure-on-exit','1' if args.restore_exposure_on_exit else '0'],pass_fds=(write_fd,args.camera_lock_fd),stdout=slog,stderr=slog,env=sdk_env,start_new_session=True,
                                      preexec_fn=lambda:parent_death_signal(signal.SIGINT))
            os.close(write_fd);write_fd=-1
            while not stopping:
                now=time.monotonic()
                if capture.poll() is not None or recorder.poll() is not None:
                    problem='相机连接或录制进程已停止，请检查数据线并重新连接。';break
                ready,_,_=select.select([sys.stdin.fileno()],[],[],0.08)
                if ready:
                    chunk=os.read(sys.stdin.fileno(),8192)
                    if not chunk:stopping=True;break
                    pending+=chunk
                    if len(pending)>16384:raise RuntimeError('控制消息过长。')
                    while b'\n' in pending:
                        line,pending=pending.split(b'\n',1)
                        data=json.loads(line);cmd=data.get('cmd')
                        if cmd=='shutdown':stopping=True;break
                        if cmd not in ('start','stop','snapshot','lens_start'):raise RuntimeError('未知控制请求。')
                        if cmd=='lens_start':
                            if status.get('recording') or status.get('lens_busy'):
                                emit('operation_error',cmd=cmd,message='请等待当前采集完成，再开始前后采集。');continue
                            if cmd in actions or requested_action and requested_action['cmd']==cmd:continue
                            target=pathlib.Path(data.get('output_root',str(base/'lens_data'))).expanduser().resolve()
                            if any(c in str(target) for c in ('\n','\r','\t')):
                                emit('operation_error',cmd=cmd,message='保存路径不能包含换行符或制表符。');continue
                            try:
                                target.mkdir(parents=True,exist_ok=True)
                                if shutil.disk_usage(target).free<1024**3:raise RuntimeError('保存位置剩余空间不足1 GB。')
                            except (OSError,RuntimeError) as e:
                                emit('operation_error',cmd=cmd,message='数据采集目录不可用：'+str(e));continue
                            dataset_destination=str(target/('X5_data_'+datetime.datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y%m%d_%H%M%S_%f')))
                        if cmd=='stop':
                            actions=deque(x for x in actions if x!='start')
                            if requested_action and requested_action['cmd']=='start':requested_action=None
                            control(cmd)
                        elif cmd=='start' and (status.get('recording') or status.get('lens_busy')):continue
                        elif cmd not in actions and not (requested_action and requested_action['cmd']==cmd):
                            actions.append(cmd)
                            if cmd=='start':record_start=None;auto_stopped=False
                # Refresh settings before every clip/snapshot, in the SDK process.
                # The UI thread and frame callbacks never perform USB RPCs.
                if not stopping and requested_action is None and actions:
                    metadata_sequence+=1;requested_action=dict(cmd=actions.popleft(),id=metadata_sequence,deadline=now+5.5)
                    request_file=metadata/'metadata_request.txt';temp=request_file.with_suffix('.tmp');temp.write_text(str(metadata_sequence)+'\n');temp.replace(request_file)
                try:camera_metadata=json.loads((metadata/'camera_metadata.json').read_text())
                except (OSError,json.JSONDecodeError):camera_metadata=None
                if camera_metadata and camera_metadata.get('request_id')!=metadata_seen:
                    metadata_seen=camera_metadata.get('request_id');emit('camera_metadata',data=camera_metadata)
                if requested_action:
                    matching=camera_metadata and camera_metadata.get('request_id')==requested_action['id']
                    if matching and camera_metadata.get('valid'):
                        # Bind an immutable readback to this exact command; a
                        # simultaneous snapshot must not replace a clip's data.
                        frozen=metadata/'readbacks'/f"{requested_action['id']:06d}";frozen.mkdir(parents=True)
                        atomic_json(frozen/'camera_metadata.json',camera_metadata)
                        for name in ('capture_settings.json','calibration.json'):shutil.copy2(metadata/name,frozen/name)
                        command=requested_action['cmd']+' '+str(frozen)
                        if requested_action['cmd']=='lens_start':command+='\t'+dataset_destination
                        control(command);requested_action=None
                    elif matching or now>requested_action['deadline']:
                        emit('operation_error',cmd=requested_action['cmd'],message='未能确认本次相机参数，操作已取消；请断开后重连。')
                        requested_action=None
                if status_file.exists():
                    try:live=json.loads(status_file.read_text())
                    except (OSError,json.JSONDecodeError):live={}
                    if live:
                        status=live
                        if live.get('phase')=='error':problem=live.get('error') or '录制进程异常结束。';break
                        if live.get('total_pairs',0)!=last_pairs:last_pairs=live['total_pairs'];last_progress=now
                        if now-last_emit>=0.2:
                            live['disk_free_gb']=round(shutil.disk_usage(base).free/1024**3,1)
                            emit('status',**live);last_emit=now;last_status=live
                        if live.get('recording'):
                            if record_start is None:record_start=now
                            if args.seconds and not auto_stopped and now-record_start>=args.seconds:control('stop');auto_stopped=True
                        else:record_start=None
                if now-last_progress>(8 if last_pairs else 15):problem='长时间没有收到相机画面，请检查 X5 电源和 USB 连接。';break
                if now-last_disk>1:
                    last_disk=now
                    if shutil.disk_usage(base).free<512*1024**2:problem='磁盘剩余空间不足，已停止采集以完成文件保存。';break
            emit('stopping',message=problem or '正在完成文件保存…')
            if capture.poll() is None:capture.send_signal(signal.SIGINT)
            try:capture.wait(timeout=15)
            except subprocess.TimeoutExpired:capture.kill();capture.wait();forced.append('capture')
            try:recorder.wait(timeout=20)
            except subprocess.TimeoutExpired:
                recorder.terminate()
                try:recorder.wait(timeout=3)
                except subprocess.TimeoutExpired:recorder.kill();recorder.wait()
                forced.append('recorder')
            if status_file.exists():
                try:last_status=json.loads(status_file.read_text())
                except (OSError,json.JSONDecodeError):pass
            problem_code=''
            if capture.returncode:
                log=(directory/'sdk.log').read_text(errors='replace')[-64000:]
                if 'IPC queue overflow:' in log:
                    problem='双路处理未能跟上相机帧率，已停止并保留现有文件。可选择轻量输出尺寸后重新连接。';problem_code='processing_overload'
                elif 'no camera:' in log:
                    protocol='parse packet error' in log or 'synchronize' in log
                    problem='USB 通信同步失败，请点击连接相机重试，软件将恢复 X5 的 USB 通信。' if protocol else '未找到可用的 X5，请检查电源、数据线和 Android USB 模式。'
                    problem_code='usb_sync' if protocol else 'no_camera'
                    if x5_usb_permission_denied():
                        problem='已检测到 X5，但没有 USB 读写权限。请在本机完成X5 USB读写权限配置，重新插拔相机后连接。'
                        problem_code='usb_permission'
                elif 'exposure' in log.lower():
                    problem='X5 未接受本次曝光设置或参数回读不一致，已停止连接。请选择“保留相机设置”后重试，并查看本次日志。';problem_code='exposure_settings'
            if not problem and (capture.returncode or recorder.returncode or forced):problem='本次采集异常结束，已保留文件和日志。'
            status=dict(capture_returncode=capture.returncode,recorder_returncode=recorder.returncode,forced_stop=forced,problem=problem,problem_code=problem_code,settings=settings)
            okay=not problem and not forced and capture.returncode==recorder.returncode==0
            if last_status:emit('status',**last_status)
            atomic_json(directory/'process_status.json',status)
            emit('finished',okay=okay,path=str(directory),status=status)
            return 0 if okay else 1
    finally:
        for child in (capture,recorder):
            if child is not None and child.poll() is None:child.kill();child.wait()
        for fd in (read_fd,write_fd,control_r,control_w):
            if fd>=0:os.close(fd)
        for sig,handler in old.items():signal.signal(sig,handler)

def main():
    parent_death_signal(signal.SIGTERM)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-root',type=pathlib.Path,default=ROOT/'recordings')
    parser.add_argument('--width',type=int,default=2656);parser.add_argument('--height',type=int,default=1328)
    parser.add_argument('--encoder',choices=['auto','h264_nvenc','libx264'],default='auto')
    parser.add_argument('--bitrate',type=int,default=24);parser.add_argument('--seconds',type=int,default=0)
    parser.add_argument('--sdk-config',type=pathlib.Path)
    parser.add_argument('--recover-usb',action='store_true')
    parser.add_argument('--exposure-mode',choices=['keep','auto','manual'],default='keep')
    parser.add_argument('--iso',type=int,choices=[100,200,400,800,1600,3200],default=100)
    parser.add_argument('--shutter',type=int,choices=[30,50,60,100,120,200,240,500,1000,2000,4000,8000],default=60,help='Shutter denominator in seconds')
    parser.add_argument('--ev',type=float,default=0)
    parser.add_argument('--restore-exposure-on-exit',action='store_true',help='Restore original exposure after controlled validation')
    args=parser.parse_args()
    try:
        if (args.width,args.height) not in ((1920,960),(2656,1328)):raise RuntimeError('请选择已验证的 1920×960 或 2656×1328 全景输出尺寸。')
        if '\n' in str(args.output_root) or '\r' in str(args.output_root):raise RuntimeError('保存路径不能含换行符。')
        if not 8<=args.bitrate<=50 or not 0<=args.seconds<=86400:raise RuntimeError('录制参数超出范围。')
        if not -4<=args.ev<=4 or abs(args.ev*2-round(args.ev*2))>1e-8:raise RuntimeError('EV 需为 -4 至 +4，间隔 0.5。')
        with CameraLock() as ownership:
            args.camera_lock_fd=ownership.file.fileno()
            return session(args)
    except (Exception,KeyboardInterrupt) as e:
        try:emit('error',message=str(e))
        except BrokenPipeError:pass
        return 1

if __name__=='__main__':raise SystemExit(main())
