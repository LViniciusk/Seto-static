#!/usr/bin/env python3
"""Extract TF3 runtime tables (DLG_Indx_E, DLG_Text_E, CARD_Genre, 0x21F9E97E bit map)
from TF3.iso and build/tf3-probe/tf3_BOOT.BIN into build/gx_data/tf3_c_tables.h."""
import os, struct, re

ROOT = r"C:\Users\Vinicius\Documents\Github\Seto-static"
TF3_ISO = os.path.join(ROOT, "TF3.iso")
TF3_BOOT = os.path.join(ROOT, "build", "tf3-probe", "tf3_BOOT.BIN")
OUT_H = os.path.join(ROOT, "build", "gx_data", "tf3_c_tables.h")

# Locate cardinfo_eng.ehp in TF3.iso (LBA 0xaf7f0, 532944 bytes verified; also parse ISO dir to be exact)
def read_iso_file(iso_path, target_rel):
    parts = [p.upper() for p in target_rel.strip("/").split("/")]
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
        root_sz = struct.unpack_from("<I", pvd, 156 + 10)[0]
        cur_lba, cur_sz = root_lba, root_sz
        for idx, comp in enumerate(parts):
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
                raise FileNotFoundError(f"{comp} in {target_rel}")
            cur_lba, cur_sz = found
        f.seek(cur_lba * 2048)
        return f.read(cur_sz)

from build_gx_carddb import parse_ehp

ehp = read_iso_file(TF3_ISO, "PSP_GAME/USRDIR/duelsys/cardinfo_eng.ehp")
print("cardinfo_eng.ehp header:", ehp[:16].hex(), "len:", len(ehp))
entries = dict(parse_ehp(ehp))

print("TF3 EHP entries:", {k: len(v) for k, v in entries.items()})
dlg_indx = entries["DLG_Indx_E.bin"]
dlg_text = entries["DLG_Text_E.bin"]
card_genre = entries["CARD_Genre.bin"]

# Validate DLG_Indx_E
n_dlg = len(dlg_indx) // 4
offs = struct.unpack(f"<{n_dlg}I", dlg_indx[:n_dlg * 4])
print(f"DLG_Indx_E: {n_dlg} entries, max_off={max(offs)}, DLG_Text_E len={len(dlg_text)}")

# Extract 0x0009b980 bit table from tf3_BOOT.BIN and find its exact size in TF3 BOOT / max arg in tf3eng
t3 = open(TF3_BOOT, "rb").read()
ph = struct.unpack_from("<I", t3, 0x1c)[0]
_, p_off, p_va, _, p_fsz, _, _, _ = struct.unpack_from("<8I", t3, ph)
tbl_off = p_off + (0x0009b980 - p_va)
# Check how many non-padding entries precede 0x0009ba00 / strings
raw_tbl = struct.unpack_from("<48I", t3, tbl_off)
# Check TF3 BOOT 0x0002f638 for bounds or check max a1 passed to psp_import_03803604 in tf3eng
tf3eng_dir = os.path.join(ROOT, "build", "s9duel-tf3eng-reloc")
max_a1 = -1
seen_a1 = set()
for fn in os.listdir(tf3eng_dir):
    if not fn.endswith(".c"): continue
    lines = open(os.path.join(tf3eng_dir, fn), errors="ignore").read().splitlines()
    for i, l in enumerate(lines):
        if "psp_import_03803604" in l:
            # look backwards up to 8 lines for r_a1 =
            found_val = None
            for k in range(max(0, i-8), i+1):
                m = re.search(r"r_a1\s*=\s*r_zero\s*\+\s*(\d+)", lines[k])
                if m:
                    found_val = int(m.group(1))
            if found_val is not None:
                seen_a1.add(found_val)
                if found_val > max_a1: max_a1 = found_val
            else:
                # dynamic r_a1
                print(f"  dynamic a1 @ {fn}:{i+1}:", [x.strip() for x in lines[max(0,i-5):i+1]])
print("0x21F9E97E const a1 values:", sorted(seen_a1), "max_a1:", max_a1)
for i in range(48):
    v = raw_tbl[i]
    print(f"  tbl[{i:2d}] = {v} (0x{v:x})")

# Write C header
def emit_u8_arr(f, name, data):
    f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i+16])
        f.write(f"    {chunk},\n")
    f.write("};\n\n")

os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
with open(OUT_H, "w", encoding="utf-8") as f:
    f.write("/* Auto-generated from TF3.iso cardinfo_eng.ehp + tf3_BOOT.BIN. */\n")
    f.write("#ifndef TF3_C_TABLES_H\n#define TF3_C_TABLES_H\n#include <stdint.h>\n\n")
    f.write(f"#define TF3_DLG_COUNT {n_dlg - 1}u\n")
    f.write(f"#define TF3_GENRE_CARDS {len(card_genre) // 6}u\n")
    # 35 entries at 0x0009b980 (entries 35+ are string "disc0:/PSP_GAME/...")
    n_bit_tbl = 35
    f.write(f"#define TF3_GENRE_TBL_LEN {n_bit_tbl}u\n")
    f.write("static const uint8_t g_tf3_genre_bit_tbl[TF3_GENRE_TBL_LEN] = {")
    f.write(", ".join(str(raw_tbl[i]) for i in range(n_bit_tbl)))
    f.write("};\n\n")
    emit_u8_arr(f, "g_tf3_dlg_indx", dlg_indx)
    emit_u8_arr(f, "g_tf3_dlg_text", dlg_text)
    emit_u8_arr(f, "g_tf3_card_genre", card_genre)
    f.write("#endif /* TF3_C_TABLES_H */\n")
print("Wrote", OUT_H)
