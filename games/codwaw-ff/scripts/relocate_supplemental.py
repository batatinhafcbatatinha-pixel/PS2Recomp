"""Adapt generated MPEG code to the module's observed runtime load address.

Run only on freshly regenerated/integrated sources. PC constants and optimized
absolute data accesses become base-relative. Immediate instruction operands are
read from the relocated instruction in guest RAM rather than frozen at export.
"""
import pathlib
import re
import sys

directory = pathlib.Path(sys.argv[1])
LOW, HIGH = 0x9e7480, 0x9f87b0
address_pattern = re.compile(r"\b0x[0-9a-fA-F]+u?\b")

def addresses(line):
    def replace(match):
        value = int(match[0].rstrip("u"), 16)
        return f"codWawMpegAddress({match[0]})" if LOW <= value < HIGH else match[0]
    return address_pattern.sub(replace, line)

for source in [*directory.glob("sub_*.cpp"), directory / "integrated.inc"]:
    body = source.read_text()
    if "// MPEG relocation adapter" in body:
        raise SystemExit(f"Regenerate before adapting {source}")
    output = ["// MPEG relocation adapter\n"]
    instruction = None
    for line in body.splitlines(keepends=True):
        comment = re.search(r"// (0x[0-9a-f]+): (0x[0-9a-f]+)\s", line)
        if comment:
            instruction = tuple(int(value, 16) for value in comment.groups())
        if line.lstrip().startswith("//"):
            output.append(line)
            continue
        if "case 0x" in line:
            output.append(line)
            continue
        line = line.replace("switch (ctx->pc)", "switch (ctx->pc - codWawMpegDelta())")
        line = addresses(line)
        if instruction and source.name != "integrated.inc":
            pc, word = instruction
            opcode = word >> 26
            read = f"Ps2FastRead32(rdram, codWawMpegAddress({pc:#x}u))"
            if opcode == 15:
                line = re.sub(r"\(uint32_t\)\d+ << 16", f"(uint32_t)({read} & 0xffffu) << 16", line)
            if opcode == 9 or opcode >= 0x20:
                immediate = word & 0xffff
                if immediate & 0x8000:
                    immediate |= 0xffff0000
                pattern = rf"(ADD32\(GPR_U32\(ctx, \d+\), ){immediate}(\))"
                value = f"static_cast<int32_t>(static_cast<int16_t>({read} & 0xffffu))"
                line = re.sub(pattern, lambda m: m[1] + value + m[2], line)
        output.append(line)
    source.write_text("".join(output))
print(f"Adapted {directory}")
