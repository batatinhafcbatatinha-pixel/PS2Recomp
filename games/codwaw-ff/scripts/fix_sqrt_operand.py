"""Patch already generated sources for the SQRT.S operand bug.

The recompiler emitted ctx->f[fd] = FPU_SQRT_S(ctx->f[fs]) but the R5900 SQRT.S reads the ft field (fs is always 0),
so every sqrt with ft != 0 took the root of f0. Each generated instruction follows its "// 0xPC: 0xWORD" comment;
the FPU_SQRT_S statement right after a SQRT.S comment gets its operand register replaced by ft.

usage: python scripts/fix_sqrt_operand.py [dir ...]   (default: project/runner and project/supplemental*)
"""
import glob
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dirs = sys.argv[1:] or [os.path.join(root, "project", "runner")] + glob.glob(os.path.join(root, "project", "supplemental*"))
comment = re.compile(r"// 0x[0-9a-fA-F]+: 0x([0-9a-fA-F]+)\s")
sqrt_stmt = re.compile(r"(ctx->f\[(\d+)\] = FPU_SQRT_S\(ctx->f\[)(\d+)(\]\);)")

files_changed = fixes = 0
for d in dirs:
    for path in glob.glob(os.path.join(d, "**", "*.cpp"), recursive=True):
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            lines = f.readlines()
        changed = False
        pending = None  # ft of the last SQRT.S comment, until its statement is seen
        for i, line in enumerate(lines):
            m = comment.search(line)
            if m:
                w = int(m.group(1), 16)
                pending = (w >> 16) & 31 if (w >> 26) == 0x11 and ((w >> 21) & 31) == 16 and (w & 0x3F) == 4 else None
                continue
            if pending is None:
                continue
            s = sqrt_stmt.search(line)
            if s:
                if int(s.group(3)) != pending:
                    lines[i] = line[:s.start()] + f"{s.group(1)}{pending}{s.group(4)}" + line[s.end():]
                    changed = True
                    fixes += 1
                pending = None
        if changed:
            files_changed += 1
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.writelines(lines)
print(f"fixed {fixes} sqrt statements in {files_changed} files")
