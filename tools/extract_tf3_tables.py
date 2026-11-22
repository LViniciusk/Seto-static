#!/usr/bin/env python3
"""Extract TF3 runtime tables (DLG_Indx_E, DLG_Text_E, CARD_Genre, 0x21F9E97E bit map)
from TF3.iso and TF3 BOOT.BIN into build/gx_data/tf3_c_tables.h."""
import argparse
import os
import re
import struct

from build_gx_carddb import parse_ehp


def read_iso_file(iso_path, target_rel):
    parts = [p.upper() for p in target_rel.strip("/").split("/")]
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
        root_sz = struct.unpack_from("<I", pvd, 156 + 10)[0]
        cur_lba, cur_sz = root_lba, root_sz
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
                raise FileNotFoundError(f"{comp} in {target_rel}")
            cur_lba, cur_sz = found
        f.seek(cur_lba * 2048)
        return f.read(cur_sz)


def emit_u8_arr(f, name, data):
    f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
        f.write(f"    {chunk},\n")
    f.write("};\n\n")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--tf3-iso", default=os.path.join(root, "TF3.iso"))
    ap.add_argument("--tf3-boot", default=os.path.join(root, "build", "gx_data", "tf3_BOOT.BIN"))
    ap.add_argument("--tf3eng-dir", default=os.path.join(root, "build", "reloc", "duel-tf3eng"))
    ap.add_argument("--out", default=os.path.join(root, "build", "gx_data", "tf3_c_tables.h"))
    ap.add_argument("--out-names", default=os.path.join(root, "build", "gx_data", "tf3_card_names.h"))
    args = ap.parse_args()

    ehp = read_iso_file(args.tf3_iso, "PSP_GAME/USRDIR/duelsys/cardinfo_eng.ehp")
    entries = dict(parse_ehp(ehp))

    dlg_indx = entries["DLG_Indx_E.bin"]
    dlg_text = entries["DLG_Text_E.bin"]
    card_genre = entries["CARD_Genre.bin"]
    card_indx = entries["CARD_Indx_E.bin"]
    card_names = entries["CARD_Name_E.bin"]

    n_dlg = len(dlg_indx) // 4
    offs = struct.unpack(f"<{n_dlg}I", dlg_indx[:n_dlg * 4])
    print(f"DLG_Indx_E: {n_dlg} entries, max_off={max(offs)}, DLG_Text_E len={len(dlg_text)}")

    with open(args.tf3_boot, "rb") as f:
        t3 = f.read()
    ph = struct.unpack_from("<I", t3, 0x1c)[0]
    _, p_off, p_va, _, _, _, _, _ = struct.unpack_from("<8I", t3, ph)
    tbl_off = p_off + (0x0009b980 - p_va)
    raw_tbl = struct.unpack_from("<48I", t3, tbl_off)

    if os.path.isdir(args.tf3eng_dir):
        seen_a1 = set()
        for fn in os.listdir(args.tf3eng_dir):
            if not fn.endswith(".c"):
                continue
            lines = open(os.path.join(args.tf3eng_dir, fn), errors="ignore").read().splitlines()
            for i, line in enumerate(lines):
                if "psp_import_03803604" in line:
                    for k in range(max(0, i - 8), i + 1):
                        m = re.search(r"r_a1\s*=\s*r_zero\s*\+\s*(\d+)", lines[k])
                        if m:
                            seen_a1.add(int(m.group(1)))
        if seen_a1:
            print("0x21F9E97E const a1 values:", sorted(seen_a1), "max_a1:", max(seen_a1))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated from TF3.iso cardinfo_eng.ehp + tf3_BOOT.BIN. */\n")
        f.write("#ifndef TF3_C_TABLES_H\n#define TF3_C_TABLES_H\n#include <stdint.h>\n\n")
        f.write(f"#define TF3_DLG_COUNT {n_dlg - 1}u\n")
        f.write(f"#define TF3_GENRE_CARDS {len(card_genre) // 6}u\n")
        n_bit_tbl = 35
        f.write(f"#define TF3_GENRE_TBL_LEN {n_bit_tbl}u\n")
        f.write("static const uint8_t g_tf3_genre_bit_tbl[TF3_GENRE_TBL_LEN] = {")
        f.write(", ".join(str(raw_tbl[i]) for i in range(n_bit_tbl)))
        f.write("};\n\n")
        emit_u8_arr(f, "g_tf3_dlg_indx", dlg_indx)
        emit_u8_arr(f, "g_tf3_dlg_text", dlg_text)
        emit_u8_arr(f, "g_tf3_card_genre", card_genre)
        f.write("#endif /* TF3_C_TABLES_H */\n")
    print("Wrote", args.out)

    name_slots = len(card_indx) // 8
    name_offs = [struct.unpack_from("<II", card_indx, i * 8)[0] for i in range(name_slots)]
    os.makedirs(os.path.dirname(args.out_names), exist_ok=True)
    with open(args.out_names, "w", encoding="utf-8") as f:
        f.write("#ifndef SETO_TF3_CARD_NAMES_H\n#define SETO_TF3_CARD_NAMES_H\n#include <stdint.h>\n")
        f.write(f"#define TF3_CARD_NAME_SLOTS {name_slots}u\n")
        f.write(f"#define TF3_CARD_NAME_BYTES {len(card_names)}u\n")
        f.write("static const uint32_t g_tf3_name_off_by_idx[TF3_CARD_NAME_SLOTS] = {\n")
        for i in range(0, name_slots, 12):
            f.write("  " + ", ".join(f"{x}u" for x in name_offs[i:i + 12]) + ",\n")
        f.write("};\n")
        f.write("static const uint8_t g_tf3_card_names[TF3_CARD_NAME_BYTES] = {\n")
        for i in range(0, len(card_names), 16):
            f.write("  " + ", ".join(f"0x{b:02X}" for b in card_names[i:i + 16]) + ",\n")
        f.write("};\n#endif\n")
    print("Wrote", args.out_names)


if __name__ == "__main__":
    main()
