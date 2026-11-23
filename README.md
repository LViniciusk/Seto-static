# Seto-static

Native Windows & Linux static recompilation of **Yu-Gi-Oh! GX Tag Force** (PSP, `ULUS-10136`), upgraded with the **Yu-Gi-Oh! GX Tag Force 3** (`ULES-01183`) duel engine and expanded card pool.

> **Legal Notice:** This repository contains no copyrighted game assets, binaries, or BIOS/firmware files. You must provide your own legally dumped `TF1.iso` (`ULUS-10136`) and `TF3.iso` (`ULES-01183`) images to build and play.

---

## Features

- **Pure GX Engine Upgrade:** Runs the Tag Force 3 duel engine (`rel_duel_eng.prx`) bridged cleanly into Tag Force 1's story, field, shop, deck editor, card album, and trunk systems—expanding the playable card pool with TF3 cards and boosters.
- **Tutorial Compatibility:** Automatically routes scripted duel tutorials to TF1's original engine so tutorial sequences stay 100% faithful.
- **Online AdHoc Multiplayer:** Synchronizes AdHoc rooms and duel packets through a relay bridge so online duels run natively with the TF3 engine.
- **OpenGL 3.3 GPU Backend:** High-resolution internal rendering (1x–8x), MSAA (up to 8x), separable Lanczos2 upscaling, asynchronous GE command queue, and automatic discrete GPU selection (NVIDIA Optimus / AMD PowerXpress).
- **Audio & FMV Playback:** HLE audio/AT3+ and video playback powered by FFmpeg (`libavcodec`, `libavformat`, `libswresample`, `libswscale`, `libavutil`).

---

## Requirements

### Game ISOs & Python (All Platforms)

1. **Game ISOs (user-provided):**
   - `TF1.iso` — *Yu-Gi-Oh! GX Tag Force* (`ULUS-10136`)
   - `TF3.iso` — *Yu-Gi-Oh! GX Tag Force 3* (`ULES-01183`) *(only needed during setup/build)*
2. **Python 3.10+** with **Pillow** installed:
   ```bash
   pip install pillow
   ```
3. **Git** and **CMake 3.16+** in `PATH`.

### Windows Toolchain (MSYS2 UCRT64 in `C:\msys64\ucrt64\bin`)

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-ccache
```
*(Installing `ninja`, `lld`, and `ccache` is optional but strongly recommended—it cuts a clean build from ~25 minutes down to under 2 minutes.)*

### Linux Toolchain (Ubuntu / Debian)

```bash
sudo apt install git cmake ninja-build gcc g++ pkg-config python3-pil \
  libsdl2-dev libpng-dev libgl-dev \
  libavcodec-dev libavformat-dev libswresample-dev libswscale-dev libavutil-dev \
  libssl-dev zlib1g-dev ccache lld
```

---

## Quick Start

### Option A — Windows GUI Launcher (Recommended on Windows)

1. Double-click `launcher.cmd` (or run `.\launcher.ps1`).
2. Switch to **Setup / Install**, select your `TF1.iso` and `TF3.iso`, and click **Install / Build**.
   - The setup script automatically fetches open-source `flash0` fonts from PPSSPP, clones & patches `psprecomp` and `pspdecrypt`, extracts/relocates/emits all game modules, builds the Pure GX card/shop/table data, and compiles `build/out/Seto-static.exe`.
3. Once finished, click **Play** (only `TF1.iso` is needed at runtime).

### Option B — Command Line (Windows & Linux)

**Windows (PowerShell):**
```powershell
python -u tools/setup_build.py --tf1-iso "path\to\TF1.iso" --tf3-iso "path\to\TF3.iso"
.\build\out\Seto-static.exe build\reloc\elf\eboot-reloc.elf "path\to\TF1.iso"
```

**Linux (Bash):**
```bash
python3 -u tools/setup_build.py --tf1-iso /path/to/TF1.iso --tf3-iso /path/to/TF3.iso
SETO_PRESENT=gl SETO_GE_GPU=draw ./build/out/Seto-static build/reloc/elf/eboot-reloc.elf /path/to/TF1.iso
```

Pass `--clean` to force regeneration of all dependencies and emitted C files.

---

## HD Texture Packs

- **HD Texture Pack (FULL):** [Download FULL](https://nanika.lvinik.app/share/spoS4O3#pHpDpvw4O9TgnqkwLd4zChx0uPJ_OVulYjMi6Q_SGDQ)
- **HD Texture Pack (LIGHT):** [Download LIGHT](https://nanika.lvinik.app/share/vCsR4sH#8SiP6bdtWXJZ7oMCo_MAKTAsJSYrVwg0DV0A5MLZ5L8)

Extract the pack and select the extracted folder in the launcher's **HD Textures** field (optional).

---

## Controls

| Key | PSP Input |
|---|---|
| `Arrow Keys` | D-Pad |
| `W` / `A` / `S` / `D` | Analog Stick |
| `Z` / `X` / `C` / `V` | Cross / Circle / Triangle / Square |
| `Q` / `E` | L / R Trigger |
| `Enter` | Start |
| `Backspace` / `Space` | Select |
| `F5` | Toggle FPS / Performance HUD |

---

## Environment Variables

| Variable | Effect |
|---|---|
| `SETO_PERF_LOG=1` | Write 1-second profiler summaries (`build/out/perf.log`) |
| `SETO_NO_WINDOW=1` | Run headless without opening a GUI window |
| `SETO_HD_TEX_DIR=<dir>` | Load a PPSSPP-style HD texture pack (`textures.ini`) |

---

## Repository Layout

- `src/` — Cross-platform Windows/Linux host (Win32 / SDL2), OpenGL 3.3 GPU backend, GE queue, HLE shims, AdHoc networking, and TF1↔TF3 duel bridge.
- `cmake/` — CMake overlays for Pure GX card fronts, extended savedata/trunk, and TF3 booster shop data.
- `tools/` — Automated setup orchestrator (`setup_build.py`) and card/shop/table extractors.
- `scripts/` — PSP PRX relocation utility (`relocate_prx.py`).
- `patches/` — Patches applied cleanly onto upstream `psprecomp` during setup.
- `tests/` — Unit and queue tests (`test-ge-queue`, `test-lbn-seek`, `test-seto-net`, `bench-render`).
- `docs/` — Architecture and texture-porting notes.

---

## Credits

- [psprecomp](https://github.com/sp00nznet/psprecomp) — Static PSP MIPS-to-C recompiler and HLE runtime.
- [pspdecrypt](https://github.com/John-K/pspdecrypt) — PSP `~PSP` module decryption tool.
- [PPSSPP](https://github.com/hrydgard/ppsspp) — Open-source `flash0` PGF system fonts.
