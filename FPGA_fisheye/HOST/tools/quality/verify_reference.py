#!/usr/bin/env python3
"""Check bundled quality reference against current PS without modifying either."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--ps-source', type=Path, default=(root.parent / 'PS/7ev_fish_app/src'))
args = p.parse_args()
reference = root / 'src/quality/ps_reference'
manifest = json.loads((reference / 'manifest.json').read_text())
for name, expected in manifest['files'].items():
    for folder in (reference, args.ps_source):
        actual = hashlib.sha256((folder / name).read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f'Reference mismatch: {folder / name}')
profile = hashlib.sha256(json.dumps(manifest['files'], sort_keys=True).encode()).hexdigest()
assert profile == manifest['sha256']
assert f'"{profile}"' in (reference / 'reference_id.h').read_text()
source = r'''
#include "fptz_lut_generate.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  uint8_t *lut = malloc(FPTZ_LUT_BYTES);
  if (!lut) return 1;
  FptzLutGen gen;
  if (fptz_lut_gen_begin_panorama(&gen,lut,fptz_scene_mask_front_bits,fptz_scene_mask_rear_bits)) return 2;
  int status;
  do { status=fptz_lut_gen_rows(&gen,8); } while (!status);
  if (status!=1) return 3;
  size_t n=fwrite(lut,1,FPTZ_LUT_BYTES,stdout);
  free(lut);
  return n==FPTZ_LUT_BYTES?0:4;
}
'''
with tempfile.TemporaryDirectory(prefix='quality-reference-') as tmp:
    tmp = Path(tmp)
    (tmp / 'check.c').write_text(source)
    digests = []
    for i, folder in enumerate((reference, args.ps_source)):
        binary = tmp / f'check{i}'
        subprocess.run(['gcc', '-O2', '-ffp-contract=off', '-fno-fast-math', '-I'+str(folder),
                        str(tmp/'check.c'), *[str(folder/n) for n in ('fptz_lut_generate.c','fptz_scene_mask.c','fptz_scene_mask_data.c')],
                        '-lm', '-o', str(binary)], check=True)
        run = subprocess.Popen([str(binary)], stdout=subprocess.PIPE)
        digest, size = hashlib.sha256(), 0
        while chunk := run.stdout.read(1024*1024):
            digest.update(chunk)
            size += len(chunk)
        assert run.wait() == 0
        digests.append((digest.hexdigest(), size))
    assert digests[0] == digests[1]
    print(json.dumps({'reference_sha256':profile,'lut_sha256':digests[0][0],
                      'lut_bytes':digests[0][1],'pixels':2656*1328,
                      'result':'all Q6 coordinates, Q8 weights and source masks match production PS',
                      'hardware_verified':False}, indent=2))
