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

**S5 — sceFont_Library integration: PASS.** The 44,052-byte PRX is rebased to
`0x08900000`, all 933 PSP relocations are applied offline, and the namespaced
541-function corpus is linked with the EBOOT. Minimal ModuleMgr serves the real
LBN path (`sceKernelLoadModule` UID `0x00070001`, `sceKernelStartModule` without
an initializer), the seven `sceLibFont` NIDs are bound to real exports, and five
of them executed: `NewLib`, `FindOptimumFont`, `Open`, `GetFontInfo` and
`SetAltCharacterCode`. The next dependency is `sceReg` (see
[S5 results](#s5-results--scefont_library-integration)).

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
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-reloc-seeds.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s6-prx.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s7-lbn-seek.patch

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

### 2. Native host (S2–S5)

```powershell
cmake -S . -B build/s5-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/s5-build -j 8
# EBOOT only (S2–S4 behaviour):
.\build\s5-build\Seto-static.exe private\ULUS10136_EBOOT.elf private\TF1.iso
# S5: load the relocated PRX, register its corpus and bind the seven NIDs:
.\build\s5-build\Seto-static.exe private\ULUS10136_EBOOT.elf private\TF1.iso `
    build\s5-font-reloc.elf

# names every guest file open/raw sector read and its mapped host path
$env:PSPRECOMP_TRACE_IO = '1'
```

Without `build/s5-font-reloc`, CMake configures the EBOOT-only host and reports
the missing corpus; passing the third argument then fails with a clear message.

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

## S5 results — sceFont_Library integration

The requested ISO range (LBN `0x52C0`, length `0x5BA0`, SHA-256
`ca621cb2212763ad85611e67e08d3e0296ce514b6ec731d7f05997c8d917c25a`)
is `sceFont_Library`, wrapped in `~SCE` and encrypted `~PSP` (tag
`0x457B05F0`). After stripping the 64-byte `~SCE` wrapper, the GPL `pspdecrypt`
tool at `c156627db7634d395c380c0a9589130f603307fc` decrypted the inner module;
its gzip output decompressed to a 44,052-byte ELF. No decryptor, game binary or
generated source is distributed here.

**Relocation.** `scripts/relocate_prx.py` applies the 933 PSP relocations
offline and rebases the ELF to `0x08900000` — RAM below the runtime heap
(`0x08C00000`), clear of the EBOOT module mapping and the boot TCB. It loads
segment 0 at `0x08900000` (`0x8750` bytes) and segment 1 at `0x08908750`
(`0x238` bytes, ending at `0x08908988`). The records are 274 `R_MIPS_32`,
529 `R_MIPS_26`, 59 `HI16` and 71 `LO16`.

The relocation table is not ordered by instruction address, so the usual
"next LO16 in the table" pairing is wrong for this module: three pairs form a
cycle, and one `lui` pairs with a `lbu` 476 bytes away. The script pairs each
`lui rt` with the LO16 whose instruction uses `rt` as its base, requires a
unique nearest match, and adjusts the HI16 high half for the sign-extended
LO16 carry. Every record is checked by re-reading the written word and
recomputing the expected value; `R_MIPS_26` targets are checked to land in the
image. The sample `lui v1,0` / `lbu a2,0` pair at `0x0`/`0x4` becomes
`lui v1,0x891` / `lbu a2,0x8750`, whose signed effective address is exactly
`0x08908750`.

The emitter needed two small patches: optional `PSPRECOMP_NAMESPACE` keeps
the PRX's generated symbols (`font_psp_func_08900000`, `font_recomp_register`)
separate from the EBOOT's, and `psp_collect_pointer_seeds` now reads each
relocation from the segment named in `r_info` instead of assuming segment 0.
The C corpus is emitted from the already-relocated ELF, so every direct call,
label and data address in it is the final guest address. The PRX corpus contains
no `$gp`-relative access; the module-info gp field follows its own `R_MIPS_32`
relocation to `0x089108C0`, consistent with the image and unused by this code.

**Loading and dispatch.** `font_prx.c` validates the two `PT_LOAD` segments,
places them in guest RAM, zeroes `.bss`, rejects overlaps and read-back
mismatches, then parses the module info and export table from the placed image.
The namespaced corpus registers 541 functions and 657 labels (dispatch grows from
16,389 to 17,587 entries); `psp_lookup(0x08900000)` hits
`font_psp_func_08900000`.

**ModuleMgr and NID binding.** `sceKernelLoadModule` (`0x977DE386`) recognizes
`disc0:/sce_lbn0x52c0_size0x5ba0`, checks the loaded image and returns the stable
UID `0x00070001`. `sceKernelStartModule` (`0x50F0C1EC`) accepts only that UID,
writes status 0 and does not run an initializer: the ELF entry is `0xFFFFFFFF`
and the export table has no `module_start`. The seven EBOOT imports are bound to
the real exports:

| NID | Export | Guest address |
|---|---|---|
| `0x67F17ED7` | `sceFontNewLib` | `0x08900000` |
| `0x099EF33C` | `sceFontFindOptimumFont` | `0x089004E0` |
| `0xA834319D` | `sceFontOpen` | `0x08900CD0` |
| `0x0DA7535E` | `sceFontGetFontInfo` | `0x08900EB4` |
| `0xEE232411` | `sceFontSetAltCharacterCode` | `0x089065E4` |
| `0xDCC80C2F` | `sceFontGetCharInfo` | `0x089011B0` |
| `0x980F4895` | `sceFontGetCharGlyphImage` | `0x089015B4` |

Each adapter calls `psp_call_from_host(export)`. It preserves the caller's
`$gp`: the PRX corpus has no `$gp`-relative access, and callbacks into the EBOOT
need the EBOOT's globals. `$sp`, `$ra`, the frame chain and the return value are
the generated code's normal path; no CPU state is copied.

**Execution.** After the load/start, the EBOOT reaches the font sequence and the
five handlers above are each called once. `entered PRX function 0x08900000`
(`sceFontNewLib`) was observed directly; 0 dispatch misses confirm the other
four transfers. The font library calls its own `sceReg` imports, which are not
implemented: `OpenRegistry` (`0x92E41280`, 3), `OpenCategory` (`0x1D8A762E`,
13), `CloseCategory` (`0x0CAE832B`, 13), `GetKeyInfo` (`0xD4475AA8`, 154) and
`GetKeyValue` (`0x28A8E98A`, 154); they return 0 and the library proceeds with
defaults. `GetCharInfo`/`GetCharGlyphImage` are not reached yet.

**EBOOT integrity.** With the PRX the `sceKernelLoadModule` retry is gone, the
game creates `snd_thread_main`, `snd_thread_seq` and `ehscript`, and the run ends
at the 8 s cap in the `ehscript` loop `0x391D8`; 13 context switches, 9 parks,
0 dispatch misses, 0 bad accesses, 0 traps, 96,256 ISO bytes. Without the third
argument the previous behaviour is unchanged. The upstream test suite passes
21/21.

Reproduction from the decrypted ELF (the extraction/decryption steps are the
same as in the earlier investigation):

```powershell
python scripts\relocate_prx.py build\s5-font.elf build\s5-font-reloc.elf
& $tool info  build\s5-font-reloc.elf
& $tool funcs build\s5-font-reloc.elf
New-Item -ItemType Directory -Force build/s5-font-reloc
& $tool emit build\s5-font-reloc.elf build\s5-font-reloc font
Get-ChildItem build/s5-font-reloc/*.c | ForEach-Object {
    & gcc -std=c11 -O2 -DPSPRECOMP_NAMESPACE=font_ -I build/psprecomp/include `
        -c $_.FullName -o "build/s5-font-reloc/$($_.BaseName).o"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($_.Name)" }
}
cmake -S . -B build/s5-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/s5-build -j 8
.\build\s5-build\Seto-static.exe private\ULUS10136_EBOOT.elf private\TF1.iso `
    build\s5-font-reloc.elf
```

## S6 results — modtitle and the first real frame

**Module-loader loop.** The loop at `0x391D8` is the body of the `ehscript`
thread (`0x391CC`), the EBOOT's asynchronous module loader. Its state word is
`0x00101710`; state 2 waits for the module UID at `0x00101708` (`s0+0x50`) and
only then calls `sceKernelStartModule`. State 1 enqueues a load request on the
`ehfile_queue` ring (`0x000FF410`, semas `ehfile_sema` `0x40003` and
`ehfile_queue` `0x40004`) with callback `0x00039320`, then wakes the caller and
polls; the caller suspends the loader. Three runtime gaps kept that handshake
from completing, each reproduced before the fix: `sceKernelWakeupThread` did not
reschedule a higher-priority sleeper (the loader's only yield);
`SuspendThread` only stored `TH_SUSPENDED`; and event-flag mode `0x10`
(`WAITCLEARALL`) was ignored, so `snd_thread_seq` repolled `SndSeqEvt` 12.7
million times. The runtime now yields after the wake, checks suspension at
baton reacquisition and clears the whole pattern on a match.

**libtitle.prx.** The EBOOT resolves the first module to the ISO extent
`disc0:/sce_lbn0x5e70_size0x38b12` (byte-identical to
`PSP_GAME/USRDIR/gmodule/libtitle.prx`, 232,210 bytes). `scripts/relocate_prx.py`
now reads the `.rel.*` sections of a one-segment PRX and rebases `e_entry`; it
places `modtitle` at `0x00640000`, one PT_LOAD (filesz `0x2C6E0`, memsz
`0xC2FE90`), with 6,095 relocations (1,030 `R_MIPS_32`, 2,683 `R_MIPS_26`,
1,191 HI16, 1,191 LO16). The 12.7 MB image does not fit the RAM window, so
`mem.c` gained a second module mapping. The relocated corpus has 1,361
functions. The table-order HI16/LO16 pairing is byte-identical on
`sceFont_Library` (SHA-256
`69211A8399A37742EBA6161A373C35E7BC884DF640B81906D04CED2C3284DFA5`):
its three differing pair choices (the `0x2354`/`0x2418`/`0x238C` cycle) emit
the same high halves. The old register/nearest pairing cannot pair `modtitle`;
table order pairs the branch-delay `lui` at `0x12208` with its `addiu` at
`0x122A4`.

**Imports and ModuleMgr.** All 197 `libehsys` imports resolve from the EBOOT's
own `modehsys` export table (1,245 functions) to real guest addresses; none are
missing. The title corpus is compiled with
`-Dpsp_hle_call=title_import_forward`, so every import stub calls the real EBOOT
function through `psp_call_from_host()`, leaving `$a0-$a3`, stack arguments,
`$sp/$ra` and the frame chain on the generated path. `module_start` and
dispatched callbacks run with the module's `$gp` (`0x006746C0`); each import
switches to the EBOOT's (`0x000F4CB0`) and restores the caller's. The title
corpus has no `$gp`-relative access. The imports from the eight other game
libraries are unresolved and are not reached at init.
`sceKernelLoadModule` accepts the LBN extent as UID `0x00070002`;
`sceKernelStartModule` calls `module_start` at `0x00640000`, and the EBOOT
loader later calls the first registered callback at `0x00640034`.

**Execution and frame.** Both transfers were observed directly:
`title: entered recompiled function 0x00640000 (module_start)` and
`title: entered recompiled callback 0x00640034`; the `LoadModule` retry is gone.
The game advanced to 597 vblanks, 600 GE lists, 150,786 commands, 22,590
vertices and 43,396,860 pixels. `psp_display_capture()` wrote
`build/s6-frame.ppm` (480x272, 417 colours, 3,466 non-black pixels): the game's
golden character sprite on a circular base, bottom right. That is a real,
visually valid frame produced by the original code; the rest of the screen is
still black because this is the loading/title transition. Display front
`0x04088000`; the GE was drawing the next frame into `0x04000000`, stride 512,
vertex type `0x000102`.

**VFPU.** The EBOOT's colour path executed the previously unknown `vbfy1`
(butterfly: `rs0+rs1`, `rs0-rs1`, ...) 384 times. It is now decoded and
implemented as a pair-wise VFPU op; the instruction traps went from 384 to 0.
That fix is what changed the frame from a single colour to the sprite. 19 bad
accesses remain in EBOOT code reached after StartModule (first read at
`0x8662CE15` from `0x00059FE0`); the `$gp` transition work leaves them
unchanged, so they have another cause. They are the next fidelity issue, not a
blocker for the captured frame.

**Integrity.** 23,499 dispatch entries (EBOOT + font + modtitle), 0 misses,
0 traps, 5 threads, 4,382 switches / 4,377 parks, 1,271,680 ISO bytes; the S5
font path still enters `0x08900000`; upstream tests 21/21.

Reproduction (the S5 font corpus and relocated ELF must already exist):

```powershell
& $tool extract private/TF1.iso libtitle.prx build/s6-title
python scripts\relocate_prx.py `
    build\s6-title\PSP_GAME_USRDIR_gmodule_libtitle.prx `
    build\s6-title-reloc.elf --base 0x00640000
& $tool emit build\s6-title-reloc.elf build\s6-title-reloc title
cmake -S . -B build/s6-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/s6-build -j 8
.\build\s6-build\Seto-static.exe private\ULUS10136_EBOOT.elf private\TF1.iso `
    build\s5-font-reloc.elf build\s6-title-reloc.elf
```

## S7 results — LBN seek correction (partial)

The game seeks to byte `165537792` in the LBN extent starting at ISO byte
`282034176`. The S6 implementation sought to byte `165537792` of the ISO
instead. `sceIoLseek` now treats all three seek origins and the returned position
relative to the LBN extent, while keeping reads within that extent. The first
bad read was actually `lbu $v0, 16($s3)` at `0x0005BE8C`; `0x00059FE0`
was merely the last traced function. `$s3` came from `0x0005A134` via `$v0`.
After the LBN fix the 19 bad accesses disappear (zero observed), but this does
not establish why the game remains in loading.

At 4, 12 and 30 seconds the framebuffer changed, but all three samples still
showed the loading sprite rather than the title screen. A 30-second run reached
1,329 vblanks and 1,332 GE lists, with zero dispatch misses, bad accesses or
traps; five threads started. The title callback's state at `0x0126C718` and
substate at `0x0126C71A` were both zero in repeated samples; the condition
needed to advance is not yet established. Upstream tests passed 21/21.
The synthetic LBN seek regression can be run with:

```powershell
cmake --build build/s6-build --target test-lbn-seek
.\build\s6-build\test-lbn-seek.exe
```

## Limitations

- `sceReg` is not implemented: the font library's registry reads return 0 and
  it uses defaults. `GetCharInfo`/`GetCharGlyphImage` and text rendering are not
  reached yet.
- Only `libtitle.prx` is integrated; the other game PRXs are not recompiled.
  The captured frame is the loading/title sprite, not a completed scene.
- The LBN seek fix eliminates the 19 previously observed bad accesses, but the
  next loading-to-title blocker is not yet identified.
- Audio is paced by the runtime but not played; controls are not implemented.
- 272 VFPU + 16 `vcmov` instructions remain traps in the EBOOT corpus; none
  were reached by this run. The title corpus has 22 VFPU traps, also unreached.
- 2,889 functions without `jr $ra` and 176 `scattered` functions remain discovery
  caveats; they did not affect the EBOOT run.
- No window; the goal is the real code path, not play.

## Repo contents

Tracked: this README, `.gitignore`, `host.c`, `umd.c`, `fpl.c`, `font_prx.c`,
`title_prx.c`, `scripts/relocate_prx.py`, `CMakeLists.txt`, and six small
patches against the pinned psprecomp. `build/` (tool clone, generated C,
relocated ELF, objects, executable) and `private/` (decrypted EBOOT, ISO) are
ignored.
