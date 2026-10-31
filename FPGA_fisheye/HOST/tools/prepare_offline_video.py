#!/usr/bin/env python3
"""Split a 2656x1328 dual-fisheye recording for the HOST offline input."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def probe(path):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams',
        '-show_packets', '-show_entries',
        'stream=codec_name,width,height,duration:packet=pts_time', '-of', 'json', str(path)]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    original = probe(source)
    stream = original['streams'][0]
    if (stream['width'], stream['height']) != (2656, 1328):
        parser.error('Expected 2656x1328 dual-fisheye input; automatic resizing is disabled')
    if output.exists():
        parser.error('Output directory already exists; choose a new directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(output.name + '.processing')
    staging.mkdir()
    # Publish only a complete, validated dataset. Keep source files untouched.
    try:
        subprocess.run([
            'ffmpeg', '-hide_banner', '-loglevel', 'warning', '-n', '-i', str(source),
            '-filter_complex',
            '[0:v]split=2[a][b];[a]crop=1328:1328:0:0,setpts=PTS-STARTPTS[front];'
            '[b]crop=1328:1328:1328:0,setpts=PTS-STARTPTS[rear]',
            *sum((['-map', '[' + lens + ']', '-an', '-c:v', 'mjpeg', '-q:v', '3',
                   '-pix_fmt', 'yuvj420p', '-threads', '2', '-fps_mode', 'passthrough',
                   '-enc_time_base', '1:1000000', '-video_track_timescale', '1000000',
                   str(staging / (lens + '.mp4'))] for lens in ('front', 'rear')), [])
        ], check=True)
        pairs = [probe(staging / (lens + '.mp4')) for lens in ('front', 'rear')]
        times = [[round(float(p['pts_time']) * 1000000) for p in info['packets']]
                 for info in pairs]
        original_times = [round(float(p['pts_time']) * 1000000) for p in original['packets']]
        normalized = [t - original_times[0] for t in original_times]
        if not times[0] or times[0] != times[1] or times[0] != normalized:
            raise RuntimeError('Frame counts or timestamps changed during conversion')
        for lens in ('front', 'rear'):
            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-n',
                            '-i', str(staging / (lens + '.mp4')), '-frames:v', '1',
                            '-threads', '1', str(staging / (lens + '.png'))], check=True)
        (staging / 'frames.csv').write_text('pts_us\n' + ''.join(f'{t}\n' for t in times[0]))
        (staging / 'offline_import.json').write_text(json.dumps({
            'source': str(source), 'frames': len(times[0]), 'last_pts_us': times[0][-1],
            'lens_mapping': {'front': 'left', 'rear': 'right'},
            'timestamps_preserved': True, 'codec': 'mjpeg',
            'video_files': {'front': 'front.mp4', 'rear': 'rear.mp4'},
            'note': 'Full recording; duration is read from video timestamps, not filenames.'
        }, indent=2))
        for name in ('metadata.json', 'frames.csv'):
            if (source.parent / name).is_file():
                shutil.copy2(source.parent / name, staging / ('source_' + name))
        staging.rename(output)
    except BaseException:
        shutil.rmtree(staging)
        raise
    print(f'Prepared {len(times[0])} synchronized frames: {output}', flush=True)


if __name__ == '__main__':
    main()
