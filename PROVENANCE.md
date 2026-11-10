# Provenance policy

PS2Recomp turns a game the user owns into a native PC program on the user's own machine. This repository holds the
recompiler, the runtime and per-game configuration only. These rules keep it that way; every contribution follows
them, whoever (or whatever tool) writes it.

## 1. What never goes into this repository
- Game executables, disc images, IRX/IOP modules from discs, BIOS, firmware, console system modules, keys.
- Game assets (textures, audio, video, text) and dumps of game memory, VU micro memory or GS memory.
- Code generated from a game: recompiled C++, translated VU micro programs, AOT-compiled IRX code, hot-patch
  libraries. They are produced locally (under `.local/` or a port's build folder, both git-ignored).
- Third-party binaries whose license does not allow redistribution (e.g. the PS Vita PowerVR driver modules, which
  `vita/setup.sh` downloads on the builder's machine).
- Mechanisms that circumvent DRM or other technical protection measures.

## 2. Reimplementing PS2 SDK and system functions
The runtime replaces parts of the PS2 system software and of the SDK libraries linked into games (syscalls, IOP
modules, `sce*` functions) with its own code. That code must be an independent implementation:
- Write it from the observable behaviour (arguments, return values, effects on memory and hardware registers), public
  documentation and specifications, open-source projects with a compatible license, and tests.
- Do not copy, translate line by line or adapt Sony's code: no SDK sources or headers, no assembly or decompiler output
  of SDK libraries, BIOS or IRX modules, whether in code, comments or commit messages.
- Keep only what interoperability needs: function names, syscall/RPC numbers, structure layouts and constants that the
  game's code depends on. Note where each came from.
- If a function cannot be implemented without copying protected expression, leave it unimplemented (the recompiled
  guest code keeps running it) and document the limitation.
- New SDK/system implementations say in a comment or the commit message what they were written from (behaviour
  observed with which test, which public document, which open project).
- Functions are recognised in a game by hashes, sizes and relocation layouts computed from the user's files; the
  repository stores those identifiers, not the functions' bytes.

## 3. Game-specific files (`games/`)
Allowed: configuration (addresses, function names, sizes, hashes of the user's files), notes describing behaviour in
prose, `handlers.cpp` wrappers that call the locally recompiled function and add host-side logic, patches that write
a few values (`.pnach`), scripts that run on the user's files. Not allowed: translated or decompiled game code, game
strings or data beyond short identifiers, dumps.

## 4. Analysis output
Ghidra/REA/decompiler output, disassembly, captures and logs of a game stay in local, git-ignored folders. Notes may
refer to addresses and decompiler names (`FUN_...`) and describe what code does; they do not paste its bodies.

## 5. Third-party code
Code from other projects keeps its license and authorship: record it in `THIRD_PARTY_NOTICES.md` with the source,
license and files, and only take code whose license is compatible with GPL-3.0.

## 6. Before publishing
Check that a commit adds no binary or generated file (`git diff --cached --stat`), that `.gitignore` covers new
generated paths, and that removed files are also gone from what is published (a `.gitignore` entry does not remove
files already committed; history rewrites are a separate, explicit decision).

This policy is a practical guide, not legal advice; components whose legality is in doubt are reviewed with a
qualified lawyer before they are published.
