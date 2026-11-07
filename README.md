# Seto-static — Tag Force 1 EBOOT static recompilation (S1)

First real static recompilation pass over **Yu-Gi-Oh! GX Tag Force (PSP, ULUS10136)**:
the decrypted `EBOOT.BIN` is translated to C by
[psprecomp](https://github.com/sp00nznet/psprecomp) and the whole generated corpus is
compiled to native Windows objects.

**Status: PASS** — real Allegrex code translated (4,950 functions, 108,087 instructions),
80/80 generated translation units compiled. Linking a runnable host is out of scope for S1.

## Input

Decrypted `EBOOT.BIN` (plain `ELF32 MIPS LE`, PRX), kept out of Git in `private/`:

| | |
|---|---|
| File | `ULUS10136_EBOOT.elf` |
| Size | 1,110,624 bytes |
| SHA-256 | `D4D0EFC3D6C49A7BA8142ADBD316ED006A3035A572169E9EF04C6CD0DABA7A91` |
| Module | `modehsys` |
| `.text` | `0x00000000` + 437,548 bytes (109,387 words) |
| Import stubs | `0x0006AD2C` + 2,192 bytes |
| Exports | 1,247 functions |

## Reproduce

Requires Git, CMake, and a C compiler. Tested with CMake 3.30.2 and
MSYS2 UCRT64 GCC 15.2.0 on Windows 11. Run from the repo root.

```powershell
# 1. Tool, pinned revision
git clone https://github.com/sp00nznet/psprecomp build/psprecomp
git -C build/psprecomp checkout 5ca18fac1691a4a7b40ae683dabdf72a5703d2b8
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-vfim-float.patch

# 2. Build the recompiler
cmake -S build/psprecomp -B build/psprecomp-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/psprecomp-build -j 8
# optional: ctest --test-dir build/psprecomp-build -C Release

# 3. Analyze
$tool  = "build/psprecomp-build/tools/allegrexrecomp/allegrexrecomp.exe"
$eboot = "private/ULUS10136_EBOOT.elf"
& $tool info  $eboot
& $tool cover $eboot
& $tool funcs $eboot

# 4. Translate to C
New-Item -ItemType Directory -Force build/tagforce
& $tool emit $eboot build/tagforce tagforce

# 5. Compile every generated unit to a native Windows object
New-Item -ItemType Directory -Force build/tagforce-obj
Get-ChildItem build/tagforce/*.c | ForEach-Object {
    & gcc -std=c11 -O2 -I build/psprecomp/include -c $_.FullName `
          -o "build/tagforce-obj/$($_.BaseName).o"
}
```

`emit` is deterministic: a second run reports `0 of 81 files changed`.

## Results

Translation (`allegrexrecomp funcs` / `cover`, then `emit`):

| Metric | Value |
|---|---|
| Functions discovered | 4,950 (seeds: entry + 512 exports + 2,303 relocation pointers) |
| Instructions in discovered code | 108,087 |
| `.text` reached | 432,348 of 439,740 bytes incl. import stubs — **98.3%** (98.8% of `.text` alone) |
| Computed jumps / resolved tables | 115 sites / 168 tables → 3,028 targets |
| Functions touching VFPU | 48 |
| VFPU instructions | 276 (0.26%) |
| Invalid words | 43 (0.04%) |
| Emitted C | 78 `tagforce_funcs_NNN.c` + header + `tagforce_imports.c` + `tagforce_register.c` |
| Translated function entries | 4,950 functions, 11,439 dispatchable interior entries |
| Instruction statements in C | 108,065 |

Native compilation:

| Metric | Value |
|---|---|
| Translation units | **80 / 80 compiled** (78 functions units + imports + register) |
| Objects produced | 80 `.o`, 9,642,303 bytes |
| Compiler | `gcc 15.2.0` (MSYS2 UCRT64), `-std=c11 -O2` |
| Errors | none |

## Limitations (honest traps, not mocks)

The emitted corpus contains 395 instructions that compile but trap at run time instead of
being translated:

| Trap | Count | Meaning |
|---|---|---|
| `vfpu?` | 276 | VFPU opcodes recognised by the decoder, not yet named/implemented by the emitter |
| `vcmov` | 16 | VFPU conditional move, emitter has no translation |
| `?` | 42 | `.word` values the decoder cannot name — confirmed data misread as code: module info/GP (`0x000F4CB0` at `0x6B83C`) and ASCII strings |
| `break` | 27 | `break` instructions |
| others | 34 | Emitter falls through to a trap for some instructions whose destination is `$zero` (12 `lb`, 6 `sll`, 3 `sra`, 2 `add`, …), plus one `floor.w.s`, one `ceil.w.s`, and 5 branch/likely slots |

`cover` (linear over `.text`, which is mostly data) reports 99.97% decodable words,
0 unknown and 29 VFPU; the numbers above are measured over *discovered code*, which is the
honest denominator.

Not attempted in S1: linking an executable, runtime/HLE bring-up, GPU/audio. `no jr $ra`
was reported for 2,889 functions — tail calls or discovery traced into data; 176 functions
are flagged `scattered` (suspect boundaries). These do not affect translation or compilation.

## Repo contents

Only this README, `.gitignore`, and the one-patch fix for the pinned recompiler are tracked.
`build/` (tool clone, generated C, objects) and `private/` (decrypted EBOOT) are ignored.
