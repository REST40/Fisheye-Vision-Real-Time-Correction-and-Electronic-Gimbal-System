#!/usr/bin/env python3
"""Differential strict-marker checks and bounded block-scan benchmark; native CPU only."""
import ctypes
import json
from pathlib import Path
import random
import struct
import subprocess
import time

root=Path(__file__).resolve().parents[2]
source=root.parent/'PS/7ev_fish_app/src'
out=root/'build/verification/ps_jpeg_scan_optimization'
out.mkdir(parents=True,exist_ok=True)
reference=root/'tests/ps/jpeg_scan_reference.c'
flags=['-std=c11','-O3','-Wall','-Wextra','-I'+str(source)]
subprocess.run(['gcc',*flags,'-fPIC','-shared',str(source/'fptz_protocol.c'),str(reference),
                '-o',str(out/'jpeg_scan_check.so')],check=True)
lib=ctypes.CDLL(str(out/'jpeg_scan_check.so'))
class Pair(ctypes.Structure):
    _fields_=[('offset',ctypes.c_uint32*2),('length',ctypes.c_uint32*2)]
validators=[lib.fptz_jpeg_is_source_baseline_reference,lib.fptz_jpeg_is_source_baseline]
pair_parsers=[lib.fptz_jpeg_pair_parse_reference,lib.fptz_jpeg_pair_parse]
for f in validators:
    f.argtypes=[ctypes.c_void_p,ctypes.c_size_t];f.restype=ctypes.c_int
for f in pair_parsers:
    f.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(Pair)];f.restype=ctypes.c_int
cases=0

def check(data,expected=None,offsets=(0,1,3,7,15)):
    global cases
    for offset in offsets:
        buf=ctypes.create_string_buffer(b'!'*offset+data)
        p=ctypes.addressof(buf)+offset
        results=[f(p,len(data)) for f in validators]
        assert results[0]==results[1],(len(data),offset,results)
        if expected is not None: assert results[1]==expected,(len(data),offset,results,expected)
        cases+=1

def check_pair(data,expected):
    global cases
    for offset in [0,1,7]:
        buf=ctypes.create_string_buffer(b'!'*offset+data)
        results=[];outputs=[]
        for f in pair_parsers:
            pair=Pair((11,22),(33,44))
            results.append(f(ctypes.addressof(buf)+offset,len(data),ctypes.byref(pair)))
            outputs.append(bytes(pair))
        assert results[0]==results[1]==expected,(results,expected)
        assert outputs[0]==outputs[1]
        if expected: assert outputs[1]==bytes(Pair((11,22),(33,44)))
        cases+=1

def segment(marker,payload):
    return b'\xff'+bytes([marker])+struct.pack('>H',len(payload)+2)+payload
sof=segment(0xc0,struct.pack('>BHHB',8,1328,1328,3)+bytes([1,0x22,0,2,0x11,0,3,0x11,0]))
header=b'\xff\xd8'+segment(0xdb,b'\x00')+segment(0xc4,b'\x00')+sof+segment(0xda,bytes([3,1,0,2,0,3,0,0,63,0]))
eoi=b'\xff\xd9'
check(header+eoi,1)
for n in range(128):
    for entropy in [b'\x35'*n,b'\xff\x00'*n,b'\xff'*n+b'\x00',b'\xfe'*n]:
        check(header+entropy+eoi,1)
# Every marker at every position around multiple 8-byte boundaries.
for pos in range(32):
    for marker in range(256):
        data=header+b'\x35'*pos+b'\xff'+bytes([marker])+b'\x35'*19+eoi
        check(data,1 if marker == 0 else 0,range(8))
for marker in range(0xd0,0xd8):
    check(header+b'\xff'+bytes([marker])+eoi,0)
check(header+eoi+b'\x35'+eoi,0)
check(header+b'\xff\xda\x00\x0c'+eoi,0)
check(header.replace(b'\xff\xc0',b'\xff\xc2',1)+eoi,0)
check(b'\xff\xd8'+segment(0xdd,b'\x00\x01')+header[2:]+eoi,0)
valid=header+b'\x35'*256+eoi
for n in range(len(valid)):
    check(valid[:n],0)
seed=random.Random(0x46465343)
for _ in range(1500):
    size=seed.randrange(4096)
    raw=seed.getrandbits(size*8).to_bytes(size,'little')
    check(header+raw+eoi,offsets=(0,1,7))
for size in [65535,65536,65537,8*1024*1024]:
    check(header+b'\x35'*(size-len(header)-2)+eoi,1,offsets=(0,1,7,15))
files=[];real=[]
for pattern in ['fault_20261014_110546_298_frame_103/*.jpg','jtag_frame_544_20261014_capture2/*.jpg']:
    for path in sorted((root/'output/diagnostics').glob(pattern)):
        data=path.read_bytes();check(data,1);real.append(data);files.append(str(path.relative_to(root)))
        for _ in range(100):
            changed=bytearray(data);i=seed.randrange(len(changed));changed[i]^=1<<seed.randrange(8)
            check(changed,offsets=(0,7))
        for n in list(range(min(96,len(data))))+list(range(len(data)-32,len(data))):
            check(data[:n],offsets=(0,1))
# Exact padded pair container: preserve strict padding and lengths.
front=real[0] if real else valid;rear=real[1] if len(real)>1 else valid
block=lambda n:(n+127)&~63
pair=struct.pack('>II',len(front),len(rear))+bytes(56)
pair+=front+bytes(block(len(front))-len(front))+rear+bytes(block(len(rear))-len(rear))
check_pair(pair,0)
for index in [8,63,64+len(front),64+block(len(front))+len(rear),len(pair)-1]:
    changed=bytearray(pair);changed[index]=1;check_pair(changed,-1)
check_pair(pair[:-1],-1)
bench=[]
for data in real[:2] or [header+b'\x35'*200000+eoi]:
    buf=ctypes.create_string_buffer(data);times=[]
    for f in validators:
        samples=[]
        for _ in range(3):
            start=time.perf_counter()
            for _ in range(250):f(buf,len(data))
            samples.append((time.perf_counter()-start)/250)
        times.append(min(samples))
    bench.append({'bytes':len(data),'bytewise_ms':times[0]*1000,'block8_ms':times[1]*1000,
                  'speedup':times[0]/times[1]})
subprocess.run(['gcc',*flags,str(root/'tests/ps/jpeg_scan_bounds_check.c'),str(reference),
                '-o',str(out/'jpeg_scan_bounds_check')],check=True)
bounds=json.loads(subprocess.check_output([str(out/'jpeg_scan_bounds_check')],text=True))
report={'cases':cases,'rules_match_preserved_bytewise_validator':True,'bounds':bounds,
        'real_jpeg_samples':files,'native_cpu_benchmark':bench,
        'scope':'Structural JPEG validation and native CPU timing only; not decoding or board FPS'}
(out/'scan_report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
