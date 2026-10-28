#!/usr/bin/env python3
"""Read-only verification of the competition source snapshot."""
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[1]
manifest=json.loads((root/'SOURCE_MANIFEST.json').read_text())
for name, expected in manifest['files'].items():
    p=root/name
    if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=expected['sha256']:
        raise SystemExit('Source snapshot mismatch: '+name)
xsas=[p.relative_to(root).as_posix() for p in root.rglob('*.xsa') if 'build' not in p.relative_to(root).parts]
assert xsas==['PL/hardware_export/pl_project_snapshot.xsa'],xsas
ip=root/'PL/fisheye_ip/ip_repo/fisheye_cort_1_0/component.xml'
ns={'s':'http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009'}
refs=ET.parse(ip).findall('.//s:file/s:name',ns)
assert refs
for ref in refs: assert (ip.parent/ref.text).is_file(),ref.text
bd=json.loads((root/'PL/design/mjpeg_system.bd').read_text())['design']['components']
assert 'decoder' in bd and 'decoder1' in bd
assert bd['encoder']['parameters']['WIDTH']['value']=='2656'
assert bd['encoder']['parameters']['HEIGHT']['value']=='1328'
print(f'PASS: {len(manifest["files"])} snapshot files; one PL XSA; dual decoders; 2656x1328 encoder; {len(refs)} IP references')
