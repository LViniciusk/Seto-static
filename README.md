# Seto-static — Tag Force 1 static recompilation

Static recompilation of **Yu-Gi-Oh! GX Tag Force (PSP, ULUS10136)** with
[psprecomp](https://github.com/sp00nznet/psprecomp), from the decrypted `EBOOT.BIN`
to a native Windows executable that loads the real module and starts its code.

**S1 — translate + compile: PASS.** 4,950 functions, 108,087 instructions, 80/80
generated C units compiled.

**S2 — native host bring-up: PASS.** The executable loads the EBOOT, enters
`module_start` at `0x00000000`, runs the real translated code (128 KiB + 256 KiB
allocations, `user_main` thread created and started) and stops in the boot loop at
`0x0002B9F4`, blocked on unimplemented `sceUmdUser` (see [S2 results](#s2-results)).

**S3 — UMD boot unblock: PASS.** A minimal `sceUmdUser`, the raw `umd1:` block
device backed by the retail ISO, and the FPL calls the boot uses take the game past
`0x2B9F4`: 47 real sector reads (96,256 bytes), ISO9660 mount, file worker thread,
and the display/GE initialization calls (see [S3 results](#s3-results)).

**S4 — real thread scheduling: PASS.** GCC/MinGW silently ignored
`__declspec(thread)`, so the scheduler's per-thread state was global and started
threads skipped the CPU baton. `_Thread_local` fixes it: the boot thread and
`user_main` now alternate on separate host threads without the `psp_ra_is_jump`
crash, and the game reaches `sceKernelLoadModule` (see [S4 results](#s4-results)).

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
| Exports | 1,247 functions (first export library: `module_start` NID `0xD632ACDB` at `0x0`) |

## Reproduce

Requires Git, CMake, and a C compiler. Tested with CMake 3.30.2 and
MSYS2 UCRT64 GCC 15.2.0 on Windows 11. Run from the repo root.

### 1. Recompiler and generated C (S1)

```powershell
git clone https://github.com/sp00nznet/psprecomp build/psprecomp
git -C build/psprecomp checkout 5ca18fac1691a4a7b40ae683dabdf72a5703d2b8
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-vfim-float.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-umd-raw.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-real-threads.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-namespace.patch

cmake -S build/psprecomp -B build/psprecomp-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/psprecomp-build -j 8

$tool  = "build/psprecomp-build/tools/allegrexrecomp/allegrexrecomp.exe"
$eboot = "private/ULUS10136_EBOOT.elf"
& $tool info  $eboot
& $tool cover $eboot
& $tool funcs $eboot
New-Item -ItemType Directory -Force build/tagforce
& $tool emit $eboot build/tagforce tagforce
```

`emit` is deterministic: a second run reports `0 of 81 files changed`.

### 2. Native host (S2/S3)

```powershell
cmake -S . -B build/s2-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/s2-build -j 8
.\build\s2-build\Seto-static.exe private\ULUS10136_EBOOT.elf private\TF1.iso

# names every guest file open/raw sector read and its mapped host path
$env:PSPRECOMP_TRACE_IO = '1'
```

The host uses the upstream runtime as-is, except that the loader's constructor
heuristic is disabled: this EBOOT has no `.ctors` section, and the heuristic matches
eight `.data` pointers (first target `0x000EA0D8`) that would be dispatched as code.

## S1 results

| Metric | Value |
|---|---|
| Functions discovered | 4,950 (seeds: entry + 512 exports + 2,303 relocation pointers) |
| Instructions in discovered code | 108,087 |
| `.text` reached | 432,348 of 439,740 bytes incl. import stubs — **98.3%** (98.8% of `.text` alone) |
| Computed jumps / resolved tables | 115 sites / 168 tables → 3,028 targets |
| VFPU instructions | 276 (0.26%) — emitted as explicit traps |
| Invalid words | 43 (0.04%) — `.word` data misread as code |
| Emitted C | 78 `tagforce_funcs_NNN.c` + header + imports + register |
| Translation units compiled | **80 / 80**, 80 objects, GCC 15.2.0 `-std=c11 -O2`, no errors |

## S2 results

`Seto-static.exe`: 7,180,229 bytes, linked by CMake/GCC.

| Check | Result |
|---|---|
| Loader | image `0x00000000..0x000ECCC0`, `PT_LOAD` memsz `0x631A80`, entry `0x00000000`, `$gp 0x000F4CB0`, 0 constructors |
| Dispatch | **16,389 entries** registered (4,950 functions + 11,439 interior labels), 0 misses |
| Entry | `reached translated entry 0x00000000` (`module_start`, export NID `0xD632ACDB`) |
| Executed services | `sceKernelAllocPartitionMemory` (uids `0x10000` @ `0x09FF0000`, `0x10001` @ `0x08C00000`), `sceKernelGetBlockHeadAddr`, `sceKernelCreateThread`, `sceKernelStartThread`, `sceKernelCpuSuspendIntr`/`ResumeIntr`, `sceIoOpen` |
| Guest thread | `user_main` at `0x000000D4`, 64 KiB stack — created and started (1/1) |
| Integrity | 0 dispatch misses, 0 bad memory accesses, 0 translated-instruction traps |
| HLE | 11 unimplemented calls, 19 calls returning zero |

### S2 first blocker

The boot path calls the helper at **`0x0002B9F4`**, which retries forever:

```
sceUmdGetDriveStat()                    NID 0x6B4A146C  unimplemented -> 0
sceUmdWaitDriveStat(0x20)               NID 0x8EF08FCE  unimplemented -> 0
sceIoOpen("umd1:", 1)                   -> 0x80010002 (ENOENT, no ./disc tree)
```

The helper loops until one of a fixed set of errors appears; `ENOENT` is not one of
them, so it spins. `sceUmdActivate` (`0xC6183D47`) is also unimplemented, and the
`ThreadManForUser` fixed-pool calls `sceKernelCreateFpl` (`0xC07BB470`) /
`sceKernelAllocateFpl` (`0xD979E9BF`) return 0.

## S3 results

`sceUmdUser` is registered from `umd.c` only after a real ISO was validated
(ISO9660 PVD `CD001` at sector 16). The raw `umd1:`/`umd0:` block device maps to
that file in `iofilemgr.c`: lseek offsets and read counts are in 2048-byte sectors,
exactly as the game uses them. `fpl.c` adds `sceKernelCreateFpl`,
`sceKernelAllocateFpl` and `sceKernelDeleteFpl` for the boot's internal heap.

| Check | Result |
|---|---|
| UMD | `sceUmdActivate`, `Deactivate`, `GetDriveStat`, `WaitDriveStat`, `WaitDriveStatCB`; `umd1:` opened read-only against `TF1.iso` |
| Real disc reads | **47 sector reads / 96,256 bytes**: sector 16 (PVD), 18 (L path table), 22+ (directory records) |
| Boot loop | `0x2B9F4` passed; ISO9660 mount `0x2B774` completed; 48 `sceUmdGetDriveStat`, 98 `sceIoLseek`, 47 `sceIoRead` |
| Later stage | file worker `0x2B2C8` created; `sceDisplaySetMode`/`SetFrameBuf`, `sceGeListEnQueue`, `sceGeDrawSync`, `sceGeListSync`, `sceGeSetCallback`, `sceGeListUpdateStallAddr` |
| Integrity | 0 dispatch misses, 0 bad accesses, 0 instruction traps; 1 unimplemented call (`sceGeEdramSetAddrTranslation`, returns 0) |

The run ends at the host's 8 s bring-up cap, in the worker loop at `0x2B3B0`, while
it waits on `sceKernelWaitSema("ehfile_queue")`. Under the default inline scheduler
`sceKernelStartThread` runs that worker inline, so its creator never returns its
frame. S4 fixes the `PSPRECOMP_REAL_THREADS=ON` path that was crashing here.

## S4 results

Root cause, reproduced under gdb: on this toolchain `HOST_TLS` was
`__declspec(thread)`, which GCC/MinGW ignores ("thread attribute directive
ignored"). `g_tls_me` and `g_tls_depth` were therefore globals:
- a new guest thread saw the creator's `depth=2` and returned from
  `baton_acquire()` without waiting;
- the boot thread and that thread executed guest code concurrently on the shared
  `psp_cpu`;
- a `PSP_CALL_BEGIN` captured a `psp_frame` from another host stack
  (`0xA17B1FFAA0` outside `[0xA179200000,0xA17A200000)`), and
  `psp_ra_is_jump()` dereferenced it.

The fix is `HOST_TLS _Thread_local` (same compiler, honoured), plus copying the
creator's `$k0` into started threads so newlib finds its reent
(`patches/psprecomp-5ca18fa-real-threads.patch`). With it, the same run no longer
crashes: the boot thread parks on a delay, `user_main` starts on its own host
thread, the UMD mount and reads are unchanged, and the next call is
`sceKernelLoadModule`.

| Check | Result |
|---|---|
| Scheduler | `PSPRECOMP_REAL_THREADS=ON`; boot thread + `user_main` on separate host threads; 1 baton switch, 0 dispatch misses, 0 bad accesses, 0 traps |
| Upstream tests | 21/21 pass with `PSPRECOMP_REAL_THREADS=ON` |
| Game progress | UMD mount and 47 reads (96,256 bytes) still complete; worker `0x2B2C8` created |
| Crash | the `psp_ra_is_jump` SIGSEGV is gone; last function `0x0000D028`, no fault |

**Next blocker.** The game calls:

```
sceKernelLoadModule("disc0:/sce_lbn0x52c0_size0x5ba0")
```

312,231 times (NID `0x977DE386`, unimplemented, returns 0). LBN `0x52C0` is the
`~SCE` module at sector 21184, size `0x5BA0` (23,456 bytes). The worker thread is
created but not started until this module loads, so the run stops on the dispatch
budget. `ModuleMgrForUser` (load/relocate/start the PRX, module UIDs) is the next
subsystem; it is not scheduler work.

## Limitations

### S5 investigation — blocked on multi-module integration

The requested ISO range (LBN `0x52C0`, length `0x5BA0`, SHA-256
`ca621cb2212763ad85611e67e08d3e0296ce514b6ec731d7f05997c8d917c25a`)
is `sceFont_Library`, wrapped in `~SCE` and encrypted `~PSP` (tag
`0x457B05F0`). The EBOOT imports seven `sceLibFont` functions; the upstream
runtime provides no corresponding HLE. Returning a module UID without providing
the library would not be a valid load.

The existing `allegrexrecomp decrypt` only inspected the header. An isolated
GPL-licensed `pspdecrypt` checkout at `c156627db7634d395c380c0a9589130f603307fc`
successfully decrypted the inner `~PSP` after removing its 64-byte `~SCE`
wrapper; its gzip output decompressed to a 44,052-byte ELF. No decryptor code,
game binary, or generated source is distributed here. The ELF contains two
PT_LOAD segments, 28 function exports, 11 imports (`sceReg` and
`IoFileMgrForUser`), PSP-specific relocations, and entry `0xFFFFFFFF`.
`allegrexrecomp funcs` found 543 functions and 7,821 instructions; `emit`
generated 11 C translation units, all compiled with GCC 15.2.0. This does not
prove the module can execute.

Integration is incomplete: both modules are linked at address zero. The emitter
patch adds optional `PSPRECOMP_NAMESPACE` for generated external symbols; compiling
the font corpus with `-DPSPRECOMP_NAMESPACE=font_` isolates its functions, import
thunks, labels and registration without changing EBOOT names. All 11 font units
compiled, and a Windows executable linked them with the EBOOT and runtime. This
is a link check only: it does not register or execute the PRX.
The upstream loader only copies the first PT_LOAD and does not relocate a
second module. Correct integration requires isolated generated symbols,
rebasing/relocation, import/export binding, and handling module startup;
none is replaced with fabricated success. The S4 host was rerun with the real
EBOOT and ISO: it still reaches the dispatch budget while retrying
`sceKernelLoadModule` (`0x977DE386`), with 0 dispatch misses, 0 bad memory
accesses and 96,256 ISO bytes read. No PRX was loaded or started.

The PRX's PSP relocation segment contains 274 type 2, 529 type 4, 59 type 5,
and 71 type 6 records (the MIPS 32, 26, HI16 and LO16 encodings). None has yet
been applied. Guest image relocation alone is insufficient: emitted C also
contains link-time addresses. In particular the EBOOT and PRX cannot safely
share the current dispatch table at address zero. No font NIDs have been bound.

Static inspection of the generated EBOOT identifies the seven font imports:

| NID | Function | EBOOT call site |
|---|---|---|
| `67F17ED7` | `sceFontNewLib` | `0x41C18` |
| `099EF33C` | `sceFontFindOptimumFont` | `0x41C68` |
| `A834319D` | `sceFontOpen` | `0x41C88`, `0x41CE8` |
| `0DA7535E` | `sceFontGetFontInfo` | `0x41D08` |
| `EE232411` | `sceFontSetAltCharacterCode` | `0x41D58` |
| `DCC80C2F` | `sceFontGetCharInfo` | `0x42C44` |
| `980F4895` | `sceFontGetCharGlyphImage` | `0x42CC4` |

These are static call sites, not observed executions: `0x41BB8` retries module
loading at `0x41BD0`, then calls `sceKernelStartModule` at `0x41BF0`, before
reaching the font calls. A font HLE therefore needs guest allocation callbacks,
valid font handles and actual PGF metrics and glyphs, not just a module UID.
Local PPSSPP font assets exist, but their provenance and compatibility have not
been established. The PRX alternative still needs relocation and symbol/import
binding; neither alternative has yet been implemented or validated.

Reproduction of the analysis, using the existing S1 tool build and a local ISO:

```powershell
python -c "from pathlib import Path; f=open(r'private/TF1.iso','rb'); f.seek(0x52c0*2048); d=f.read(0x5ba0); Path('build/s5-font.psp').write_bytes(d[64:])"
# Build pspdecrypt separately under WSL from the revision above; see its GPL license.
wsl build/pspdecrypt/pspdecrypt -o build/s5-font.gz build/s5-font.psp
python -c "import gzip; from pathlib import Path; Path('build/s5-font.elf').write_bytes(gzip.decompress(Path('build/s5-font.gz').read_bytes()))"
& $tool info build/s5-font.elf
& $tool funcs build/s5-font.elf
New-Item -ItemType Directory -Force build/s5-font
& $tool emit build/s5-font.elf build/s5-font font
# Compile each generated PRX unit with its own namespace; EBOOT stays unchanged.
Get-ChildItem build/s5-font/*.c | ForEach-Object {
    & gcc -std=c11 -O2 -DPSPRECOMP_NAMESPACE=font_ -I build/psprecomp/include `
        -c $_.FullName -o "build/s5-font/$($_.BaseName).o"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($_.Name)" }
}
```

- `sceKernelLoadModule` / `sceKernelStartModule` are not implemented; the game
  retries the `~SCE` module at LBN `0x52C0`.
- `sceGe` command stream, renderer, audio and controls are not implemented.
- 276 VFPU + 16 `vcmov` instructions remain traps; none were reached by this run.
- 2,889 functions without `jr $ra` and 176 `scattered` functions remain discovery
  caveats; they did not affect S4 execution.
- No window, renderer, audio or controls; the goal is the real code path, not play.

## Repo contents

Tracked: this README, `.gitignore`, `host.c`, `umd.c`, `fpl.c`, `CMakeLists.txt`, and
three small patches against the pinned psprecomp. `build/` (tool clone, generated C,
objects, executable) and `private/` (decrypted EBOOT, ISO) are ignored.
