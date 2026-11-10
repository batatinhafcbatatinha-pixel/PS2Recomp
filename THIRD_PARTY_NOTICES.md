# Third-party notices

PS2Recomp is licensed under the GNU General Public License v3.0 (see `LICENSE`). This file lists the code and files
from other projects that are part of this repository, and the libraries the build downloads.

No game executable, disc image, BIOS, firmware, proprietary module, game asset or recompiled game code is part of this
repository. Code derived from a game is generated on the user's machine, from the user's own files (see `PROVENANCE.md`).

## Code from other projects in this repository

### PS2Recomp (upstream)
- Source: https://github.com/ran-j/PS2Recomp, by ran-j and contributors.
- License: GPL-3.0. This repository is a fork of it.

### God of War Native Port (runtime branch)
- Source: https://github.com/Waffle565/God-Of-War-Native-Port, branch `runtime`, by Waffle565.
- License: GPL-3.0 (`LICENSE` of that branch).
- Adopted 2026-10-26: the VU1 exact engine and native-block backend, the threaded and hardware ("hw") GS renderers,
  the GS swizzle/memory helpers and the VU1 native block generator. Their comments are marked `GOW-Port`.
- Files: `ps2xRuntime/include/runtime/ps2_vu1.h`, `ps2xRuntime/include/runtime/ps2_vu1_native.h`,
  `ps2xRuntime/include/runtime/ps2_perf.h`, `ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp`, `ps2_vu1_lower.cpp`,
  `ps2_vu1_upper.cpp`, `ps2_vu1_compiled.inl`, `ps2_vu1_native.inl`, `ps2_vu1_step.inl`,
  `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp`, `ps2xRuntime/include/runtime/gs/` (`gs_backend.h`,
  `gs_backend_state.h`, `gs_hw_backend.h`, `gs_shared_present.h`, `gs_swizzle.h`, `gs_threaded_backend.h`, `gs_types.h`,
  `gs_upload24.h`, `ps2_gs_common.h`, `ps2_gs_memory.h`), `ps2xRuntime/src/lib/gs/` (`gpu/gs_gl.cpp`, `gpu/gs_gl.h`,
  `gpu/gs_hw_backend.cpp`, `gpu/gs_texture_store.cpp`, `gpu/gs_texture_store.h`, `gs_gpu_helpers.h`,
  `gs_gpu_profile.h`, `gs_gpu_upload_ring.h`, `gs_inventory.h`, `gs_swizzle.cpp`, `gs_threaded_backend.cpp`,
  `ps2_gs_memory.cpp`), `ps2xRuntime/include/ThreadPriority.h`, `tools/ps2port/vu1n_gen.py`. All of them have been
  modified in this repository since.
- These are generic emulation components: no game code or game data came with them (the VU1 native blocks of a game
  are generated locally from the user's captures, see `PROVENANCE.md`).

### Shadow of the Colossus port of PS2Recomp (`sotc-port` branch)
- Source: https://github.com/TaylorNAlbarnaz/PS2Recomp, branch `sotc-port`, by TaylorNAlbarnaz.
- License: GPL-3.0.
- Reached this repository through the God of War Native Port: the OpenGL GS backend and its queue, swizzle and
  GS memory helpers, thread priorities. The comments cite the commits (`ac9efa0`, `8b51cb9`, `9bb389e`, `c419f26`).
- Files: `ps2xRuntime/include/ThreadPriority.h`, `ps2xRuntime/include/runtime/gs/gs_backend_state.h`,
  `gs_shared_present.h`, `gs_swizzle.h`, `gs_threaded_backend.h`, `ps2xRuntime/src/lib/gs/gpu/gs_gl.cpp`, `gs_gl.h`,
  `ps2xRuntime/src/lib/gs/gs_gpu_helpers.h`, `gs_swizzle.cpp`, `gs_threaded_backend.cpp`, `ps2_gs_memory.cpp`.

### ps2autotests (test vectors)
- Source: https://github.com/unknownbrackets/ps2autotests.
- Used in: `ps2xTest/src/ps2_float_hw_vectors.inc` (EE FPU results measured on a real PS2, from its `.expected`
  files).
- License: ISC.

```
Copyright (c) 2014, ps2autotests project contributors.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### SDK symbol database (ps2xAnalyzer)
- File: `ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`, added upstream (ran-j/PS2Recomp #130,
  "integrating sce-symbol-scanner"), generated from "sce-symbol-scanner symboldb resources".
- Contents: per SDK function, a SHA-1 hash, size, relocation offsets, referenced symbol names and SDK versions. It
  contains no machine code. Its origin and license have not been confirmed; it is listed here so it can be reviewed.

## Fonts (ps2xStudio)
- `ps2xStudio/external/Font/Font_1.ttf`: Inter, Copyright (c) 2016 The Inter Project Authors
  (https://github.com/rsms/inter), SIL Open Font License 1.1: `ps2xStudio/external/Font/LICENSE-Font_1-Inter.txt`.
- `ps2xStudio/external/Font/Font_2.ttf`: DejaVu (Bitstream Vera derivative), Bitstream Vera license, DejaVu changes in
  the public domain: `ps2xStudio/external/Font/LICENSE-Font_2-DejaVu.txt`.

## Downloaded at build time (not part of this repository)
CMake fetches these during configuration; each keeps its own license.

| Project | License |
| --- | --- |
| raylib (https://github.com/raysan5/raylib) | Zlib |
| rabbitizer (https://github.com/Decompollaborate/rabbitizer) | MIT |
| toml11 (https://github.com/ToruNiina/toml11) | MIT |
| ELFIO (https://github.com/serge1/ELFIO) | MIT |
| fmt (https://github.com/fmtlib/fmt) | MIT |
| nlohmann/json (https://github.com/nlohmann/json) | MIT |
| Dear ImGui (https://github.com/ocornut/imgui), imgui_club | MIT |
| SDL (https://github.com/libsdl-org/SDL) | Zlib |
| sse2neon (https://github.com/DLTcollab/sse2neon) | MIT |
| ImGuiColorTextEdit, ImGuiFileDialog | MIT |
| rlImGui (https://github.com/raylib-extras/rlImGui) | Zlib |
| GoogleTest | BSD-3-Clause |
| Google Benchmark | Apache-2.0 |
| libdwarf (https://github.com/davea42/libdwarf-code), uni-algo, boost-ext/reflect | see each project |

The PS Vita build (`vita/setup.sh`) downloads the PowerVR driver modules from PVR_PSP2
(https://github.com/GrapheneCt/PVR_PSP2) and gl4es4vita on the builder's machine and stages them outside this
repository; they are proprietary or third-party binaries and are not distributed here.
