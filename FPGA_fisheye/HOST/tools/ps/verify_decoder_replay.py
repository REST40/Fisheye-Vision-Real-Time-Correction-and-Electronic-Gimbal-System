#!/usr/bin/env python3
"""Replay captured DDR JPEGs with Verilator; compare pixels under AXI stalls.

Requires Verilator and a C++ compiler. Run in the repository or a container
mounting it. --fault-rows checks that the unpatched wrapper reproduces a fault
for the final JPEG argument; no board is needed and no firmware is loaded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jpegs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fault-rows", type=int)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    verilator = shutil.which("verilator")
    if not verilator:
        parser.error("Verilator is required")
    core = sorted((root / "PL/mjpeg/third_party/core_jpeg/src_v").glob("*.v"))
    # Production project uses the adapted MCU coordinate generator.
    shared = [p for p in core if p.name not in ("jpeg_input.v", "jpeg_mcu_id.v")]
    shared.append(root / "PL/mjpeg/adapted/jpeg_mcu_id.v")
    binaries = {}
    for name, top, sources, harness in [
        ("core", "jpeg_core", shared + [root / "PL/mjpeg/third_party/core_jpeg/src_v/jpeg_input.v"], "jpeg_core_replay.cpp"),
        ("original", "jpeg_decoder", shared + [root / "PL/mjpeg/third_party/core_jpeg/src_v/jpeg_input.v"], "jpeg_axi_replay.cpp"),
        ("fixed", "jpeg_decoder", shared + [root / "PL/mjpeg/adapted/jpeg_input.v"], "jpeg_axi_replay.cpp"),
    ]:
        build = args.output / name
        cmd = [verilator, "--cc", "--exe", "--build", "-j", "2", "-Wno-fatal", "--top-module", top,
               "--Mdir", str(build), "-CFLAGS", "-O3"]
        if top == "jpeg_core":
            cmd.append("-GSUPPORT_WRITABLE_DHT=1")
        else:
            cmd.append("--public-flat-rw")
            sources += [root / "PL/mjpeg/adapted/jpeg_decoder.v", root / "PL/mjpeg/rtl/mjpeg_rgb_burst.v"]
        cmd += [str(p) for p in sources] + [str(root / "HOST/tests/ps" / harness)]
        with (args.output / f"{name}_build.log").open("w") as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
        binaries[name] = build / f"V{top}"

    report = {"inputs": [], "cases": [], "hardware_test": False,
              "parser_sha256": hashlib.sha256((root / "PL/mjpeg/adapted/jpeg_input.v").read_bytes()).hexdigest()}

    def run(name, jpeg, extra, expected=True):
        result = subprocess.run([str(binaries[name]), str(jpeg), *map(str, extra)], capture_output=True, text=True)
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stdout + result.stderr)
        row = json.loads(result.stdout)
        row.update(variant=name, jpeg=jpeg.name)
        report["cases"].append(row)
        print(json.dumps(row), flush=True)
        assert row["complete"] == expected, row
        return row

    for path in args.jpegs:
        jpeg = path.resolve()
        report["inputs"].append({"path": str(jpeg), "sha256": hashlib.sha256(jpeg.read_bytes()).hexdigest()})
        baseline = run("core", jpeg, [])
        # The AXI prefetch leaves an invalid cycle between 2 KB banks.
        fault_case = args.fault_rows is not None and path == args.jpegs[-1]
        original = run("original", jpeg, [1, 0], expected=not fault_case)
        if fault_case:
            assert original["rows"] == args.fault_rows, original
        for seed, mode in [(1, 0), (1, 1), (77, 1), (99, 2)]:
            fixed = run("fixed", jpeg, [seed, mode])
            assert fixed["rgb_fnv1a64"] == baseline["rgb_fnv1a64"], fixed
            assert fixed["rows"] == 1328 and fixed["error"] == 0
    report["passed"] = True
    (args.output / "replay_report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
