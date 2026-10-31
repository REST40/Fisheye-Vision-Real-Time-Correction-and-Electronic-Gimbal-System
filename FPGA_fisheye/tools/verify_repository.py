#!/usr/bin/env python3
"""Check source dependencies and project configuration without FPGA tools."""
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root=Path(__file__).resolve().parents[1]
script=(root/'PL/scripts/create_project.tcl').read_text(encoding='utf-8')
source_list=re.search(r'set source_paths\s*\{(.*?)\n\}',script,re.S)
if not source_list: raise SystemExit('Cannot locate the Vivado source list.')
sources=re.findall(r'\{([^{}]+)\}',source_list.group(1))
if not sources: raise SystemExit('The Vivado source list is empty.')
for name in sources:
    if not (root/'PL'/name).is_file(): raise SystemExit('Missing Vivado source: '+name)
ip=root/'PL/fisheye_ip/ip_repo/fisheye_cort_1_0/component.xml'
ns={'s':'http://www.spiritconsortium.org/XMLSchema/SPIRIT/1685-2009'}
refs=ET.parse(ip).findall('.//s:file/s:name',ns)
assert refs
for ref in refs: assert (ip.parent/ref.text).is_file(),ref.text
bd=json.loads((root/'PL/design/mjpeg_system.bd').read_text())['design']['components']
assert 'decoder' in bd and 'decoder1' in bd
assert bd['encoder']['parameters']['WIDTH']['value']=='2656'
assert bd['encoder']['parameters']['HEIGHT']['value']=='1328'
reference=root/'HOST/src/quality/ps_reference'
manifest=json.loads((reference/'manifest.json').read_text(encoding='utf-8'))
for name,expected in manifest['files'].items():
    for folder in (reference,root/'PS/7ev_fish_app/src'):
        content=(folder/name).read_bytes()
        if hashlib.sha256(content).hexdigest()!=expected:
            raise SystemExit('Quality reference mismatch: '+str((folder/name).relative_to(root)))
profile=hashlib.sha256(json.dumps(manifest['files'],sort_keys=True).encode()).hexdigest()
assert profile==manifest['sha256']
assert f'"{profile}"' in (reference/'reference_id.h').read_text(encoding='utf-8')
for name in ('HOST/CMakeLists.txt','HOST/config/calibration.json','HOST/config/sdk.json',
             'HOST/resources/theme.qss','HOST/resources/icon.svg',
             'PS/7ev_fish_app/src/main.c','PS/7ev_fish_app/src/lscript.ld',
             'PS/scripts/create_platform.tcl'):
    assert (root/name).is_file(),name
for name in ('core_jpeg','core_jpeg_decoder','jpeg_open'):
    assert (root/'PL/mjpeg/third_party'/name/'LICENSE').is_file(),name
print(f'PASS: {len(sources)} Vivado sources; {len(refs)} IP references; dual decoders; '
      f'2656x1328 encoder; {len(manifest["files"])} matching HOST/PS reference files')
