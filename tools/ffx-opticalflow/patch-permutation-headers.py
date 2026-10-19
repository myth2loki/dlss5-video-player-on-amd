#!/usr/bin/env python3
"""Add the one field SDK 2.3.0's shader-blob reader expects and no compiler in
the SDK's history emits.

ffx_internal_types.h's POPULATE_SHADER_BLOB_FFX reads `entryName` off every
PermutationInfo. The newest FidelityFX_SC still in the SDK's git history (1.1.2)
emits blobData/blobSize but no entry name: that field arrived with a compiler
AMD stopped shipping in the repository. Every other field 2.3.0 reads -
numSRVTextures / numSRVBuffers / numRTAccelerationStructures - is already
present, so the generated headers are otherwise the right shape and this is the
only edit they need.

The entry point is whatever -E named when the shader was compiled. The build
script compiles with `-E CS` and FFX's compute shaders keep that name, so "CS"
is the value the blob was compiled under; it is used only to look the entry up
in the DXIL when the pipeline is created.

Run after build-shader-blobs.sh, over its output directory.
"""
import re
import sys
from pathlib import Path

STRUCT = re.compile(r"^(\s*const unsigned char\* blobData;\n)", re.M)
INIT = re.compile(r"\{ (g_\w+?_size, g_\w+?_data,) ")


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        r"F:\dlss5-video\FidelityFX-SDK\Kits\FidelityFX\framegeneration\fsr3\dx12\ffx_sc_output2")
    entry = sys.argv[2] if len(sys.argv) > 2 else "CS"
    if not root.is_dir():
        print(f"not a directory: {root}")
        return 1

    patched = 0
    skipped = []
    for path in sorted(root.glob("*_permutations.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "entryName" in text:
            continue
        new, n_struct = STRUCT.subn(
            lambda m: m.group(1) + f'    const char*         entryName;   /* "{entry}" */\n',
            text, count=1)
        if n_struct != 1:
            skipped.append(f"{path.name}: no blobData field")
            continue
        new, n_init = INIT.subn(lambda m: "{ " + m.group(1) + f' "{entry}", ', new)
        if n_init == 0:
            skipped.append(f"{path.name}: no initialiser to patch")
            continue
        path.write_text(new, encoding="utf-8")
        patched += 1

    print(f"patched {patched} permutation headers")
    left = [p.name for p in sorted(root.glob("*_permutations.h"))
            if "entryName" not in p.read_text(encoding="utf-8", errors="replace")]
    print("without entryName:", left if left else "none")
    for s in skipped:
        print("  skipped", s)
    return 0 if not left else 1


if __name__ == "__main__":
    sys.exit(main())
