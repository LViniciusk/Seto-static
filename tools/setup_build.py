#!/usr/bin/env python3
"""End-to-end Seto-static (Pure GX) setup and build orchestrator.

Given TF1.iso (ULUS-10136) and TF3.iso (ULES-01183), fetches open-source
flash0 fonts, clones/patches psprecomp and pspdecrypt, builds the static
recompiler, extracts/decrypts/relocates/emits all EBOOT & PRX corpora,
builds Pure GX data + Phase C patches, and compiles build/out/Seto-static.exe.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import gzip
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

PPSSPP_COMMIT = "5ce7f1cfeae1bfcb1a5e88fef74ce127c696cdfc"
FLASH0_FONT_BASE_URL = f"https://raw.githubusercontent.com/hrydgard/ppsspp/{PPSSPP_COMMIT}/assets/flash0/font"
FLASH0_FONT_FILES = (
    "jpn0.pgf", "kr0.pgf",
    "ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf",
    "ltn4.pgf", "ltn5.pgf", "ltn6.pgf", "ltn7.pgf",
    "ltn8.pgf", "ltn9.pgf", "ltn10.pgf", "ltn11.pgf",
    "ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
)

PSPRECOMP_REPO = "https://github.com/sp00nznet/psprecomp"
PSPRECOMP_COMMIT = "5ca18fac1691a4a7b40ae683dabdf72a5703d2b8"

PSPDECRYPT_REPO = "https://github.com/John-K/pspdecrypt"
PSPDECRYPT_COMMIT = "c156627db7634d395c380c0a9589130f603307fc"

# (iso_path_in_gmodule, reloc_elf_name, base_addr, emit_subdir, namespace)
TF1_GMODULES = [
    ("PSP_GAME/USRDIR/gmodule/libtitle.prx",         "title-reloc.elf",         0x00640000, "title",         "title"),
    ("PSP_GAME/USRDIR/gmodule/libstory.prx",         "story-reloc.elf",         0x08910000, "story",         "story"),
    ("PSP_GAME/USRDIR/gmodule/libfield.prx",         "field-reloc.elf",         0x08C00000, "field",         "field"),
    ("PSP_GAME/USRDIR/gmodule/libdeck.prx",          "deck-reloc.elf",          0x01400000, "deck",          "deck"),
    ("PSP_GAME/USRDIR/gmodule/libdeck_tutorial.prx", "decktut-reloc.elf",       0x093E0000, "decktut",       "decktut"),
    ("PSP_GAME/USRDIR/gmodule/libduel_mgr.prx",      "duel-mgr-reloc.elf",      0x02200000, "duel-mgr",      "duelmgr"),
    ("PSP_GAME/USRDIR/gmodule/libduel_eng.prx",      "duel-eng-reloc.elf",      0x02E00000, "duel-eng",      "dueleng"),
    ("PSP_GAME/USRDIR/gmodule/libduel_draw.prx",     "duel-draw-reloc.elf",     0x03000000, "duel-draw",     "dueldraw"),
    ("PSP_GAME/USRDIR/gmodule/libdeck_swap.prx",     "duel-swap-reloc.elf",     0x03500000, "duel-swap",     "duelswap"),
    ("PSP_GAME/USRDIR/gmodule/libtutorial.prx",      "duel-tut-reloc.elf",      0x03530000, "duel-tut",      "dueltut"),
    ("PSP_GAME/USRDIR/gmodule/libduelrecord.prx",    "duel-rec-reloc.elf",      0x03550000, "duel-rec",      "duelrec"),
    ("PSP_GAME/USRDIR/gmodule/libcharalist.prx",     "charalist-reloc.elf",     0x09410000, "charalist",     "charalist"),
    ("PSP_GAME/USRDIR/gmodule/libshop.prx",          "shop-reloc.elf",          0x03600000, "shop",          "shop"),
    ("PSP_GAME/USRDIR/gmodule/libcardalbum.prx",     "cardalbum-reloc.elf",     0x09420000, "cardalbum",     "cardalbum"),
    ("PSP_GAME/USRDIR/gmodule/liblimitlist.prx",     "limitlist-reloc.elf",     0x09430000, "limitlist",     "limitlist"),
    ("PSP_GAME/USRDIR/gmodule/librecipe_viewer.prx", "recipeviewer-reloc.elf",  0x09440000, "recipeviewer",  "recipeviewer"),
    ("PSP_GAME/USRDIR/gmodule/libtutoriallist.prx",  "tutoriallist-reloc.elf",  0x09450000, "tutoriallist",  "tutoriallist"),
    ("PSP_GAME/USRDIR/gmodule/libselect_card.prx",   "selectcard-reloc.elf",    0x09460000, "selectcard",    "selectcard"),
    ("PSP_GAME/USRDIR/gmodule/liblabo.prx",          "labo-reloc.elf",          0x09480000, "labo",          "labo"),
    ("PSP_GAME/USRDIR/gmodule/libconv_machine.prx",  "conv-reloc.elf",          0x05000000, "conv",          "convmachine"),
    ("PSP_GAME/USRDIR/gmodule/libpassword.prx",      "password-reloc.elf",      0x05700000, "password",      "password"),
    ("PSP_GAME/USRDIR/gmodule/libhtml_view.prx",     "htmlview-reloc.elf",      0x05B00000, "htmlview",      "htmlview"),
]

DEPS_DIR = os.path.join(BUILD, "deps")
RELOC_DIR = os.path.join(BUILD, "reloc")
RELOC_ELF_DIR = os.path.join(RELOC_DIR, "elf")
GX_DATA_DIR = os.path.join(BUILD, "gx_data")
FLASH0_DIR = os.path.join(BUILD, "flash0")


def log(msg):
    print(msg, flush=True)


def run(cmd, cwd=ROOT):
    log(f"> {' '.join(cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)


EXE_EXT = ".exe" if os.name == "nt" else ""


def pick_cmake_generator():
    if shutil.which("ninja"):
        return "Ninja"
    return "MinGW Makefiles" if os.name == "nt" else "Unix Makefiles"


def check_tools():
    required = ["git", "cmake", "gcc", "g++", "pkg-config"]
    missing = [t for t in required if shutil.which(t) is None]
    if missing:
        hint = (
            "Install Git, CMake and MSYS2 UCRT64 (gcc, g++, pkg-config, ffmpeg, zlib, openssl; recommended: ninja, lld, ccache)."
            if os.name == "nt"
            else "Install git, cmake, gcc, g++, pkg-config, ninja-build, libsdl2-dev, libpng-dev, libgl-dev, ffmpeg dev libs, libssl-dev, zlib1g-dev."
        )
        raise RuntimeError(
            "Missing build tools in PATH: " + ", ".join(missing) + ". " + hint
        )
    pkgs = ["libavcodec", "libavformat", "libswresample", "libswscale", "libavutil"]
    if os.name != "nt":
        pkgs += ["sdl2", "libpng", "gl"]
    r = subprocess.run(["pkg-config", "--exists"] + pkgs, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(
            f"Missing dev packages via pkg-config ({', '.join(pkgs)})."
        )
    try:
        import PIL  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(
            f"Python package 'Pillow' is required by {sys.executable} (run: {sys.executable} -m pip install pillow)."
        ) from exc


def read_iso_path(iso_path, target_rel):
    parts = [p.upper() for p in target_rel.strip("/").split("/")]
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        cur_lba = struct.unpack_from("<I", pvd, 158)[0]
        cur_sz = struct.unpack_from("<I", pvd, 166)[0]
        for comp in parts:
            f.seek(cur_lba * 2048)
            buf = f.read(((cur_sz + 2047) // 2048) * 2048)
            off = 0
            found = None
            while off < cur_sz:
                rlen = buf[off]
                if rlen == 0:
                    off = ((off // 2048) + 1) * 2048
                    continue
                lba = struct.unpack_from("<I", buf, off + 2)[0]
                sz = struct.unpack_from("<I", buf, off + 10)[0]
                nlen = buf[off + 32]
                name = buf[off + 33:off + 33 + nlen].decode("ascii", "ignore").split(";")[0].upper()
                if name == comp:
                    found = (lba, sz)
                    break
                off += rlen
            if not found:
                raise FileNotFoundError(f"{target_rel} not found in {iso_path}")
            cur_lba, cur_sz = found
        f.seek(cur_lba * 2048)
        return f.read(cur_sz)


def read_iso_lbn(iso_path, lbn, size):
    with open(iso_path, "rb") as f:
        f.seek(lbn * 2048)
        data = f.read(size)
    if len(data) != size:
        raise RuntimeError(f"Short read from {iso_path} @ LBN 0x{lbn:x}")
    return data


def verify_iso_headers(tf1_iso, tf3_iso):
    if not os.path.isfile(tf1_iso):
        raise FileNotFoundError(f"TF1 ISO not found: {tf1_iso}")
    if not os.path.isfile(tf3_iso):
        raise FileNotFoundError(f"TF3 ISO not found: {tf3_iso}")
    read_iso_path(tf1_iso, "PSP_GAME/SYSDIR/EBOOT.BIN")
    read_iso_path(tf3_iso, "PSP_GAME/USRDIR/gmodule/rel_duel_eng.prx")


def ensure_flash0(flash0_dir=FLASH0_DIR, clean=False):
    font_dir = os.path.join(flash0_dir, "font")
    os.makedirs(font_dir, exist_ok=True)
    to_fetch = [
        fn for fn in FLASH0_FONT_FILES
        if clean or not os.path.isfile(os.path.join(font_dir, fn)) or os.path.getsize(os.path.join(font_dir, fn)) == 0
    ]
    if not to_fetch:
        log(f"flash0 fonts already present in {font_dir}")
        return flash0_dir

    log(f"Downloading {len(to_fetch)} flash0 font file(s) from PPSSPP assets...")

    def _fetch_one(fn):
        url = f"{FLASH0_FONT_BASE_URL}/{fn}"
        dst = os.path.join(font_dir, fn)
        tmp = dst + ".tmp"
        req = urllib.request.Request(url, headers={"User-Agent": "Seto-static-setup"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = resp.read()
        if not data:
            raise RuntimeError(f"Empty response downloading {url}")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dst)
        return fn

    with ThreadPoolExecutor(max_workers=6) as pool:
        futs = [pool.submit(_fetch_one, fn) for fn in to_fetch]
        for idx, fut in enumerate(as_completed(futs), 1):
            fn = fut.result()
            log(f"  [{idx}/{len(to_fetch)}] {fn}")
    return flash0_dir


def ensure_psprecomp(jobs, clean=False):
    pre_dir = os.path.join(DEPS_DIR, "psprecomp")
    pre_build = os.path.join(pre_dir, "build")
    recomp_exe = os.path.join(pre_build, "tools", "allegrexrecomp", f"allegrexrecomp{EXE_EXT}")

    if clean and os.path.isdir(pre_dir):
        run(["git", "-C", pre_dir, "reset", "--hard", PSPRECOMP_COMMIT])
        run(["git", "-C", pre_dir, "clean", "-fd"])

    if not os.path.isdir(os.path.join(pre_dir, ".git")):
        if os.path.isdir(pre_dir):
            shutil.rmtree(pre_dir)
        os.makedirs(DEPS_DIR, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--no-tags", PSPRECOMP_REPO, pre_dir])
        run(["git", "-C", pre_dir, "-c", "advice.detachedHead=false", "checkout", PSPRECOMP_COMMIT])

    if not os.path.isfile(os.path.join(pre_dir, "src", "hle", "atrac.c")):
        run(["git", "-C", pre_dir, "apply", "--whitespace=nowarn", os.path.join(ROOT, "patches", "psprecomp-5ca18fa-s10-duel-scene-full.patch")])
        run(["git", "-C", pre_dir, "apply", "--whitespace=nowarn", os.path.join(ROOT, "patches", "psprecomp-5ca18fa-b23-stack-frames.patch")])

    if clean or not os.path.isfile(recomp_exe):
        gen = pick_cmake_generator()
        cfg_cmd = [
            "cmake", "-S", pre_dir, "-B", pre_build,
            "-G", gen, "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_FLAGS=-O1",
        ]
        if shutil.which("ccache"):
            cfg_cmd.append("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
        run(cfg_cmd)
        run(["cmake", "--build", pre_build, "--target", "allegrexrecomp", "-j", str(jobs)])

    if not os.path.isfile(recomp_exe):
        raise RuntimeError(f"Missing allegrexrecomp binary at {recomp_exe}")
    return recomp_exe


def ensure_pspdecrypt(jobs, clean=False):
    pdec_dir = os.path.join(DEPS_DIR, "pspdecrypt")
    pdec_exe = os.path.join(pdec_dir, f"pspdecrypt{EXE_EXT}")

    if not os.path.isdir(os.path.join(pdec_dir, ".git")):
        if os.path.isdir(pdec_dir):
            shutil.rmtree(pdec_dir)
        os.makedirs(DEPS_DIR, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--no-tags", PSPDECRYPT_REPO, pdec_dir])
        run(["git", "-C", pdec_dir, "-c", "advice.detachedHead=false", "checkout", PSPDECRYPT_COMMIT])

    if clean or not os.path.isfile(pdec_exe):
        log(f"Compiling pspdecrypt{EXE_EXT}...")
        jobs_compile = []
        objs = []
        obj_tag = ".win.o" if os.name == "nt" else ".lnx.o"
        for root_d, _, files in os.walk(pdec_dir):
            if ".git" in root_d.split(os.sep):
                continue
            for fn in sorted(files):
                src = os.path.join(root_d, fn)
                if fn.endswith(".c"):
                    obj = src + obj_tag
                    jobs_compile.append((["gcc", "-O1", "-c", src, "-o", obj], obj))
                elif fn.endswith(".cpp"):
                    obj = src + obj_tag
                    jobs_compile.append((["g++", "-O1", "-c", src, "-o", obj], obj))
        with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
            futs = [pool.submit(subprocess.run, cmd, check=True) for cmd, _ in jobs_compile]
            for fut in as_completed(futs):
                fut.result()
        objs = [obj for _, obj in jobs_compile]
        subprocess.run(["g++", "-O1", "-o", pdec_exe] + objs + ["-lz", "-lcrypto"], check=True)
    return pdec_exe


_REG_FN_RE = re.compile(r'^(void\s+\w+\s*\(\s*void\s*\)\s*\{)\s*$', re.MULTILINE)
_REG_CALL_RE = re.compile(r'^\s*(psp_register|psp_register_label)\(\s*(0x[0-9A-Fa-f]+u?)\s*,\s*([A-Za-z_]\w*)\s*\);\s*$')


def compact_register_c(reg_file):
    """Rewrite huge *_register.c call chains into a static const table + loop."""
    if not os.path.isfile(reg_file):
        return
    with open(reg_file, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    if "_seto_reg_ent" in src:
        return
    m = _REG_FN_RE.search(src)
    if not m:
        return
    header = src[:m.start()]
    body = src[m.end():]
    entries = []
    closed = False
    for line in body.splitlines():
        s = line.strip()
        if not s:
            continue
        if s == "}":
            closed = True
            break
        cm = _REG_CALL_RE.match(s)
        if not cm:
            return
        is_lbl = 1 if cm.group(1) == "psp_register_label" else 0
        entries.append((cm.group(2), cm.group(3), is_lbl))
    if not closed or len(entries) < 64:
        return

    out = [
        header,
        "typedef struct { uint32_t addr; void (*fn)(void); uint8_t is_label; } _seto_reg_ent;\n",
        "static const _seto_reg_ent _seto_reg_tbl[] = {\n",
    ]
    out.extend(f"    {{{addr}, {sym}, {is_lbl}}},\n" for addr, sym, is_lbl in entries)
    out.append("};\n")
    out.append(f"{m.group(1)}\n")
    out.append(
        "    for (size_t i = 0; i < sizeof(_seto_reg_tbl) / sizeof(_seto_reg_tbl[0]); i++) {\n"
        "        if (_seto_reg_tbl[i].is_label) psp_register_label(_seto_reg_tbl[i].addr, _seto_reg_tbl[i].fn);\n"
        "        else psp_register(_seto_reg_tbl[i].addr, _seto_reg_tbl[i].fn);\n"
        "    }\n"
        "}\n"
    )
    tmp = reg_file + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)
    os.replace(tmp, reg_file)


def relocate_and_emit(recomp_exe, raw_elf, reloc_elf, base_addr, emit_subdir, ns, clean=False):
    reloc_script = os.path.join(ROOT, "scripts", "relocate_prx.py")
    os.makedirs(os.path.dirname(reloc_elf), exist_ok=True)
    if clean or not os.path.isfile(reloc_elf):
        run([sys.executable, reloc_script, raw_elf, reloc_elf, "--base", f"0x{base_addr:08X}"])
    emit_path = os.path.join(RELOC_DIR, emit_subdir)
    reg_file = os.path.join(emit_path, f"{ns}_register.c")
    if clean or not os.path.isfile(reg_file):
        os.makedirs(emit_path, exist_ok=True)
        run([recomp_exe, "emit", reloc_elf, emit_path, ns])
    compact_register_c(reg_file)


def extract_and_emit_all(tf1_iso, tf3_iso, recomp_exe, pdec_exe, jobs, clean=False):
    raw_dir = os.path.join(BUILD, "_setup_raw")
    os.makedirs(raw_dir, exist_ok=True)
    os.makedirs(RELOC_ELF_DIR, exist_ok=True)
    os.makedirs(GX_DATA_DIR, exist_ok=True)

    work_items = []

    # 1. TF1 EBOOT.BIN (~PSP -> ELF -> reloc 0x08000000 -> reloc/tagforce/)
    eboot_reloc = os.path.join(RELOC_ELF_DIR, "eboot-reloc.elf")
    eboot_reg = os.path.join(RELOC_DIR, "tagforce", "tagforce_register.c")
    if clean or not os.path.isfile(eboot_reloc) or not os.path.isfile(eboot_reg):
        eboot_enc = os.path.join(raw_dir, "TF1_EBOOT.BIN")
        eboot_dec = os.path.join(raw_dir, "TF1_EBOOT.elf")
        with open(eboot_enc, "wb") as f:
            f.write(read_iso_path(tf1_iso, "PSP_GAME/SYSDIR/EBOOT.BIN"))
        run([pdec_exe, "-o", eboot_dec, eboot_enc])
        work_items.append((eboot_dec, eboot_reloc, 0x08000000, "tagforce", "tagforce"))

    # 2. TF1 sceFont_Library (LBN 0x52C0, 0x5BA0 bytes, ~SCE 64B header + ~PSP -> gzip ELF)
    font_reloc = os.path.join(RELOC_ELF_DIR, "font-reloc.elf")
    font_reg = os.path.join(RELOC_DIR, "font", "font_register.c")
    if clean or not os.path.isfile(font_reloc) or not os.path.isfile(font_reg):
        font_blob = read_iso_lbn(tf1_iso, 0x52C0, 0x5BA0)
        font_psp = os.path.join(raw_dir, "font.psp")
        font_dec_gz = os.path.join(raw_dir, "font.elf.gz")
        font_dec = os.path.join(raw_dir, "font.elf")
        inner = font_blob[64:] if font_blob[:4] == b"~SCE" else font_blob
        with open(font_psp, "wb") as f:
            f.write(inner)
        run([pdec_exe, "-o", font_dec_gz, font_psp])
        with open(font_dec_gz, "rb") as f:
            dec_bytes = f.read()
        if dec_bytes[:2] == b"\x1f\x8b":
            dec_bytes = gzip.decompress(dec_bytes)
        with open(font_dec, "wb") as f:
            f.write(dec_bytes)
        work_items.append((font_dec, font_reloc, 0x08900000, "font", "font"))

    # 3. TF1 gmodule/*.prx
    for iso_rel, reloc_name, base_addr, emit_subdir, ns in TF1_GMODULES:
        reloc_elf = os.path.join(RELOC_ELF_DIR, reloc_name)
        reg_file = os.path.join(RELOC_DIR, emit_subdir, f"{ns}_register.c")
        if clean or not os.path.isfile(reloc_elf) or not os.path.isfile(reg_file):
            raw_elf = os.path.join(raw_dir, os.path.basename(iso_rel))
            with open(raw_elf, "wb") as f:
                f.write(read_iso_path(tf1_iso, iso_rel))
            work_items.append((raw_elf, reloc_elf, base_addr, emit_subdir, ns))

    # 4. TF3 BOOT.BIN + rel_duel_eng.prx
    tf3_boot = os.path.join(GX_DATA_DIR, "tf3_BOOT.BIN")
    if clean or not os.path.isfile(tf3_boot):
        with open(tf3_boot, "wb") as f:
            f.write(read_iso_path(tf3_iso, "PSP_GAME/SYSDIR/BOOT.BIN"))

    tf3eng_reloc = os.path.join(RELOC_ELF_DIR, "duel-tf3eng-reloc.elf")
    tf3eng_reg = os.path.join(RELOC_DIR, "duel-tf3eng", "tf3eng_register.c")
    if clean or not os.path.isfile(tf3eng_reloc) or not os.path.isfile(tf3eng_reg):
        raw_tf3eng = os.path.join(raw_dir, "rel_duel_eng.prx")
        with open(raw_tf3eng, "wb") as f:
            f.write(read_iso_path(tf3_iso, "PSP_GAME/USRDIR/gmodule/rel_duel_eng.prx"))
        work_items.append((raw_tf3eng, tf3eng_reloc, 0x03600000, "duel-tf3eng", "tf3eng"))

    if work_items:
        workers = max(1, min(jobs, 8))
        log(f"Relocating and emitting {len(work_items)} module(s) with {workers} worker(s)...")
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [
                pool.submit(relocate_and_emit, recomp_exe, raw_elf, reloc_elf, base_addr, emit_subdir, ns, clean=clean)
                for raw_elf, reloc_elf, base_addr, emit_subdir, ns in work_items
            ]
            for fut in as_completed(futs):
                fut.result()


def build_gx_data_and_patch(tf1_iso, tf3_iso):
    run([
        sys.executable, os.path.join(ROOT, "tools", "build_gx_carddb.py"),
        "--tf1-iso", tf1_iso, "--tf3-iso", tf3_iso, "--out-dir", GX_DATA_DIR,
    ])
    run([
        sys.executable, os.path.join(ROOT, "tools", "gen_arch_tables.py"),
        "--tf3-iso", tf3_iso,
        "--tf3-boot", os.path.join(GX_DATA_DIR, "tf3_BOOT.BIN"),
        "--eboot-reloc", os.path.join(RELOC_ELF_DIR, "eboot-reloc.elf"),
        "--tf3eng-dir", os.path.join(RELOC_DIR, "duel-tf3eng"),
        "--out", os.path.join(GX_DATA_DIR, "tf3_arch_tables.h"),
    ])
    run([
        sys.executable, os.path.join(ROOT, "tools", "extract_tf3_tables.py"),
        "--tf3-iso", tf3_iso,
        "--tf3-boot", os.path.join(GX_DATA_DIR, "tf3_BOOT.BIN"),
        "--tf3eng-dir", os.path.join(RELOC_DIR, "duel-tf3eng"),
        "--out", os.path.join(GX_DATA_DIR, "tf3_c_tables.h"),
        "--out-names", os.path.join(GX_DATA_DIR, "tf3_card_names.h"),
    ])
    run([sys.executable, os.path.join(ROOT, "tools", "apply_phase_c_patches.py")])


def build_host(tf3_iso, jobs):
    out_build = os.path.join(BUILD, "out")
    gen = pick_cmake_generator()
    run([
        "cmake", "-S", ROOT, "-B", out_build,
        "-G", gen, "-DCMAKE_BUILD_TYPE=Release",
        f"-DPython3_EXECUTABLE={sys.executable}",
        f"-DSETO_TF3_SHOP_ISO={tf3_iso}",
    ])
    run(["cmake", "--build", out_build, "--target", "Seto-static", "-j", str(jobs)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tf1-iso", required=True)
    ap.add_argument("--tf3-iso", required=True)
    ap.add_argument("--jobs", type=int, default=(os.cpu_count() or 8))
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()

    os.makedirs(BUILD, exist_ok=True)
    log("[1/7] Checking build toolchain and ISOs...")
    check_tools()
    verify_iso_headers(args.tf1_iso, args.tf3_iso)

    log("[2/7] Preparing flash0 fonts, psprecomp and pspdecrypt...")
    with ThreadPoolExecutor(max_workers=3) as pool:
        f_flash = pool.submit(ensure_flash0, clean=args.clean)
        f_recomp = pool.submit(ensure_psprecomp, args.jobs, clean=args.clean)
        f_pdec = pool.submit(ensure_pspdecrypt, args.jobs, clean=args.clean)
        f_flash.result()
        recomp_exe = f_recomp.result()
        pdec_exe = f_pdec.result()
    log("[4/7] Dependencies ready.")

    log("[5/7] Extracting, decrypting, relocating and emitting corpora...")
    extract_and_emit_all(args.tf1_iso, args.tf3_iso, recomp_exe, pdec_exe, args.jobs, clean=args.clean)

    log("[6/7] Generating Pure GX data and applying Phase C patches...")
    build_gx_data_and_patch(args.tf1_iso, args.tf3_iso)

    log(f"[7/7] Building Seto-static{EXE_EXT}...")
    build_host(args.tf3_iso, args.jobs)

    exe_path = os.path.join(BUILD, "out", f"Seto-static{EXE_EXT}")
    if not os.path.isfile(exe_path):
        raise RuntimeError(f"Build finished but {exe_path} was not produced.")
    if not os.path.isfile(os.path.join(FLASH0_DIR, "font", "ltn0.pgf")):
        raise RuntimeError(f"flash0 fonts missing under {FLASH0_DIR}")
    raw_dir = os.path.join(BUILD, "_setup_raw")
    if os.path.isdir(raw_dir):
        shutil.rmtree(raw_dir, ignore_errors=True)
    log("SETUP_COMPLETE")


if __name__ == "__main__":
    main()
