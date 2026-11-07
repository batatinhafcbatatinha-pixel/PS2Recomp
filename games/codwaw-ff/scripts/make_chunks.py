"""Group the generated recompiled sources (project/runner/*.cpp) into chunk files.

CMake needs ~17 minutes to generate a build for 80k sources; with ~2.5k chunk
files that include the originals it takes seconds. Chunks are rewritten only
when their content changes, so ninja recompiles just the affected chunks.

usage: python scripts/make_chunks.py [runner_dir] [chunk_dir] [batch]
"""
import os
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
runner = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "project", "runner"))
chunks = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "project", "runner-chunks"))
batch = int(sys.argv[3]) if len(sys.argv) > 3 else 32

# main.cpp belongs to the executable; register_functions.cpp is huge and gets its own chunk.
SOLO = {"register_functions.cpp"}
names = sorted(n for n in os.listdir(runner) if n.endswith(".cpp") and n != "main.cpp")
rest = [n for n in names if n not in SOLO]
groups = [(f"chunk_{i // batch:05d}.cpp", rest[i:i + batch]) for i in range(0, len(rest), batch)]
# CMake compiles chunk_register.cpp without optimization (it is only table stores).
if "register_functions.cpp" in names:
    groups.append(("chunk_register.cpp", ["register_functions.cpp"]))

os.makedirs(chunks, exist_ok=True)
wanted = set()
changed = 0
rel = os.path.relpath(runner, chunks).replace("\\", "/")
for name, group in groups:
    wanted.add(name)
    text = "".join(f'#include "{rel}/{n}"\n' for n in group)
    path = os.path.join(chunks, name)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                continue
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    changed += 1

removed = 0
for n in os.listdir(chunks):
    if n.endswith(".cpp") and n not in wanted:
        os.remove(os.path.join(chunks, n))
        removed += 1

print(f"{len(names)} sources -> {len(groups)} chunks ({changed} written, {removed} removed) in {chunks}")
