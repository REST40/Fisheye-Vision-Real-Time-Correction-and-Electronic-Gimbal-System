#!/usr/bin/env python3
"""Generate synthetic, non-camera fixtures for the existing offline host tests."""
import argparse
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=root / 'data/demo')
args = parser.parse_args()
if not shutil.which('ffmpeg'):
    parser.error('ffmpeg is required')
names = ['front.png', 'rear.png', 'front_10s.mp4', 'rear_10s.mp4', 'frames.csv']
if any((args.output / name).exists() for name in names):
    parser.error('Output contains fixture files; use a new --output directory instead of overwriting')
args.output.mkdir(parents=True, exist_ok=True)
for lens, filt in [('front', 'null'), ('rear', 'hflip')]:
    common = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-n', '-f', 'lavfi', '-i',
              'testsrc2=size=1328x1328:rate=30:duration=10', '-vf', filt]
    subprocess.run(common + ['-frames:v', '1', '-threads', '1', str(args.output / (lens+'.png'))], check=True)
    subprocess.run(common + ['-frames:v', '300', '-an', '-c:v', 'mjpeg', '-q:v', '5',
                            '-pix_fmt', 'yuvj420p', '-threads', '2', str(args.output / (lens+'_10s.mp4'))], check=True)
(args.output / 'frames.csv').write_text('pts_us\n' + ''.join(f'{round(i*1000000/30)}\n' for i in range(300)))
print(f'Synthetic demo fixtures created at {args.output}')
