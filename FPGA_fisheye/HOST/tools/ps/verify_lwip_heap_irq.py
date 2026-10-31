#!/usr/bin/env python3
"""Reproduce a TX interrupt freeing heap memory during lwIP allocation.

Uses the actual BSP mem.c with a test-only interrupt injection at the allocation
commit point. Production lwIP sources are never modified. The unprotected case
must produce overlapping live allocations; the protected case must not.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bsp", type=Path, required=True, help="BSP processor directory containing include/ and libsrc/")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    bsp, output = args.bsp.resolve(), args.output.resolve()
    root = Path(__file__).resolve().parents[3]
    source_path = bsp / "libsrc/lwip211_v1_8/src/lwip-2.1.1/src/core/mem.c"
    source = source_path.read_text()
    original_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
    point = "      if ((!mem->used) &&\n          (mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size) {"
    assert source.count(point) == 1, "unexpected lwIP allocator version"
    source = source.replace("#include <string.h>", "#include <string.h>\nextern void irq_test_hook(void);")
    source = source.replace(point, point + "\n        irq_test_hook();")
    include = output / "include"
    (include / "arch").mkdir(parents=True, exist_ok=True)
    (include / "arch/cc.h").write_text('''#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr,"lwIP assertion: %s\\n",x); abort(); } while(0)
uint32_t sys_arch_protect(void);
void sys_arch_unprotect(uint32_t);
#define SYS_ARCH_DECL_PROTECT(lev) uint32_t lev
#define SYS_ARCH_PROTECT(lev) ((lev)=sys_arch_protect())
#define SYS_ARCH_UNPROTECT(lev) sys_arch_unprotect(lev)
''')
    (include / "lwipopts.h").write_text("#define NO_SYS 1\n#define SYS_LIGHTWEIGHT_PROT 1\n#define MEM_ALIGNMENT 64\n#define MEM_SIZE 131072\n#define LWIP_STATS 0\n")
    (output / "mem.c").write_text(source)
    cases = []
    for enabled in (0, 1):
        binary = output / f"check_{enabled}"
        subprocess.run(["gcc", "-O2", f"-DLWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT={enabled}",
                        "-I", str(include), "-I", str(bsp / "include"), str(output / "mem.c"),
                        str(root / "HOST/tests/ps/lwip_heap_irq_check.c"), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
        case = json.loads(result.stdout)
        assert case["overlapping_allocations"] == (enabled == 0)
        cases.append(case)
        print(result.stdout.strip())
    assert hashlib.sha256(source_path.read_bytes()).hexdigest() == original_hash
    (output / "report.json").write_text(json.dumps({"passed": True, "bsp_source_sha256": original_hash,
                                                   "cases": cases}, indent=2) + "\n")


if __name__ == "__main__":
    main()
