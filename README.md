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

**S8 — title input: PASS.** A real Windows Enter reaches the recompiled title
through `sceCtrlPeekBufferPositive`, runs the original fade and reaches the
interactive title menu (scene state 3) in 5/5 diagnostic runs. The menu itself
needs a further selection, which is S9 (see
[S8 results](#s8-results--title-input)).

**S9.1 — live Win32 window: PASS.** A native 960 × 544 window presents the
PSP front buffer at ~30 FPS with correct colours, orientation and 30:17
letterboxing on resize. The existing input path still reaches START → fade →
scene state 3 in 5/5 window runs (see
[S9.1 results](#s91-results--live-win32-window)).

**S9.2 — title menu: PASS.** The scene state 3 handler uses D-pad Up/Down
(`0x10`/`0x40`) to move the cursor and CROSS (`0x4000`) to confirm. With those
controls, the original menu advances from cursor 1 to 0 and confirmation changes
the scene state from 3 to 4, presenting the next scene, in 5/5 headless runs
(see [S9.2 results](#s92-results--title-menu)).

**S10 — field renders; Classroom travel blocked: PARTIAL.** The goal is a visible,
navigable Classroom before a duel. Free Duel is locked on a new game, and the
first class precedes the Crowler duel, so the field has to be usable first.

Three fixes were needed to see it:

1. **VFPU `vtfm` orientation.** The helper multiplied with the matrix the wrong
   way round. The fix, plus the measured `vtfm3.t`/`vtfm4.q` vectors from
   [pspautotests](https://github.com/hrydgard/pspautotests/blob/master/tests/cpu/vfpu/matrix.expected),
   turned seven failing lanes into exact matches.
2. **Homogeneous transforms.** Three `vhtfm4` instructions were emitted as 3x3
   transforms, dropping the translation. They now keep the matrix order and the
   implicit final input of 1, with PSP-measured homogeneous vectors and synthetic
   emitter checks.
3. **Where the room goes.** Not a transform bug. Dumping both VRAM buffers at
   free roam showed the complete room in one buffer and the background plus
   characters in the other. In the traced frame the 3D room is drawn first and a
   full-screen opaque 2D gradient is drawn *after* it, so the room is covered.
   The gradient quad's vertices are (0,0)-(480,272) raw, i.e. whole pixels: a
   through-mode 12.4 reading was tried and disproved by tracing the world map,
   whose background tiles are also full-screen at these coordinates (dividing by
   16 shrinks the map and the real name-dialog bars to nothing). That change was
   reverted; CTest 21/21.

Evidence: with the pixel reading, the name dialog shows the real "ENTER NAME"
bar and message box (`build/s345-name.png`). The room itself is present in VRAM
(`build/vram-b.png` from run s358) and the office is rendered when the covering
quad is not drawn (`build/s356-menuopen.png`, from the disproved experiment).
The open problem is frame composition/order between the field's 3D list and the
full-screen 2D background.

The map transition does complete on some runs: in s363 the field unloaded at
vb 3396 and reloaded at vb 4496, and s366 reached free roam again
(`build/s366-room.png`), but the destination chosen was still Chancellor's
Office. The destination list renders without its panel or highlight, and its
cursor cannot be verified from pixels, so choosing Classroom is not yet
repeatable. So the office is visibly functional, but the Classroom and the duel
are not reached.

Automation notes: with `SETO_MENU_MAP=1` the script stops after the Classroom
dialogue and captures `build/classroom-free-roam.ppm` instead of blindly
attempting Duel. `SETO_TRACE_FIELD=1` traces the next GE frame's
world/view/projection matrices, viewport, model positions, screen bounds and
pixels written per primitive, over four vblanks so an alternate-vblank render
cannot be missed, and can dump both VRAM swapchain buffers. The YES/NO decision
now follows the message window's own selection byte (`sel==2`) because the pixel
classifier was calibrated on the inflated UI.

**S9.3 — story/name menu: PARTIAL.** A minimal `sceUtilityOsk` HLE lets the
original `ENTER NAME` flow return a configured name, and a minimal mailbox HLE
unblocks the game's loader. The recompiled game reaches the **MAIN MENU**, loads
the Story/Deck/Character List modules, and renders the Group List screen. A
title-menu option for a duel has not yet been driven to a battle (see
[S9.3 results](#s93-results--story-name-menu)).

**S9.7 — office exit and moddeck_tutorial: PARTIAL.** The field Map option is
the room exit (“Exit from this spot?” YES/NO). YES advances the story and once
requested `libdeck_tutorial.prx` (LBN `0x5790`); the module is recompiled at
`0x093E0000` and its image/exports load, but its start was not yet reproduced
(see [S9.7 results](#s97-results--office-exit-and-moddeck_tutorial)).

**S9.9 — duel subsystem integrated: PARTIAL.** The six PRXs behind the first
scripted duel are discovered from the ISO (`modduel_mgr`, LBN `0x5c20`, plus
`modduel_eng`, `modduel_draw`, `moddeck_swap`, `modtutorial`,
`modduelrecord`), recompiled, and loaded into the low window
(`0x02200000`–`0x03556890`) with their imports resolved. The s292 run completes
the Classroom dialogue and reaches Duel in the original field menu, but the
duel manager has not started (see
[S9.10 results](#s910-results--classroom-and-duel-command)).

**S9.6 — field action menu: PASS (menu), PARTIAL (duel).** START opens the
field Talk/Duel/Item/PDA/Map menu at the idle state; DOWN selects Duel and
CROSS starts the pre-duel conversation. Following the exit hint and loading
`moddeck`/the duel PRXs is the next step (see
[S9.6 results](#s96-results--field-action-menu-and-duel)).

**S9.5 — readable text and moddeck: PARTIAL.** A small `sceReg` HLE plus a
`flash0:` mapping lets the real font library open the system PGF, so menus and
story dialogue now render and the opening conversation advances. `moddeck`
runs and its Deck Editor renders; no duel has been started yet (see
[S9.5 results](#s95-results--system-fonts-readable-text-and-moddeck)).

**S9.4 — modstory/modfield: PARTIAL.** The real `libstory.prx` and
`libfield.prx` images are relocated and executed. `modstory`'s `module_start`
runs, requests `libfield.prx` through ModuleMgr, and `modfield`'s
`module_start` runs in turn. The game renders the first story-mode scene (HUD,
character portrait, message window) and the D-pad moves the player; no duel has
been reached yet and the dialogue text is not drawn (see
[S9.4 results](#s94-results--modstorymodfield)).

**S7 — title screen: PASS.** The EBOOT corpus is relocated into the PSP user
RAM window, which fixes the sound engine's pointer/handle classification and
lets the loading stream retire. The GE now applies `ORIGIN`/`OFFSET` to relative
list addresses, indirect calls resolve re-exported imports to their real HLE
thunks, and `sceUmdDeactivate` no longer aliases `sceUmdCancelWaitDriveStat`. The
real title screen is reached and captured (see [S7 results](#s7-results--title-screen)).

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
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s7-ge-callback.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s7-address-space.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s8-input.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s8-wakeup-yield.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s9-mailbox.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s9-field.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s9-font.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s9-duel.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s10-vtfm.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s10-ge-trace.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s10-vhtfm.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s10-frame-dump.patch
git -C build/psprecomp apply ../../patches/psprecomp-5ca18fa-s10-thread-trace.patch

cmake -S build/psprecomp -B build/psprecomp-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/psprecomp-build -j 8

$tool  = "build/psprecomp-build/tools/allegrexrecomp/allegrexrecomp.exe"
# The EBOOT is linked at 0; the host runs it in the PSP user RAM window, where
# its static objects are pointers rather than the engine's packed handles.
python scripts\relocate_prx.py private\ULUS10136_EBOOT.elf build\s7-eboot-reloc.elf --base 0x08000000
$eboot = "build/s7-eboot-reloc.elf"
& $tool info  $eboot
& $tool cover $eboot
& $tool funcs $eboot
New-Item -ItemType Directory -Force build/tagforce
& $tool emit $eboot build/tagforce tagforce
```

`emit` is deterministic: a second run reports `0 of 82 files changed`.

### 2. Native host (S2–S5)

```powershell
cmake -S . -B build/s5-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/s5-build -j 8
# EBOOT only (S2–S4 behaviour):
.\build\s5-build\Seto-static.exe build\s7-eboot-reloc.elf private\TF1.iso
# S5: load the relocated PRX, register its corpus and bind the seven NIDs:
.\build\s5-build\Seto-static.exe build\s7-eboot-reloc.elf private\TF1.iso `
    build\s5-font-reloc.elf

# names every guest file open/raw sector read and its mapped host path
$env:PSPRECOMP_TRACE_IO = '1'
# optional: bound a bring-up run with a report after N seconds (unset = run on)
$env:SETO_BRINGUP_CAP = '30'
# optional: stop after N indirect dispatches (unset = no budget)
$env:SETO_DISPATCH_BUDGET = '50000000'
```

The host opens a native Win32 window that presents the live PSP framebuffer;
set `SETO_NO_WINDOW=1` for a headless diagnostic run. The keyboard is read from
the global async key state. **Enter is START** (PSP `0x00000008`), **Up/Down
are the D-pad** (`0x10`/`0x40`) and **Z is CROSS** (`0x4000`). Click the window
and hold Enter until the fade starts; at the title menu use Up/Down and Z.

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

## S7 results — title screen

The run reaches the real title menu. `build/s7-title.ppm` (and
`build/s7-title.png`) is the game's own Yu-Gi-Oh! GX Tag Force screen with
"PRESS START BUTTON"; the earlier S6 frame was the loading sprite. The fixes
below are ordered by when they became the first unmet condition.

### LBN seek

The game seeks to byte `165537792` in the LBN extent starting at ISO byte
`282034176`. The S6 implementation sought to byte `165537792` of the ISO
instead. `sceIoLseek` now treats all three seek origins and the returned position
relative to the LBN extent, while keeping reads within that extent. The first
bad read was actually `lbu $v0, 16($s3)` at `0x0005BE8C`; `0x00059FE0`
was merely the last traced function. `$s3` came from `0x0005A134` via `$v0`.
After the LBN fix the 19 bad accesses disappear (zero observed), but this does
not establish why the game remains in loading.

### GE callbacks

The EBOOT registers a GE callback (signal `0x00004084`, finish `0x00004100`)
and passes the returned id to `sceGeListEnQueue`. The runtime returned 0 from
`sceGeSetCallback` and never invoked either function, although the game's
finish handler is what retires its render queue. The runtime now implements
`sceGeSetCallback`/`sceGeUnsetCallback` with the PSP `PspGeCallbackData` layout
(`signal_func`, `signal_arg`, `finish_func`, `finish_arg`), stores the id on the
queue, and delivers the signal callback with the id and the registered argument
while preserving the caller's registers. The finish callback is delivered only
when the list reaches `FINISH`: `END` releases the queue without raising the
finish interrupt, and a list stopped at its stall delivers nothing until
`sceGeListUpdateStallAddr` releases it, exactly once. The new regression in
`tests/test_hle.c` covers the normal finish, a stalled list released later,
no duplicate after completion, `END` alone, the argument/id, and register
preservation. The game's finish handler ran 329 times in the instrumented run.

### Module placement and the sound stream

The title callback `0x00640034` enters the animation handlers and blocks in
`0x006469A8`, which waits for the sound stream queued at `0x00417004` to drain.
The stream is allocated, keyed on (`__sceSasSetVoice`/`__sceSasSetKeyOn` with a
real sample at `0x00617488`) and reaches its natural end; the blocker was the
queue *removal*. The cleanup path `0x005CA74` calls `0x005D72C` to classify its
argument: a value with bit `0x20000000` is a packed handle, bit `0x10000000`
selects the low 16 bits, a value below `0x08000000` is re-encoded through
`0x005D6A8`, and anything else is already a pointer. With the EBOOT linked at 0,
the stream object `0x00418F60` fell in the "encode" range, so the cleanup received
a packed handle instead of the queue node, cleared the wrong slot and left the
stream queued forever.

The EBOOT corpus is now relocated to `0x08000000` (`relocate_prx.py --base`),
the PSP user RAM window. All of its static objects move with it
(`0x00418F60` → `0x08418F60`), `0x005D72C` returns the pointer unchanged, the
queue entry is found and removed, and `0x006469A8` releases. This is an address
classification problem, not an audio one: no SAS HLE return was changed. The
rebased corpus discovers 5,042 functions and compiles without errors.

### GE relative addresses

The rebase exposed a GE bug: the game's display lists use `ORIGIN` (the address
of the command itself) and relative `JUMP`/`CALL` offsets, and the runtime added
the 24-bit operand straight to `BASE`. A list at `0x080E9E28` therefore jumped to
`0x00000178` and produced 214 million bad accesses in one run. `JUMP` and `CALL`
now add `origin`; `VADDR`/`IADDR` do too; `OFFSET` (`0x13`) stores an origin
offset instead of overwriting `BASE`. Addresses are masked to the 28-bit physical
space. The regression in `tests/test_hle.c` walks an `ORIGIN` + `OFFSET` + `BASE`
list and asserts six commands, `FINISH`, and zero bad accesses.

### Indirect imports

A re-exported import called through the EBOOT export table is an indirect
dispatch target. The emitter registered function entries only, so the call landed
on the raw `jr $ra` stub at `0x0806B1E4` and returned 0 for
`sceGeEdramGetAddr`; the renderer initialized with a zero VRAM base. The emitter
now registers every import thunk after the function entries, so it wins any
collision (and import-only addresses such as the re-exports are covered too).
`tests/test_emit.c` covers the precedence and an import-only entry.

### UMD cancellation

`sceUmdDeactivate` was bound to NID `0x6AF9B50A`, which is
`sceUmdCancelWaitDriveStat`; the real deactivate is `0xE83742BA`
(`0x6AF9B50A` is Cancel). The title loader calls cancel, so the runtime
deactivated the drive, later reads returned `-1`, and the loader tore down before
loading `title_menu/title_menu_e.ehp`. The NIDs are separated and cancel is a
no-op (this host never parks a drive wait). With the drive kept active, the
317 KB menu resource loads and the title menu renders.

### Title frame

Final run (`SETO_BRINGUP_CAP=30`): 876 vblanks, 878 GE lists, 611,100 commands,
85,924 vertices, 235,207,386 pixels, 877 finishes, five threads, zero dispatch
misses, bad accesses or traps, 5,278,704 ISO bytes. A second independent 30-second
run produced the same title frame. Without `SETO_BRINGUP_CAP` the host does not
exit on its own; the cap is a diagnostic, not part of the game. The synthetic LBN
seek regression can be run with:

```powershell
cmake --build build/s6-build --target test-lbn-seek
.\build\s6-build\test-lbn-seek.exe
```

## S8 results — title input

**Input path.** The host has no window, so a host thread samples
`GetAsyncKeyState(VK_RETURN)` every 10 ms. One physical press sets one START
request; release clears it. The title reads a one-frame press edge at
`0x08101530`, and the EBOOT input update `0x08035AC8` clears that edge on its
next run. If the title frame lands after the clear, the press is lost: a 250 ms
pulse was missed in one of five runs (the dispatcher read step 2 with buttons 0
while Peek had already seen START). The fix is to re-issue the pad edge from the
guest's own input-update entry until the title's step reaches 3; the loop then
stops. This is one logical press, not a held START. The request times out after
5 s if the title never consumes it. The runtime's pad state is atomic
(`_Atomic` buttons/sticks), and `sceCtrlPeekBufferPositive` (NID `0x3A622550`)
shares the `ReadBufferPositive` writer. The menu-ready, request, done and
scene-3 flags are `_Atomic`. The game polls Peek through the EBOOT pad wrapper
`0x08035AC8`; at the title, the game's own handler `0x00647164` read the pressed
mask and saw `0x00000008`. The regression in `tests/test_hle.c` checks no
buttons, centered sticks, START delivered by Peek, and release clearing it.
Only START is mapped.

**Scheduler yield.** In some runs the title callback stayed inside its frame
loop `0x006401E8`, calling `sceKernelWakeupThread` 48 million times on
`user_main`. The runtime only handed the CPU over when the woken thread had
higher priority, so the display thread starved: 370 vblanks in 45 s. A wakeup
now also hands over for a live same/lower-priority target, at most once per
millisecond; its timestamp is reset with the scheduler. After the change, 5/5
runs held a normal rate (1,178–1,360 vblanks at 45 s, 0–1 wakeups) instead of
the 2/5 starved runs before it, and the five final 30 s title runs had zero bad
accesses (874–1,842 vblanks).

**Transition.** On START the game runs its own fade/wipe (`0x0064641C`,
`0x00644768`): `build/s8-fade.ppm` shows the title being wiped. The valid
branch is `0x0064719C`: it sets the pending step at `0x126C6FE` to 4 and
advances the dispatcher to step 3. Step 3 (`0x0064725C`) runs `0x00644768` for
about 20 frames; when the fade completes it calls `0x006473E0`, copies the
pending step (4) into `0x126C6FC`, and step 4 (`0x00647280`) runs the scene
manager `0x00647468`. The scene state at `0x126CF6C` advances `0 → 1 → 2 → 3`
and then stops at the title menu, which waits for a further button. The host
only sends START, so this path reaches the menu and does not issue the
`LoadModule`. The earlier "fade stops with pending 4" was the step 3→4 handoff:
the pending value is consumed, and state 3 is the next screen, not a stalled
fade.

The selection is now located: the 40-entry table at `0x00666B28` is indexed by
the animation id at `0x126C6F8`; entry 5 (`0x00642FBC`) owns the menu and calls
the dispatcher `0x006470B4`. That dispatcher indexes the pointer table
`0x00666CA0` by the 16-bit step at `0x126C6FC` (step 1 → `0x00647128` →
`0x00647164`; step 2 → `0x00647164`; step 3 → `0x0064725C`; step 4 →
`0x00647280`; 12 and above → `0x006472EC`).

Without input the step cycles `0 → 1 → 2 → 0` around vblank 400; step 1 is the
PRESS START window and step 0 is idle. With a START request the title enters the
fade path above. The diagnostic sets the request at the guest's step-1 entry
(`0x00647128`); the input-update watch `0x08035AC8` re-issues the pad edge until
the title's step reaches 3. In five independent runs the request was published
at vblank 412–449, and step 3 + scene state 3 were reached at vblank 474–509,
**5/5**, with no `ModuleMgr` load. The request loop stops at step 3, so it cannot
repeat at the menu.

**Menu capture.** `build/s8-menu.ppm` is captured 2 s after scene state 3 in
each diagnostic run. It shows the title artwork with the magenta/orange
selection bar but no distinguishable menu text; the scene state 3 handler entry
is the proof of the state. No artificial image is produced.

The 250 ms blind pulse from the previous commit is gone: the EBOOT input update
clears the one-frame edge at `0x08101530`, and a slow title frame could read it
after the clear. The request loop is synchronized to the guest's own input
update, not to host time.

The idle/attract path times out at step 2 (the 990 timer), resets to step 0 and
issues

```
sceKernelLoadModule("disc0:/sce_lbn0x5c20_size0x1f6f2")
```

which is `modduel_mgr` (128,754 bytes), at vblank ≈ 2390. That is the only path
that reaches the call; it is not counted as a START transition. The module
imports `libehsys` (1,245), `libduel_eng` (143), `libduel_draw` (38),
`libdeck_swap` (3) and `libtutorial` (7), and reserves 11.6 MB of `.bss`. Only
`libehsys` resolves here; the other four libraries are a duel subsystem that
this host does not carry, so the next scene is not reached and none of them was
integrated. The host now names a `LoadModule` path it cannot serve instead of
failing silently.

**Dispatch budget.** One normal 35 s title run ended on the old fixed 5,000,000
dispatch budget; it is now opt-in (`SETO_DISPATCH_BUDGET=<calls>`), because the
title's wait loop legitimately runs for a long time.

**Integrity.** Zero dispatch misses and traps in the five diagnostic runs. Four
had zero bad accesses; one had 3 at `0x3` from `0x08034644` **before** the START
request, during the title load. That is the pre-existing `ehfile_queue` race,
not caused by START and not blocking the transition. The same race appeared
before START in earlier runs. A 6 s physical Enter test produced exactly one
`input: START press`, one release, and scene state 3 with zero bad accesses.
Upstream tests 21/21; LBN seek PASS; all fourteen psprecomp patches apply to
`5ca18fa` and the clean tree passes 21/21.

To reproduce: `SETO_BRINGUP_CAP=60` and `SETO_AUTO_START=1` (request at step 1,
capture 2 s after scene state 3), or press Enter when the prompt is shown and
hold until the fade starts. The fade reaches the title menu; the idle path
reaches the `LoadModule` at vblank ≈ 2390.

## S9.1 results — live Win32 window

**Implementation.** `host_window.c` adds one presentation thread and a native
window; the game, GE and scheduler are untouched. The thread creates a
`WS_OVERLAPPEDWINDOW` with a 960 × 544 client area using `RegisterClassExW` /
`CreateWindowExW`, runs a normal `GetMessageW` loop, and updates on a 33 ms
`SetTimer` (~30 FPS). `WM_SIZE` only invalidates; `WM_PAINT` uses
`StretchDIBits` into a 30:17 fit, centred with a black background; `WM_CLOSE` /
`WM_DESTROY` kill the timer and exit the thread. The two 480 × 272 buffers and
the `BITMAPINFO` are allocated once. `SETO_NO_WINDOW=1` skips the window.

**Framebuffer.** `psp_display_read` returns the front buffer; the runtime
documents that the game double-buffers, so reading it while the guest runs is
the intended use. The presentation thread reads one snapshot per timer tick.
The pixels are `0xAABBGGRR`; the top-down 32-bit `BI_RGB` DIB needs
`0x00RRGGBB`, so red and blue are swapped once per frame. The window never
writes guest memory or changes display state.

**Visual validation.** The window showed the real Konami loading frame, the
title artwork, `PRESS START BUTTON`, the text fading into the selection bar and
the scene-state-3 title screen. Captures are in `build/s9-shot-4.png`,
`build/s9-shot-8.png`, `build/s9-fade-00.png`, `build/s9-fade-03.png`,
`build/s9-fade-04.png` and `build/s9-shot-12.png` (ignored, not committed).
Resizing to 1200 × 500 and 400 × 400 produced letterboxed images with no
distortion (`build/s9-wide.png`, `build/s9-small.png`); colours and vertical
orientation were correct. Scene state 3 still renders the title artwork with
the magenta/orange bar, not a visually distinct menu text; that is recorded,
not changed here.

**Functional validation (five window runs, `SETO_AUTO_START=1`, 20 s cap).**

| Run | Window | START request | Scene state 3 | Bad accesses |
|---:|---|---:|---:|---:|
| 1 | 960 × 544 | vb 466 | vb 532 | 0 |
| 2 | 960 × 544 | vb 463 | vb 521 | 3 (before START) |
| 3 | 960 × 544 | vb 461 | vb 519 | 6 (before START) |
| 4 | 960 × 544 | vb 442 | vb 505 | 3 (before START) |
| 5 | 960 × 544 | vb 419 | vb 472 | 0 |

All five reached scene state 3 through the original branch. A physical Enter
hold produced one `input: START press`, one release and no repeated request.
Closing the window destroyed the presentation thread; the process continued to
the bring-up cap and reported normally.

**Performance.** With the same 20 s cap, windowed runs progressed
512–1074 vblanks and headless runs 520–778 (one headless run produced no
report); the windowed process used about 11.8 s CPU at 15 s wall versus 3.6 s
headless. The extra CPU is the 30 FPS read/convert/StretchDIBits thread, not a
guest slowdown: the window does not alter timers or the scheduler, and
windowed runs reached scene state 3 in 5/5. The vblank numbers are noisy because
the focused window changes OS scheduling; no regression was observed in the
game path.

**Manual run.**

```powershell
cd C:\Users\Vinicius\Documents\Github\Seto-static

# Build the current source (the generated corpora under build\ are reused).
cmake -S . -B build\s9-build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build\s9-build -j 8

# Run with the real EBOOT, ISO and PRXs used in S5-S8.
.\build\s9-build\Seto-static.exe `
    build\s7-eboot-reloc.elf `
    "C:\Users\Vinicius\Documents\Github\Seto\TF1.iso" `
    build\s5-font-reloc.elf `
    build\s6-title-reloc.elf

# The window opens at 960 x 544. Click it and hold Enter until the fade starts.
# Release Enter to re-arm a later press. At the title menu use Up/Down to move
# and Z (CROSS) to confirm.

# Optional: stop after 30 seconds and print the diagnostics.
$env:SETO_BRINGUP_CAP = '30'

# Optional: run without the window (headless diagnostics).
$env:SETO_NO_WINDOW = '1'

# Stop a run without a cap: Ctrl+C in this console, or from another PowerShell:
Stop-Process -Name Seto-static
```

The generated `build\s7-eboot-reloc.elf`, `build\s5-font-reloc.elf` and
`build\s6-title-reloc.elf` come from the Reproduce steps above and are ignored
by Git. If they are missing, regenerate them first.

## S9.2 results — title menu

**Handler.** The scene state 3 handler `0x00645084` reads the game pad through
`0x00664014`: `+4` is the one-frame press edge. The confirm filter
`0x006646F4` returns `0x4000` at this state, so the accepted confirm button is
**CROSS**. With no edge it checks `0x10` (**UP**) and `0x40` (**DOWN**) to move
the cursor at `0x126CFBC`; the option table is at `0x126CFA8` and the option
count at `0x126CF74` is 2. Confirmation copies the selected value
(`0x126CF71`) to `0x126CF6E` and advances the scene state. Both cursor positions
(0 and 1) reach the same next scene state 4.

**Controls and implementation.** Only UP, DOWN and CROSS are needed; left/right,
CIRCLE and START-as-confirm are not. The window keyboard maps Up/Down to
`0x10`/`0x40` and Z to `0x4000`; Enter remains START.

- `title_prx.c`: the existing input-update watch `0x08035AC8` now also reads the
  scene state (`0x126CF6C`) and cursor (`0x126CFBC`). A menu request toggles the
  requested pad edge while the state is 3, using the same one-frame edge path as
  START. START request/done handling is unchanged.
- `host.c`: the diagnostic sends UP then CROSS through the same
  `psp_title_menu_request` used by the keyboard. The real keyboard has one
  request per press, an 800 ms bound, and does not repeat while held.
- No guest state is written and the Win32 window is unchanged. All automated
  runs use `SETO_NO_WINDOW=1`.

**Validation (five independent headless runs).**

| Run | START done | Scene 3 | UP cursor | CROSS | Final state | Bad accesses |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | vb 491 | yes | 1 → 0 | accepted | 4 | 3 (before START) |
| 2 | vb 524 | yes | 1 → 0 | accepted | 4 | 0 |
| 3 | vb 518 | yes | 1 → 0 | accepted | 4 | 0 |
| 4 | vb 492 | yes | 1 → 0 | accepted | 4 | 0 |
| 5 | vb 524 | yes | 1 → 0 | accepted | 4 | 0 |

All five reached scene state 4; zero dispatch misses and zero traps. The one
bad-access run logged the known `ehfile_queue` accesses at `0x3`/`0x08034644`
before START, not during the selection. A physical keyboard test with no window
also produced START press/release, UP cursor 1 → 0, Z CROSS, state 3 → 4 and
zero bad accesses.

**Progression.** The confirmation reaches scene state 4, phase 2. The first
frame of the next scene is in `build/s92-after.ppm` (the character/arena view,
dark because the transition is still fading in). `0x126F6FC` remains 0 and no
`sceKernelLoadModule` is issued; `0x006475C0` (phase 2) is the next wait and is
left for S9.3. The title menu still has no distinguishable text, but its cursor
and confirmation are processed by the original code.

**Integrity.** Upstream 21/21; LBN seek PASS; `git diff --check` clean.

## S9.3 results — story, name and main menu

**OSK.** The title imports the system on-screen keyboard through
`0xF6269B82`/`0xF3F76017`/`0x4B85C861`/`0x3DFAEBA9`. The host now serves that
contract in `title_prx.c` using the standard `SceUtilityOskParams`/data layout:
one field, the name from `SETO_PLAYER_NAME` (fallback `YUGI`), result
`CHANGED`, and init → visible → quit → finished statuses. The title's own code
copies the returned string and runs the original confirmation dialog. No guest
state is written.

**Mailbox.** The game's loader uses `sceKernelCreateMbx`, `DeleteMbx`,
`SendMbx` and `PollMbx`; the runtime did not implement them. A small FIFO mailbox
HLE (`patches/psprecomp-5ca18fa-s9-mailbox.patch`) adds the four calls. With it,
the Group List screen progresses instead of spinning on an empty `PollMbx`.

**Flow observed.** After the title menu (cursor 0) the game runs the original
`ENTER NAME` screen. The OSK returns `YUGI`, START opens the confirmation
dialog, and CROSS confirms it. The game then reaches the **MAIN MENU**. Menu
options observed:

| Option | Loaded/entered |
|---:|---|
| 1 | `libstory.prx` (`ModuleMgr` ENOENT) |
| 2 | `libdeck.prx` (`ModuleMgr` ENOENT) |
| 3 | `libcharalist.prx` (`ModuleMgr` ENOENT) |
| 4 | Options screen |
| 5 | Group List (requires the mailbox HLE) |
| 6 | returns to the name-entry/new-game flow |

The first three modules are not integrated; their `LoadModule` requests are
recorded rather than faked. The Group List is rendered by the original code
after the mailbox fix (`build/s92-mbx-cap.png`).

**Validation.** The long synthetic path reached the name confirmation/main menu
in 4 of 5 runs; the remaining run stopped earlier in the name animation. The
mailbox/Group List run used the same sequence and reached scene state 4,
phase 11. The sequence is timing-sensitive and is not yet a 5/5 battle path.
The window is unchanged; all automated runs use `SETO_NO_WINDOW=1`.

**Manual keys for this milestone.** At `ENTER NAME`, Z opens the OSK (the name
is `SETO_PLAYER_NAME`/`YUGI`), Enter opens the confirmation dialog, and Z
confirms it. At the main menu, Up/Down/Z navigate and select. The title menu and
the rest of the keyboard path are unchanged.

**Next target.** The title menu's second option (cursor 1) has not been driven
to a duel. The main menu's first three modules are known but not integrated;
the next round should identify the shortest real duel entry and integrate only
the module(s) it requests.

## S9.4 results — modstory/modfield

**Images.** Two retail PRXs are now recompiled and executed:

| Module | ISO LBN | Size | Relocated base | `.bss` | Imports |
|---|---:|---:|---:|---:|---:|
| `modstory` (`libstory.prx`) | `0x5e20` | `0x24c42` | `0x08910000` | 2.8 MB | 137 (libehsys + 3 libfield) |
| `modfield` (`libfield.prx`) | `0x5c60` | `0x4a21a` | `0x08C00000` | 8.2 MB | 258 (libehsys) |

Both are placed with `psp_mem_write_block` in the main-RAM window; the runtime
heap base moves from `0x08C00000` to `0x09600000` so the 8.2 MB field image has
room. `modstory`'s three `libfield` imports (`0x86220A06`, `0x79E5F8B7`,
`0xE92DC2F2`) are resolved from `modfield`'s own export table, so the story
module calls the real field functions instead of failing.

Reproduction (the game requests both extents by LBN, so slice them from the ISO):

```powershell
# the game requests both extents by LBN, so slice them out of the ISO
$iso = 'C:\Users\Vinicius\Documents\Github\Seto\TF1.iso'
$fs = [IO.File]::OpenRead($iso)
foreach ($e in @(@(0x5e20, 0x24c42, 'build\libstory.prx'),
                 @(0x5c60, 0x4a21a, 'build\libfield.prx'))) {
    $fs.Position = [int64]$e[0] * 2048
    $buf = New-Object byte[] $e[1]
    [void]$fs.Read($buf, 0, $e[1])
    [IO.File]::WriteAllBytes($e[2], $buf)
}
$fs.Close()

python scripts\relocate_prx.py build\libstory.prx build\s93-story-reloc.elf --base 0x08910000
python scripts\relocate_prx.py build\libfield.prx build\s93-field-reloc.elf --base 0x08C00000
& $tool emit build\s93-story-reloc.elf build\s93-story-reloc story
& $tool emit build\s93-field-reloc.elf build\s93-field-reloc field
```

**Runtime gaps fixed.** The field module hits `vcmov` in an EBOOT math helper;
`psp_vcmov` and the `A_VCMOV` emitter case now exist, and the corpus is
regenerated (0 VFPU traps in the story run). `sceDisplayGetVcount` and
`sceDisplayGetAccumulatedHcount` were missing; they now report the runtime's
real vblank counter (the accumulated hcount is 0 because there is no scanout).
Both changes are in `patches/psprecomp-5ca18fa-s9-field.patch`.

**Observed flow.** Selecting Story at the main menu loads `modstory`; its
`module_start` runs, requests `libfield.prx` through ModuleMgr, and
`modfield`'s `module_start` runs. The game then renders the first story-mode
scene: the HUD (`1st May`, `7:30 AM`, `0 DP`, `92 Days Until Tournament`), a
character portrait and the message window. The D-pad moves the player (free
roam) and START fades the scene. `build/s93-story15.ppm` / `s95-story15.png`
are the captured frame.

**Validation.** A clean headless run reaches `story: entered`, `field: entered`
and the scene above with **0 bad accesses, 0 traps**, 2,403 vblanks and 8,605
unimplemented calls (Atrac plus `sceReg`). The story message files
(`ImpChrMsgIe.bin`, `script/st01_t01_*`) are opened and read from the ISO.

**Gap.** The message window is drawn but its text is not: no glyph quads reach
the GE for the dialogue, while the HUD text renders. No duel has been reached;
the story starts in free roam and the dialogue cannot be read.

## S9.5 results — system fonts, readable text and moddeck

**Root cause of the missing text.** `sceFontOpen` always returned handle 0 and
`sceFontGetCharInfo`/`GetCharGlyphImage` failed with `0x80460003`. The font
library asks the registry for its font list (`/DATA/FONT`: `path_name`,
`num_fonts`, `/DATA/FONT/PROPERTY/INFO0...`), then opens
`flash0:/font/ltn0.pgf`. `sceReg` was unimplemented and the host had no flash0,
so no font was ever loaded. The HUD draws with the game's own bitmap font,
which is why it worked while every `sceFont` string was blank.

**Fix.** `sceReg` now serves the `/DATA/FONT` and `/SYSPROFILE/RESOLUTION`
subtrees with the PSP-3000 values (Latin regular font = `ltn0.pgf`), and the
file HLE maps `flash0:/...` under `PSPRECOMP_FLASH0`. The font assets come from
a local PSP flash0 dump (the PPSSPP asset tree also carries them); they are
Sony firmware data and are **not** versioned here. With
`PSPRECOMP_FLASH0=<flash0 root>` the real font library opens the PGF, and
`GetCharInfo` returns real metrics.

**Result.** Text now renders everywhere: the title/menu labels, the main menu
(`Story Mode`, `Free Duel`, `Deck Menu`, ...), the story location
(`Chancellor's Office`), speaker names and dialogue. The opening conversation
advances with CROSS (`We aim to create the next generation of duelists.`,
`Study hard to become the next King of Games.`, `Leave here and get to the
Classroom at once.`). A clean run reaches the story scene with **0 bad
accesses** (`build/s124-story20.ppm`).

**moddeck.** Deck Menu requests `libdeck.prx` (LBN `0x56f0`). The 13.8 MB
`.bss` image is relocated to `0x08910000` (this mode does not load
story/field) and its `module_start` runs; the real Deck Editor renders
(`build/s105-duel15.png`).

**Free Duel is locked in a fresh game.** In the main menu "Free Duel" is drawn
with grey text (RGB 128,128,128) while Story Mode and Deck Menu are white
(255,255,255) in the same frame, and the menu cursor skips over it
(`build/s93-free-menu.ppm`). The main-menu navigation was not "missing" the
entry: the entry is disabled without a save/story progress, so the deck path
was reached through the neighbouring Deck Menu. The story is therefore the only
route to a first duel.

**Gap.** No duel has been started. The story conversation is long and the
player has not yet left the Chancellor's Office; Free Duel selection needs a
reliable single-step menu input.

## S9.6 results — field action menu and Duel

**The field action menu opens with START.** At the field idle state (after the
office conversation, no dialogue box up), pressing START (`0x00000008`) draws
the NPC action menu:

```text
Talk
Duel
Item
PDA
Map
```

Earlier probes missed it because they pressed START during a dialogue or a
fade (which triggers a scene wipe instead). With the menu up, one D-pad DOWN
moves the highlight from Talk to **Duel** (`build/s181-pause2.ppm`), and CROSS
starts the pre-duel conversation. Sheppard then gives the route hint: “The exit
is at the bottom right hand of this room.” (`build/s181-pduel25.ppm`).

**Deck request observed.** Navigating the menu also made the story request
`libdeck.prx` (LBN `0x56f0`). In story mode the host does not load moddeck:
story (`0x08910000`), field (`0x08C00000`) and the 13.8 MB deck `.bss` do not
fit the current RAM windows together, so the request fails with ENOENT. This is
the next memory-layout blocker, not a missing game call.

## S9.7 results — office exit and moddeck_tutorial

**The Map option is the room exit.** With the field action menu open, choosing
**Map** opens the dialog “Exit from this spot?” with **YES/NO**. Confirming YES
continues the story (Sheppard: “He'll take over from there.”), and in one run
the game then requested `libdeck_tutorial.prx` repeatedly:

```text
module: load 'disc0:/sce_lbn0x5790_size0x292fa'   (moddeck_tutorial)
```

**moddeck_tutorial integrated.** The 168,698-byte PRX is relocated to
`0x093E0000` (memsz `0x20910`, only 8,256 bytes of `.bss`), which fits the gap
between modfield and the heap. The corpus resolves its 1,245 libehsys imports
from the EBOOT and loads the image:

```text
decktut PRX 0x093E0000..0x09400910 (8256 bytes .bss), gp 0x094068B0, 1245 EBOOT exports
```

The `module_start` has not been observed yet: the tutorial request did not
reproduce in the following runs (the YES dialogue timing varies), so the loader
is verified at image/export level only.

**Next target.** Reproduce the tutorial request and start, then follow the
story into the classroom and capture the duel PRX requests.

**Module lifecycle.** Tracing ModuleMgr shows the game stops and unloads
`libtitle` (UID `0x00070002`) immediately before it loads `libstory` and
`libfield`. The title is mapped in the low/extra window, so after the story
starts that window is free: the 13.8 MB `moddeck` image fits at `0x01400000`
without touching the story/field RAM windows or the heap.

`moddeck` now loads lazily on the ModuleMgr request (`psp_deck_prepare` +
`psp_mem_map_extra`). A run in which the story requested the deck produced:

```text
module: load 'disc0:/sce_lbn0x56f0_size0x2ae0a'
deck PRX 0x01400000..0x021CEE10 (14349504 bytes .bss), gp 0x01427930, 1245 EBOOT exports
deck: entered recompiled module_start 0x01407874
```

**Field buttons observed.** With a clean free-roam frame (no message window):

| Button | Effect |
|---|---|
| CROSS | Talks to a nearby character; away from one shows “There's no one to talk to!” |
| TRIANGLE | Opens the Top Menu (Deck Menu / Status / Email / Schedule / Options / End Game) |
| SQUARE / SELECT | Opens the “Explanation” help window |
| START / CIRCLE / R / L | No observed effect in free roam |

The Talk/Duel/Item/PDA/Map command menu has not been reproduced by any single
button press in the current free-roam state; the one capture that showed it
(`s200-menu2`) came after the talk + walk prelude. This is the immediate blocker
for the room exit.

**Pad pipeline (static analysis).** The EBOOT updates the pad in
`psp_func_08035AC8` (calls `sceCtrlPeekBufferPositive`, 0x3A622550) and keeps
the processed snapshot at `0x08101534` (getter `0x08035CFC`, libehsys export
NID `0xF5E3080C`). `modstory` imports 95 libehsys exports and none of them is
that getter; the field pad handler `0x08C0D4E8` receives the state pointer in
`a0` and writes only TRIANGLE -> state 23 and SQUARE -> state 5. CROSS and
START never reach either handler, so the field command menu is owned by the
EBOOT/story UI code that consumes the snapshot from `0x08101534`.

**START in the actual free-roam state.** The host now waits for field state 1
(free roam, after a script state 5/6/23 occurred) and presses START there.
Result: `free-start menu=0 state=1` — START does nothing even in the real
free-roam state with Sheppard's empty target plate up. A held CROSS in the same
state moves the field to state 6 (dialogue) and a tap to state 23 (script), so
CROSS is the conversation button, not the command-menu button. The Talk/Duel/
Item/PDA/Map menu therefore does not come from a raw pad bit handled by the
field or by the four observed EBOOT pad consumers.

**The map list is field-side.** `modfield`'s constructor
(`psp_func_08C00038`) calls the EBOOT list API — wrapper `0x74E51EA4`
(`0x0805504C`) and callback `0xF1BC43DB` (`0x08055044`) — with the text blob at
`0x08C35E74` (`bg_XX [ROOM]` lines for the 22 travel rooms) and stores the
returned list handle in its globals. That list is the travel/map destination
list behind the Map option; it is not the 5-option Talk/Duel/Item/PDA/Map menu.
The field module imports that helper only once, at init, so the command menu
itself is still EBOOT/story UI.

**Live tracing.** `SETO_TRACE_FIELD=1` logs the field state word
(`0x093CDAFC`) on every change and every `story -> field` command dispatch
(export `0x86220A06`). A headless story run shows the field cycling
`1 -> 5 -> 6 -> 23` during the opening script while the story issues commands
13, 14, 15, 16, 17 and 19; state 1 is free roam. No command-menu state is
produced by the field module itself, so the Talk/Duel/Item/PDA/Map UI belongs
to `modstory`, not `modfield`.

**Where the field logic lives (static analysis).** The field state machine is
`psp_func_08C0C814`; its 24-entry jump table is at `0x08C34890` and the state
word is `[0x093CDAFC]` (setter `0x08C0D464` writes state 21 with two words from
its argument). The story→field command API is the export `0x86220A06`
(`psp_func_08C0C008`): it indexes a 20-entry table at `0x08C38640`
(12-byte entries: start, query, busy) and calls `entry.start(arg)`.
`psp_func_08C0C088` is the matching query/“command done” entry. The pad handler
`0x08C0D4E8` only consumes R (camera), TRIANGLE (state 23, Top Menu) and
SQUARE (state 5); CROSS/START are handled by the field/story UI elsewhere,
which is why no single raw pad bit opens the command menu.

**Next target.** Reproduce the field command menu, choose Map → “Exit from this
spot?” YES, and follow the story into the classroom to the first scripted duel.

## S9.8 results — field command menu and world map

**The command menu is now opened deterministically.** In field state 1 (free
roam, after a script state 5/6/23), the host holds the analog stick down-right
(`(255,255)`) for ~2.8 s, stops, and presses CROSS. The original game draws the
Talk/Duel/Item/PDA/Map menu (`build/s93-menu-open.ppm`, reproduced in every run
since the discovery). Four D-pad DOWNs move the highlight to **Map**
(`build/s93-map-sel.ppm`); CROSS opens “Exit from this spot?” with YES/NO
(`build/s93-map.ppm`); CROSS confirms YES and the game plays the story
transition into the **Duel Academy world map**
(`build/s93-exit-yes.ppm`).

The earlier failures are explained: START does not open the menu, and the
trigger is the analog down-right walk + CROSS in real free roam, not a raw pad
bit handled by `modfield`/the pad consumers.

**Destination list opens.** On the world map, four D-pad moves (right, up,
left, down) followed by CROSS open the travel destination list: Main Gate
(highlighted), Classroom, Store, Duel Field, Chancellor's Office
(`build/s93-map-cross.ppm`). The host now sends DOWN (Classroom) and CROSS to
travel. The emulator slowed to ~2 vblanks/s after hours of runs, so the
confirmation itself has not yet completed inside the bring-up cap; the list
itself is reproduced.

**Next target.** Complete the Classroom travel (DOWN + CROSS) and follow the
story to the first scripted duel, capturing the ModuleMgr requests for the duel
PRXs. `moddeck` (LBN `0x56f0`) and `moddeck_tutorial` (LBN `0x5790`) are already
loadable.

## S9.9 results — duel subsystem integrated

**The duel PRX set.** The EBOOT's module table (ISO offset `0x13100`) names the
`lib*.prx` aliases; scanning the ISO for `SceModuleInfo` matched them to the
real files. The first scripted duel's manager is `modduel_mgr` (LBN `0x5c20`,
`0x1f6f2` bytes). Its import table names the dependencies and their function
counts: libehsys 1,245; libduel_eng 143; libduel_draw 38; libdeck_swap 3;
libtutorial 7. Matching the ELF headers gives:

| module | LBN | file bytes | memsz | imports |
| --- | --- | --- | --- | --- |
| `modduel_mgr` | `0x5c20` | `0x1f6f2` | `0xB18610` | libehsys, libduel_eng, libduel_draw, libdeck_swap, libtutorial |
| `modduel_eng` | `0x5940` | `0x16cc7a` | `0x116210` | libehsys |
| `modduel_draw` | `0x5800` | `0x9d22a` | `0x457A20` | libduel_eng, libehsys |
| `moddeck_swap` | `0x5750` | `0x1d212` | `0x17290` | libehsys, libduel_draw |
| `modtutorial` | `0x5ef0` | `0xfdba` | `0x13B10` | libehsys, libduel_draw, libduel_eng |
| `modduelrecord` | `0x57f0` | `0x7da2` | `0x6890` | libehsys |

None of their export NIDs collide with libehsys, so the import forwarder can
resolve by NID alone.

Reproduction (slice by LBN, then relocate and emit each corpus):

```powershell
$iso = 'C:\Users\Vinicius\Documents\Github\Seto\TF1.iso'
$fs = [IO.File]::OpenRead($iso)
foreach ($e in @(@(0x5c20, 0x1f6f2, 'build\libduel_mgr.prx'),
                 @(0x5940, 0x16cc7a, 'build\libduel_eng.prx'),
                 @(0x5800, 0x9d22a, 'build\libduel_draw.prx'),
                 @(0x5750, 0x1d212, 'build\libdeck_swap.prx'),
                 @(0x5ef0, 0xfdba, 'build\libtutorial.prx'),
                 @(0x57f0, 0x7da2, 'build\libduelrecord.prx'))) {
    $fs.Position = [int64]$e[0] * 2048
    $buf = New-Object byte[] $e[1]
    [void]$fs.Read($buf, 0, $e[1])
    [IO.File]::WriteAllBytes($e[2], $buf)
}
$fs.Close()
python scripts\relocate_prx.py build\libduel_mgr.prx   build\s9duel-mgr-reloc.elf  --base 0x02200000
python scripts\relocate_prx.py build\libduel_eng.prx   build\s9duel-eng-reloc.elf  --base 0x02E00000
python scripts\relocate_prx.py build\libduel_draw.prx  build\s9duel-draw-reloc.elf --base 0x03000000
python scripts\relocate_prx.py build\libdeck_swap.prx  build\s9duel-swap-reloc.elf --base 0x03500000
python scripts\relocate_prx.py build\libtutorial.prx   build\s9duel-tut-reloc.elf  --base 0x03530000
python scripts\relocate_prx.py build\libduelrecord.prx build\s9duel-rec-reloc.elf  --base 0x03550000
& $tool emit build\s9duel-mgr-reloc.elf  build\s9duel-mgr-reloc  duelmgr
& $tool emit build\s9duel-eng-reloc.elf  build\s9duel-eng-reloc  dueleng
& $tool emit build\s9duel-draw-reloc.elf build\s9duel-draw-reloc dueldraw
& $tool emit build\s9duel-swap-reloc.elf build\s9duel-swap-reloc duelswap
& $tool emit build\s9duel-tut-reloc.elf  build\s9duel-tut-reloc  dueltut
& $tool emit build\s9duel-rec-reloc.elf  build\s9duel-rec-reloc  duelrec
```

**Integration.** Six corpora are emitted from the relocated images and loaded
at `0x02200000`, `0x02E00000`, `0x03000000`, `0x03500000`, `0x03530000` and
`0x03550000` — above moddeck's window (`0x01400000`–`0x021CEE10`), so a deck
request cannot overwrite the duel manager. `duel_prx.c` loads them, builds one
export table per module (each entry carries that module's `$gp`), forwards
imports at call time, and answers ModuleMgr Load/Start for the six LBN paths
and their `lib*.prx` basenames.

**Runtime mapping.** The pinned psprecomp had a single "extra" low-window
mapping, already used by libtitle and moddeck, so a second PRX set could not be
placed. `patches/psprecomp-5ca18fa-s9-duel.patch` turns it into a 12-slot table
and matches each range in order. The patch applies on top of the previous
fourteen (`git apply --check` exits 0).

**Evidence (headless).** Before the guest runs, the host loads all six images:

```text
duel PRX modduel_mgr 0x02200000..0x02D18610 (11536752 bytes .bss), gp 0x0221FC80, 0 exports, entry 0x02200000
duel PRX modduel_eng 0x02E00000..0x02F16210 (26656 bytes .bss), gp 0x02F179D0, 143 exports, entry 0x02E00000
duel PRX modduel_draw 0x03000000..0x03457A20 (4057616 bytes .bss), gp 0x030807E0, 38 exports, entry 0x03001BF4
duel PRX moddeck_swap 0x03500000..0x03517290 (3728 bytes .bss), gp 0x0351E3E0, 3 exports, entry 0x035117E0
duel PRX modtutorial 0x03530000..0x03543B10 (31568 bytes .bss), gp 0x03543FA0, 7 exports, entry 0x035302CC
duel PRX modduelrecord 0x03550000..0x03556890 (192 bytes .bss), gp 0x0355E7B0, 3 exports, entry 0x03550000
```

`modduel_mgr` reports zero function exports because it exports no callable
functions; no other module imports it, so that is expected.

**Travel and input.** The host now keeps the scheduler turning until its
scripted input finishes (plus a 5,000-yield grace slice) instead of stopping
5,000 yields after `module_start`; that fixed the runs that ended in the middle
of the Classroom conversation. The exact travel nondeterminism is understood:
the destination list opens only after the map CROSS's fade ends. When that
takes longer than 2.5 s, the old DOWN landed on the map and the list later
opened on Main Gate, where every following CROSS was ignored. The host now
waits for the list and for the orange highlight to reach Classroom before it
confirms. Title name input now waits for the visible prompt panel before
alternating the original CROSS/START controls; the s292 log showed
`state=4 phase=4` with the prompt visible, then `state 4->1->2->3` and
`story=1`.

The Classroom conversation itself is reproduced (`build/s282-class25.ppm` …
`s282-class300.ppm`: Crowler “I'm by far the top teacher in the entire
Academy.”, “Yeah! I beat you here!”, “No, not that!”). The live Win32 window run
(`s281`) showed the command menu, Map highlight, exit dialog, world map and
destination list frame-by-frame.

## S9.10 results — Classroom and Duel command

**Headless evidence (s292).** The original story reached the Classroom, rendered
Crowler's “There's a new student coming today” dialogue, and returned to free
roam (`classroom done=1 state=1`). The existing down-right walk + CROSS opened
the original Talk/Duel/Item/PDA menu; DOWN highlighted Duel, and CROSS drove the
field through states `5 -> 6 -> 23 -> 1`. Captures:
`build/s93-travel10.ppm`, `build/s93-class-duel-menu.ppm`,
`build/s93-class-duel-select.ppm`, `build/s93-first-duel.ppm`.

**s308 update — duel modules start, battle not yet proved.** The original
ModuleMgr path loaded and started `modduel_mgr` (entry `0x02200000`),
`modduel_draw` (`0x03001BF4`) and `modduel_eng` (`0x02E00000`); all three
`module_start` calls returned `0`. The final frame shows “Challenge me? Fine,
I’ll take you on.” The run hit its 180 s cap with field state still `0`, no
duel board, and no processed result. It also logged three bad accesses at
`0x00000003` (first from `0x08034644`) and repeated `ehfile_queue` waits; their
causal relation to battle progression is not established.

The s308 input trace reported `auto START done=0 scene3=0`. Investigation found
that `psp_trace_watch` holds only one active watch: adding the `0x0803D968`
diagnostic replaced the `0x08035AC8` input-update hook. The current fix keeps
both observations in the existing input hook. In s309 the original title input
then reported `START done=1 scene3=1` and advanced state `3 -> 4`; the run had
zero bad accesses and reached the visible name confirmation.

In s312 the screenshot classifier identified the pink highlight on **NO**,
sent one original D-pad DOWN press, and still identified NO; it sent no CROSS
and paused (`build/s312-confirm-dialog.png`, `build/s312-after-down.png`). Runs
s313–s316 did not reach phase `6`. The final s315 frame visibly showed
`ENTER NAME`; its red prompt text was present, but the generic dialogue-box
brightness gate rejected it during the transition. Removing that gate let
s316 detect the returned prompt, but too early at phase `1`; START then reached
phase `4` while the final frame still showed `ENTER NAME`. Requiring phase `4`
before pressing START fixed the timing: s317 reached the original “Is this OK?”
dialog at state `4`/phase `6`, with **NO** highlighted. An original analog-LEFT
input for 600 ms left NO selected, so no CROSS was sent. The run had zero bad
accesses. Repeated `ehfile_queue` waits remain observed, but their causal
relation to progression is not established. No battle board or processed
result has yet been verified.

## S9.12 results — original name confirmation passed

**Handler.** The name-confirmation dialog is the EBOOT message window
`0x080476CC` (libehsys export NID `0x39ADDA9F`), entered from the title poll
`0x0064C0D8`. It reads the **digital D-pad** from `0x08101540` (`0x80`
left, `0x20` right, `0x10` up, `0x40` down); the one-frame confirm edge is
`0x08101538` filtered by `0x0803D8F8` (NID `0x8171F765`, the same filter the
title menu uses). The earlier analog-stick attempt never set those bits.

**Fix.** `host.c` sends one digital LEFT when the dialog opens. The window
moves the highlight from **NO** to **YES**; YES is drawn blue (`112,182,255`)
and NO pink (`255,98,173`), so the classifier now accepts the blue highlight
and sends CROSS only when YES is visibly selected. No guest memory is written;
input still goes through `psp_ctrl_set` and the EBOOT input update.

**Evidence (s332).** Window state `3 -> 4 -> 5 -> 6` with poll return `2`;
title phase `6 -> 9`; scene `4 -> 1 -> 2 -> 3`; Story Mode then loads
`modstory` and `modfield` and runs the opening script. The command menu opens
(`build/s93-menu-open.ppm`: Chancellor's Office, Talk/Duel/Item/PDA/Map), the
world map travels to the Classroom, and the classroom script runs until the
`SETO_BRINGUP_CAP=150` cap. CTest 21/21.

## Limitations

- `sceReg` serves only the `/DATA/FONT` and `/SYSPROFILE/RESOLUTION` subtrees
  the font library reads; other categories return not-found and the library
  uses defaults. Fonts need a flash0 tree via `PSPRECOMP_FLASH0`; without it the
  game runs but `sceFont` strings are blank.
- `libtitle.prx`, `libstory.prx`, `libfield.prx`, `libdeck.prx`,
  `libdeck_tutorial.prx` and the six duel PRXs are integrated; `libcharalist`
  and the recipe viewers are not. The duel images load, but no duel has started
  yet.
- The rebased corpus relocates the EBOOT into the PSP user RAM window. The host
  refuses the unrelocated image instead of guessing its base.
- Audio is paced by the runtime but not played; the Atrac stream calls remain
  unimplemented. START, the D-pad, CROSS and CIRCLE are mapped.
- `vcmov` is implemented; 270 other VFPU instructions remain traps in the
  rebased EBOOT corpus (0.25%), unreached by the story run. The title corpus has
  22 VFPU traps, also unreached.
- 2,889 functions without `jr $ra` and 176 `scattered` functions remain discovery
  caveats; they did not affect the EBOOT run.
- No window; the goal is the real code path, not play.

## Repo contents

Tracked: this README, `.gitignore`, `host.c`, `umd.c`, `fpl.c`, `font_prx.c`,
`title_prx.c`, `story_prx.c`, `field_prx.c`, `deck_prx.c`, `decktut_prx.c`,
`duel_prx.c`, `scripts/relocate_prx.py`, `CMakeLists.txt`, `tests/lbn_seek.c`,
and fifteen small patches against the pinned psprecomp. `build/` (tool clone, generated C,
relocated ELF, objects, executable) and `private/` (decrypted EBOOT, ISO) are
ignored.
