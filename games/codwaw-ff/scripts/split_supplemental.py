"""Split generated overlay bodies into independent parallel compilation units."""
import pathlib
import re
import sys

directory = pathlib.Path(sys.argv[1]).resolve()
destination = pathlib.Path(sys.argv[2]).resolve()
prefix_name = sys.argv[3] if len(sys.argv) > 3 else "codwaw_mpeg_chunk_"
if not re.fullmatch(r"[a-zA-Z0-9_]+", prefix_name):
    raise SystemExit("Invalid chunk prefix")
body = (directory / "integrated.inc").read_text()
functions = sorted(set(re.findall(r'^void (sub_\w+)\(', body, re.M)))
declarations = "".join(f"void {name}(uint8_t *, R5900Context *, PS2Runtime *);\n" for name in functions)
prefix = '#include "ps2_runtime.h"\nint32_t codWawMpegDelta();\nuint32_t codWawMpegAddress(uint32_t);\n' + declarations
for index in range(0, len(functions), 8):
    includes = "".join(f'#include "{(directory / (name + ".cpp")).as_posix()}"\n' for name in functions[index:index+8])
    (destination / f"{prefix_name}{index // 8:02d}.cpp").write_text(prefix + includes)
(directory / "bindings.inc").write_text(re.sub(r'^#include "sub_.*?\.cpp"\n', '', body, flags=re.M))
print(f"Generated {(len(functions)+7)//8} independent chunks")
