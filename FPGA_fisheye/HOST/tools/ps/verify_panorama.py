#!/usr/bin/env python3
"""Check production dual PS LUTs and panorama negotiation without board access."""
from pathlib import Path
import json
import subprocess
import argparse
import re
import zipfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--ps-source', type=Path, default=(root.parent / 'PS/7ev_fish_app/src'))
parser.add_argument('--xsa', type=Path, default=(root.parent / 'PL/hardware_export/pl_project_snapshot.xsa'))
args=parser.parse_args()
source=args.ps_source
with zipfile.ZipFile(args.xsa) as archive:
    modules=[m for n in archive.namelist() if n.endswith('.hwh') for m in ET.fromstring(archive.read(n)).iter('MODULE')]
    module=next(m for m in modules if m.get('INSTANCE')=='corrector')
    frequency=int(next(p.get('CLKFREQUENCY') for p in module.iter('PORT') if p.get('NAME')=='aclk'))
configured=int(re.search(r'#define FPTZ_CORRECTOR_CLOCK_HZ (\d+)u', (source/'fptz_server.h').read_text()).group(1))
assert frequency==configured, (frequency,configured)
output = root / 'build/verification/ps_panorama'
output.mkdir(parents=True, exist_ok=True)
flags = ['-ffp-contract=off', '-fno-fast-math', '-I' + str(source)]
functions = ['fptz_lut_project_one_lens', 'fptz_lut_project_one',
             'fptz_lut_gen_begin_dual', 'fptz_lut_gen_begin', 'fptz_lut_weight_q8',
             'fptz_lut_gen_pixels', 'fptz_lut_gen_rows']
subprocess.run(['gcc', '-O0', *flags,
                *['-D' + name + '=' + name + '_reference' for name in functions],
                '-c', str(root / 'tests/ps/lut_reference.c'),
                '-o', str(output / 'reference.o')], check=True)
common = [str(source / (name + '.c')) for name in
          ['fptz_lut_generate', 'fptz_lut', 'fptz_scene_mask', 'fptz_scene_mask_data']]
results = {'corrector_clock_hz':frequency}
for name, extra in [('lut_latency_check', [str(output / 'reference.o')]),
                    ('latency_wire_check', [str(source / 'fptz_protocol.c')]),
                    ('panorama_server_check', [str(source / 'fptz_server.c'),
                                               str(source / 'fptz_protocol.c')])]:
    binary = output / name
    subprocess.run(['gcc', '-O3', *flags, str(root / 'tests/ps' / (name + '.c')),
                    *common, *extra, '-lm', '-o', str(binary)], check=True)
    results[name] = json.loads(subprocess.check_output([str(binary)], text=True))
binary = output / 'stage_wait_check'
subprocess.run(['gcc', '-O3', *flags, str(root / 'tests/ps/stage_wait_check.c'),
                str(source / 'fptz_corrector.c'), str(source / 'fptz_protocol.c'),
                '-o', str(binary)], check=True)
results['stage_wait_check'] = json.loads(subprocess.check_output([str(binary)], text=True))
results['scope'] = 'Native CPU protocol/geometry checks; no hardware timing claim'
(output / 'report.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
