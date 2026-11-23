#!/usr/bin/env python3
"""Evaluate all archetype/filter libehsys_rel NIDs (va >= 0x05bf00) in tf3_BOOT.BIN
over kid in [0..8000) and emit C lookup tables into build/gx_data/tf3_arch_tables.h.
"""
import argparse
from collections import Counter
import os
import re
import struct

from build_gx_carddb import parse_ehp
from extract_tf3_tables import read_iso_file


def rd32(b, o): return struct.unpack_from("<I", b, o)[0]
def rd16(b, o): return struct.unpack_from("<H", b, o)[0]
def s16(v): return v - 0x10000 if v & 0x8000 else v
def u32(v): return v & 0xFFFFFFFF
def s32(v): v &= 0xFFFFFFFF; return v - 0x100000000 if v & 0x80000000 else v


def load_elf(path):
    with open(path, "rb") as f:
        d = f.read()
    phoff = rd32(d, 28)
    phentsz, phnum = struct.unpack_from("<HH", d, 42)
    segs = []
    modoff = 0
    for i in range(phnum):
        p = phoff + i * phentsz
        pt, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, p)
        if i == 0:
            modoff = pa & 0x7FFFFFFF
        if pt == 1:
            segs.append((off, va, fsz, msz))

    def ptr_off(v):
        for off, bva, fsz, msz in segs:
            if bva <= v < bva + fsz:
                return off + (v - bva)
        return None

    ent_top, ent_end = struct.unpack_from("<2I", d, modoff + 0x24)
    ex = {}
    e = ptr_off(ent_top)
    e_end = ptr_off(ent_end)
    while e + 16 <= e_end:
        namep = rd32(d, e)
        elen = d[e + 8]
        nvar = d[e + 9]
        nfunc = rd16(d, e + 10)
        tbl = rd32(d, e + 12)
        lib = d[ptr_off(namep):].split(b"\x00", 1)[0].decode("ascii", "ignore") if namep else "<self>"
        toff = ptr_off(tbl)
        total = nvar + nfunc
        ex[lib] = {rd32(d, toff + i * 4): rd32(d, toff + (total + i) * 4) for i in range(nfunc)}
        e += max(16, elen * 4)
    return d, ptr_off, ex


def make_runner(db, pb, intid_b, prop_b, genre_b):
    def run_fn(entry_va, a0):
        R = [0] * 32
        R[4] = u32(a0)
        R[29] = 0x09FF0000
        R[31] = 0xDEAD0000
        stack = {}

        def m_rd8(addr):
            addr = u32(addr)
            if 0x08800000 <= addr < 0x08800000 + len(intid_b):
                return intid_b[addr - 0x08800000]
            if 0x08800000 + len(intid_b) <= addr < 0x08810000:
                return 0
            if 0x08900000 <= addr < 0x08910000:
                off = addr - 0x08900000
                return prop_b[off] if off < len(prop_b) else 0
            if 0x08A00000 <= addr < 0x08A10000:
                off = addr - 0x08A00000
                return genre_b[off] if off < len(genre_b) else 0
            if 0x09FE0000 <= addr < 0x0A000000:
                return stack.get(addr, 0)
            o = pb(addr)
            if o is None:
                raise RuntimeError(f"bad rd8 @ 0x{addr:08x}")
            return db[o]

        def m_rd16(addr):
            return m_rd8(addr) | (m_rd8(addr + 1) << 8)

        def m_rd32(addr):
            addr = u32(addr)
            if addr == 0x00120D3C:
                return 0x08A00000
            if addr == 0x00120D48:
                return 0x08900000
            if addr == 0x00120D50:
                return 0x08800000
            return m_rd8(addr) | (m_rd8(addr + 1) << 8) | (m_rd8(addr + 2) << 16) | (m_rd8(addr + 3) << 24)

        def m_wr8(addr, v):
            stack[u32(addr)] = v & 0xFF

        def m_wr32(addr, v):
            addr = u32(addr)
            for i in range(4):
                stack[addr + i] = (v >> (8 * i)) & 0xFF

        pc = entry_va
        steps = 0
        while pc != 0xDEAD0000:
            steps += 1
            if steps > 5000:
                raise RuntimeError("loop")
            w = rd32(db, pb(pc))
            op = (w >> 26) & 63
            rs = (w >> 21) & 31
            rt = (w >> 16) & 31
            rd = (w >> 11) & 31
            sa = (w >> 6) & 31
            fn = w & 63
            imm = w & 0xFFFF
            simm = s16(imm)
            npc = pc + 4
            tpc = None
            null = False
            if w == 0:
                pass
            elif op == 0:
                if fn == 0: R[rd] = u32(R[rt] << sa)
                elif fn == 2: R[rd] = u32(R[rt]) >> sa
                elif fn == 3: R[rd] = u32(s32(R[rt]) >> sa)
                elif fn == 4: R[rd] = u32(R[rt] << (R[rs] & 31))
                elif fn == 6: R[rd] = u32(R[rt]) >> (R[rs] & 31)
                elif fn == 8: tpc = R[rs]
                elif fn == 9: R[31] = pc + 8; tpc = R[rs]
                elif fn == 0x0A:
                    if R[rt] == 0: R[rd] = R[rs]
                elif fn == 0x0B:
                    if R[rt] != 0: R[rd] = R[rs]
                elif fn in (0x20, 0x21): R[rd] = u32(R[rs] + R[rt])
                elif fn in (0x22, 0x23): R[rd] = u32(R[rs] - R[rt])
                elif fn == 0x24: R[rd] = R[rs] & R[rt]
                elif fn == 0x25: R[rd] = R[rs] | R[rt]
                elif fn == 0x26: R[rd] = R[rs] ^ R[rt]
                elif fn == 0x27: R[rd] = u32(~(R[rs] | R[rt]))
                elif fn == 0x2A: R[rd] = 1 if s32(R[rs]) < s32(R[rt]) else 0
                elif fn == 0x2B: R[rd] = 1 if u32(R[rs]) < u32(R[rt]) else 0
                else: raise RuntimeError(f"R fn={fn:02x}")
            elif op == 1:
                if (s32(R[rs]) < 0) if (rt & 1) == 0 else (s32(R[rs]) >= 0):
                    tpc = pc + 4 + simm * 4
                elif rt & 2:
                    null = True
            elif op == 2:
                tpc = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            elif op == 3:
                tgt = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                R[31] = pc + 8
                tpc = tgt
            elif op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
                b = op & 3
                c = (R[rs] == R[rt]) if b == 0 else (R[rs] != R[rt]) if b == 1 else (s32(R[rs]) <= 0) if b == 2 else (s32(R[rs]) > 0)
                if c:
                    tpc = pc + 4 + simm * 4
                elif op >= 0x14:
                    null = True
            elif op in (8, 9): R[rt] = u32(R[rs] + simm)
            elif op == 10: R[rt] = 1 if s32(R[rs]) < simm else 0
            elif op == 11: R[rt] = 1 if u32(R[rs]) < u32(simm) else 0
            elif op == 12: R[rt] = R[rs] & imm
            elif op == 13: R[rt] = R[rs] | imm
            elif op == 14: R[rt] = R[rs] ^ imm
            elif op == 15: R[rt] = u32(imm << 16)
            elif op == 0x1F:
                if fn == 0:
                    R[rt] = (R[rs] >> sa) & ((1 << (rd + 1)) - 1)
                elif fn == 0x20 and sa == 0x10:
                    b = R[rt] & 0xFF
                    R[rd] = u32(b - 256 if b & 0x80 else b)
                elif fn == 0x20 and sa == 0x18:
                    h = R[rt] & 0xFFFF
                    R[rd] = u32(h - 65536 if h & 0x8000 else h)
                else:
                    raise RuntimeError(f"1f fn={fn}")
            elif op == 0x20:
                b = m_rd8(R[rs] + simm)
                R[rt] = u32(b - 256 if b & 0x80 else b)
            elif op == 0x21:
                h = m_rd16(R[rs] + simm)
                R[rt] = u32(h - 65536 if h & 0x8000 else h)
            elif op == 0x23: R[rt] = m_rd32(R[rs] + simm)
            elif op == 0x24: R[rt] = m_rd8(R[rs] + simm)
            elif op == 0x25: R[rt] = m_rd16(R[rs] + simm)
            elif op == 0x28: m_wr8(R[rs] + simm, R[rt])
            elif op == 0x29:
                m_wr8(R[rs] + simm, R[rt])
                m_wr8(R[rs] + simm + 1, R[rt] >> 8)
            elif op == 0x2B: m_wr32(R[rs] + simm, R[rt])
            else:
                raise RuntimeError(f"op={op:02x}")
            R[0] = 0
            if null:
                pc = pc + 8
            elif tpc is not None:
                dw = rd32(db, pb(pc + 4))
                if dw != 0:
                    dop = (dw >> 26) & 63
                    drs = (dw >> 21) & 31
                    drt = (dw >> 16) & 31
                    drd = (dw >> 11) & 31
                    dsa = (dw >> 6) & 31
                    dfn = dw & 63
                    dim = dw & 0xFFFF
                    dsi = s16(dim)
                    if dop == 0 and dfn == 0: R[drd] = u32(R[drt] << dsa)
                    elif dop == 0 and dfn == 2: R[drd] = u32(R[drt]) >> dsa
                    elif dop == 0 and dfn == 3: R[drd] = u32(s32(R[drt]) >> dsa)
                    elif dop == 0 and dfn == 0x21: R[drd] = u32(R[drs] + R[drt])
                    elif dop == 0 and dfn == 0x23: R[drd] = u32(R[drs] - R[drt])
                    elif dop == 0 and dfn == 0x24: R[drd] = R[drs] & R[drt]
                    elif dop == 0 and dfn == 0x25: R[drd] = R[drs] | R[drt]
                    elif dop == 0 and dfn == 0x2B: R[drd] = 1 if u32(R[drs]) < u32(R[drt]) else 0
                    elif dop in (8, 9): R[drt] = u32(R[drs] + dsi)
                    elif dop == 11: R[drt] = 1 if u32(R[drs]) < u32(dsi) else 0
                    elif dop == 12: R[drt] = R[drs] & dim
                    elif dop == 13: R[drt] = R[drs] | dim
                    elif dop == 15: R[drt] = u32(dim << 16)
                    elif dop == 0x23: R[drt] = m_rd32(R[drs] + dsi)
                    elif dop == 0x24: R[drt] = m_rd8(R[drs] + dsi)
                    elif dop == 0x25: R[drt] = m_rd16(R[drs] + dsi)
                    elif dop == 0x28: m_wr8(R[drs] + dsi, R[drt])
                    elif dop == 0x2B: m_wr32(R[drs] + dsi, R[drt])
                    else: raise RuntimeError(f"delay dop={dop:02x}")
                    R[0] = 0
                pc = u32(tpc)
            else:
                pc = npc
        return R[2]

    return run_fn


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--tf3-iso", default=os.path.join(root, "TF3.iso"))
    ap.add_argument("--tf3-boot", default=os.path.join(root, "build", "gx_data", "tf3_BOOT.BIN"))
    ap.add_argument("--eboot-reloc", default=os.path.join(root, "build", "reloc", "elf", "eboot-reloc.elf"))
    ap.add_argument("--tf3eng-dir", default=os.path.join(root, "build", "reloc", "duel-tf3eng"))
    ap.add_argument("--out", default=os.path.join(root, "build", "gx_data", "tf3_arch_tables.h"))
    args = ap.parse_args()

    db, pb, exb = load_elf(args.tf3_boot)
    _, _, ex1_eb = load_elf(args.eboot_reloc)
    ehp = read_iso_file(args.tf3_iso, "PSP_GAME/USRDIR/duelsys/cardinfo_eng.ehp")
    ci_entries = dict(parse_ehp(ehp))
    prop_b = ci_entries["CARD_Prop.bin"]
    intid_b = ci_entries["CARD_IntID.bin"]
    genre_b = ci_entries["CARD_Genre.bin"]

    imports_txt = open(os.path.join(args.tf3eng_dir, "tf3eng_imports.c"), encoding="utf-8", errors="ignore").read()
    nids = [int(m, 16) for m in re.findall(r"psp_hle_call\(0x([0-9A-Fa-f]+)u\)", imports_txt)]
    tf1_eb = ex1_eb["libehsys"]
    tf3_eb = exb["libehsys_rel"]
    miss49 = sorted(
        [n for n in nids if (n not in tf1_eb and tf3_eb[n] >= 0x05BF00) or (n in tf1_eb and 0x08049F78 <= tf1_eb[n] <= 0x0804CA08)],
        key=lambda x: tf3_eb[x],
    )
    print(f"Total card/archetype predicates to evaluate from tf3_BOOT.BIN: {len(miss49)}")

    run_fn = make_runner(db, pb, intid_b, prop_b, genre_b)
    max_kid = 8000
    words = (max_kid + 63) // 64
    bool_nids = []
    multi_nids = []

    for nid in miss49:
        va = tf3_eb[nid]
        vals = [run_fn(va, k) for k in range(max_kid)]
        uniq = sorted(set(vals))
        if set(uniq).issubset({0, 1}):
            bits = [0] * words
            for k, v in enumerate(vals):
                if v:
                    bits[k >> 6] |= 1 << (k & 63)
            bool_nids.append((nid, va, bits))
        else:
            def_val = Counter(vals).most_common(1)[0][0]
            non_id = [(k, v) for k, v in enumerate(vals) if v != k]
            non_def = [(k, v) for k, v in enumerate(vals) if v != def_val]
            if len(non_id) < len(non_def):
                multi_nids.append((nid, va, uniq, non_id, True, 0))
            else:
                multi_nids.append((nid, va, uniq, non_def, False, def_val))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/gen_arch_tables.py from tf3_BOOT.BIN */\n")
        f.write("#ifndef TF3_ARCH_TABLES_H\n#define TF3_ARCH_TABLES_H\n#include <stdint.h>\n\n")
        f.write(f"#define TF3_ARCH_KID_LIMIT {max_kid}u\n")
        f.write(f"#define TF3_ARCH_WORDS     {words}u\n\n")
        f.write("typedef struct { uint32_t nid; uint64_t bits[TF3_ARCH_WORDS]; } tf3_arch_entry_t;\n\n")
        f.write(f"static const tf3_arch_entry_t g_tf3_arch_bool[{len(bool_nids)}] = {{\n")
        for nid, _, bits in bool_nids:
            hex_words = ",".join(f"0x{w:016x}ULL" for w in bits)
            f.write(f"    {{ 0x{nid:08x}u, {{ {hex_words} }} }},\n")
        f.write("};\n\n")
        f.write(f"#define TF3_ARCH_BOOL_COUNT {len(bool_nids)}u\n\n")
        for nid, _, _, nz, def_id, def_val in multi_nids:
            f.write(f"static inline uint32_t tf3_arch_fn_{nid:08x}(uint16_t kid) {{\n")
            f.write("    switch (kid) {\n")
            for k, v in nz:
                f.write(f"    case {k}u: return {v}u;\n")
            f.write(f"    default: return {'kid' if def_id else f'{def_val}u'};\n    }}\n}}\n\n")
        f.write("static inline int tf3_arch_eval(uint32_t nid, uint16_t kid, uint32_t *out) {\n")
        for nid, _, _, _, _, _ in multi_nids:
            f.write(f"    if (nid == 0x{nid:08x}u) {{ *out = tf3_arch_fn_{nid:08x}(kid); return 1; }}\n")
        f.write("    for (unsigned i = 0; i < TF3_ARCH_BOOL_COUNT; i++) {\n")
        f.write("        if (g_tf3_arch_bool[i].nid == nid) {\n")
        f.write("            *out = (kid < TF3_ARCH_KID_LIMIT) ? (uint32_t)((g_tf3_arch_bool[i].bits[kid >> 6] >> (kid & 63)) & 1u) : 0u;\n            return 1;\n")
        f.write("        }\n    }\n    return 0;\n}\n\n")
        arr_5d = struct.unpack_from("<462H", db, pb(0x001103F0))
        f.write("static const uint16_t g_tf3_tbl_5d9e6261[462] = {\n")
        for i in range(0, 462, 16):
            f.write("    " + ", ".join(str(x) for x in arr_5d[i:i + 16]) + ("," if i + 16 < 462 else "") + "\n")
        f.write("};\n\n#endif\n")

    print(f"Wrote {args.out}: {len(bool_nids)} bool bitsets + {len(multi_nids)} multi-val switches")


if __name__ == "__main__":
    main()
