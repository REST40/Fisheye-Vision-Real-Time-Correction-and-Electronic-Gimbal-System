#!/usr/bin/env python3
"""Compare actual PS CRC with zlib and the preserved bytewise baseline on this CPU."""
import ctypes
import json
from pathlib import Path
import random
import subprocess
import time
import zlib

root = Path(__file__).resolve().parents[2]
source = root.parent / 'PS/7ev_fish_app/src'
out = root / 'build/verification/ps_crc_optimization'
out.mkdir(parents=True, exist_ok=True)
fixture = (root / 'tests/fixtures/ps_protocol/fptz_protocol.c').read_text()
a = fixture.index('/* Reflected IEEE CRC-32:')
b = fixture.index('static int known_type(')
baseline = out / 'crc_baseline.c'
baseline.write_text('#include <stdint.h>\n#include <stddef.h>\n' +
                    fixture[a:b].replace('fptz_crc32(', 'fptz_crc32_baseline('))
subprocess.run(['gcc', '-std=c11', '-O3', '-Wall', '-Wextra', '-fPIC', '-shared',
                '-I' + str(source), str(source / 'fptz_protocol.c'), str(baseline),
                '-o', str(out / 'crc_check.so')], check=True)
lib = ctypes.CDLL(str(out / 'crc_check.so'))
functions = [lib.fptz_crc32_baseline, lib.fptz_crc32]
for f in functions:
    f.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    f.restype = ctypes.c_uint32
cases = 0

def check(data, offsets):
    global cases
    expected = zlib.crc32(data)
    for offset in offsets:
        storage = ctypes.create_string_buffer(b'!' * offset + data)
        pointer = ctypes.addressof(storage) + offset
        assert all(f(pointer, len(data)) == expected for f in functions), (len(data), offset)
        cases += 1

assert lib.fptz_crc32(None, 0) == 0
check(b'123456789', range(64))
seed = random.Random(0x435243)
block = seed.getrandbits(8192 * 8).to_bytes(8192, 'little')
for length in list(range(257)) + [511,512,513,1023,1024,1025,1399,1400,1401,65535,65536,65537]:
    check((block * ((length + 8191)//8192))[:length], range(16))
for length in [185584,203044,388864,400000,8*1024*1024,16*1024*1024+192]:
    check((block * ((length + 8191)//8192))[:length], [0,1,3,7,63])
files = []
for pattern in ['fault_20261014_110546_298_frame_103/*.jpg',
                'fault_20261014_110546_298_frame_103/input_pair.bin',
                'jtag_frame_544_20261014_capture2/*.jpg']:
    for path in sorted((root/'output/diagnostics').glob(pattern)):
        check(path.read_bytes(), [0,1,7])
        files.append(str(path.relative_to(root)))
bench = []
for size in [230000,400000]:
    data = ctypes.create_string_buffer((block * ((size+8191)//8192))[:size])
    timings = []
    for f in functions:
        samples=[]
        for _ in range(3):
            started=time.perf_counter()
            for repeat in range(160):
                value=f(data,size)
            samples.append((time.perf_counter()-started)/160)
        timings.append(min(samples))
    bench.append({'bytes':size,'baseline_ms':1000*timings[0],
                  'slicing8_ms':1000*timings[1],'speedup':timings[0]/timings[1]})
subprocess.run(['gcc','-std=c11','-O3','-Wall','-Wextra','-I'+str(source),
                str(root/'tests/ps/crc_bounds_check.c'),str(source/'fptz_protocol.c'),
                str(baseline),'-o',str(out/'crc_bounds_check')],check=True)
bounds=json.loads(subprocess.check_output([str(out/'crc_bounds_check')],text=True))
report={'cases':cases,'bounds_and_rejection':bounds,'matches':'zlib IEEE CRC32 and preserved bytewise implementation',
        'fault_samples':files,'native_cpu_benchmark':bench,
        'scope':'Native CPU only; no Cortex-A53 timing or full-pipeline FPS claim'}
(out/'crc_report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
