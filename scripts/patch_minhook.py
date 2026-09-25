#!/usr/bin/env python3
"""Apply the small MinHook trampoline fix required by Crimson Desert's AVX camera prologue.

MinHook 1.3.3 uses HDE64, which does not decode VEX/AVX instructions.  The camera
pose function starts with a complete 7-byte instruction (already enough for the
entry JMP) followed by a VEX instruction.  Upstream trampoline.c decodes that
unused next instruction before noticing that enough bytes were collected, so
MH_CreateHook incorrectly returns MH_ERROR_UNSUPPORTED_FUNCTION.

tools/ is intentionally gitignored, therefore the fix is applied at build time
instead of modifying the local dependency checkout in the repository.
"""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_minhook.py <trampoline.c>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"MinHook trampoline source not found: {path}", file=sys.stderr)
        return 2

    text = path.read_text(encoding="utf-8")
    marker = "if ((hs.flags & F_ERROR) && oldPos < sizeof(JMP_REL))"
    if marker in text:
        print("MinHook AVX trampoline fix: already applied")
        return 0

    old = """        copySize = HDE_DISASM((LPVOID)pOldInst, &hs);\n        if (hs.flags & F_ERROR)\n            return FALSE;\n"""
    new = """        copySize = HDE_DISASM((LPVOID)pOldInst, &hs);\n        // Once the target has already supplied enough bytes for the entry JMP,\n        // this instruction is not copied into the trampoline. HDE64 does not understand VEX/AVX opcodes,\n        // so do not reject an otherwise complete trampoline only because this unused next instruction cannot be decoded.\n        if ((hs.flags & F_ERROR) && oldPos < sizeof(JMP_REL))\n            return FALSE;\n"""
    if old not in text:
        print("MinHook AVX trampoline fix: expected MinHook 1.3.3 code was not found", file=sys.stderr)
        return 1

    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("MinHook AVX trampoline fix: applied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
