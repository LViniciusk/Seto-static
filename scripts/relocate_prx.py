#!/usr/bin/env python3
"""Relocate a decrypted PSP PRX for Seto-static.

Only the PSP relocation types these modules use are implemented, with the
semantics PPSSPP's ELF loader applies: r_info bits 8..15 name the segment the
word lives in, bits 16..23 the segment whose load address is added to the
value. The output is an ELF whose PT_LOAD vaddrs and section addresses are
rebased, so the existing C emitter can translate it as a normal image.

Modules linked as two PT_LOAD segments carry their relocations in PT_PSPREL;
modules linked as one segment keep them in the `.rel.*` sections. Both are
handled.

Usage: relocate_prx.py <in.elf> <out.elf> [--base 0x08900000]
"""
import struct
import sys

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
PT_LOAD, PT_PSPREL = 1, 0x700000A0
SHF_ALLOC = 2
RAM_LO, RAM_HI = 0x08000000, 0x0A000000
HEAP_LO = 0x09700000
EBOOT_END = 0x00640000
VRAM_LO = 0x04000000
VRAM_HI = 0x04400000
SHT_PSPREL = 0x700000A0


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def pu32(d, o, v):
    struct.pack_into("<I", d, o, v & 0xFFFFFFFF)


def fail(msg):
    raise SystemExit("relocate_prx: " + msg)


def main(argv):
    if len(argv) < 3:
        fail("usage: relocate_prx.py <in.elf> <out.elf> [--base 0x...]")
    base = 0x08900000
    if len(argv) >= 5 and argv[3] == "--base":
        base = int(argv[4], 0)
    data = bytearray(open(argv[1], "rb").read())
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail("not a 32-bit little-endian ELF")
    if struct.unpack_from("<H", data, 18)[0] != 8:
        fail("not MIPS")

    phoff = u32(data, 28)
    phentsize = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    shoff = u32(data, 32)
    shentsize = struct.unpack_from("<H", data, 46)[0]
    shnum = struct.unpack_from("<H", data, 48)[0]

    segs, rel_ph = [], None
    for i in range(phnum):
        p = phoff + i * phentsize
        if p + 32 > len(data):
            fail("program header out of file")
        p_type = u32(data, p)
        if p_type == PT_LOAD:
            segs.append({
                "ph": p,
                "off": u32(data, p + 4),
                "vaddr": u32(data, p + 8),
                "filesz": u32(data, p + 16),
                "memsz": u32(data, p + 20),
            })
        elif p_type == PT_PSPREL:
            rel_ph = p
    if len(segs) not in (1, 2):
        fail("expected one or two PT_LOAD segments, found %d" % len(segs))

    for i, s in enumerate(segs):
        if s["filesz"] > s["memsz"]:
            fail("segment %d filesz > memsz" % i)
        if s["off"] + s["filesz"] > len(data):
            fail("segment %d outside the file" % i)
        if s["vaddr"] + s["memsz"] > RAM_HI:
            fail("segment %d outside the link range" % i)
    if segs[0]["vaddr"] != 0:
        fail("expected the first segment to link at zero")

    # The fixed base is below the runtime's heap; it must not collide with the
    # EBOOT (a separate host-mapped image) or the RAM limits.
    if base & 3:
        fail("base is not word-aligned")
    end = max(base + s["vaddr"] + s["memsz"] for s in segs)
    if base < EBOOT_END:
        fail("relocated image overlaps the EBOOT module mapping")
    # Two validated windows: the PSP RAM window used by sceFont_Library, and
    # the low window between the EBOOT mapping and VRAM for a module too large
    # for RAM. Neither may touch VRAM (0x04000000) or the runtime heap.
    if RAM_LO <= base and end <= HEAP_LO:
        pass
    elif base >= EBOOT_END and end <= VRAM_LO:
        pass
    elif base >= VRAM_HI and end <= RAM_LO:
        pass
    else:
        fail("relocated image [0x%08X,0x%08X) leaves the free windows"
             % (base, end))
    if len(segs) == 2 and segs[0]["memsz"] > segs[1]["vaddr"]:
        fail("the two segments overlap")
    old_vaddr = [s["vaddr"] for s in segs]
    for s in segs:
        s["vaddr"] += base
    for s in segs:
        pu32(data, s["ph"] + 8, s["vaddr"])

    # The ELF entry is an image offset like any other code address. A library
    # with no initializer uses 0xFFFFFFFF and keeps it.
    entry = u32(data, 24)
    if entry < 0x80000000:
        pu32(data, 24, entry + base)

    # Section addresses feed the emitter's .text/module-info discovery.
    for i in range(shnum):
        sh = shoff + i * shentsize
        if sh + 40 > len(data):
            break
        flags = u32(data, sh + 8)
        addr = u32(data, sh + 12)
        if flags & SHF_ALLOC:
            pu32(data, sh + 12, addr + base)

    # ---- relocation table -------------------------------------------------
    pairs = []
    if rel_ph:
        rel_off = u32(data, rel_ph + 4)
        rel_size = u32(data, rel_ph + 16)
        if rel_off + rel_size > len(data) or rel_size % 8:
            fail("bad relocation segment")
        for i in range(rel_size // 8):
            pairs.append((u32(data, rel_off + i * 8),
                          u32(data, rel_off + i * 8 + 4)))
    else:
        # One-segment modules keep the same records in `.rel.*` sections.
        for i in range(shnum):
            sh = shoff + i * shentsize
            if sh + 40 > len(data):
                break
            if u32(data, sh + 4) != SHT_PSPREL:
                continue
            off = u32(data, sh + 16)
            size = u32(data, sh + 20)
            if size == 0:
                continue
            if off + size > len(data) or size % 8:
                fail("bad relocation section %d" % i)
            for j in range(size // 8):
                pairs.append((u32(data, off + j * 8),
                              u32(data, off + j * 8 + 4)))
    n = len(pairs)
    if n == 0:
        fail("no relocation entries")
    entries = []
    for i in range(n):
        r_offset, r_info = pairs[i]
        typ = r_info & 0xF
        rw = (r_info >> 8) & 0xFF
        rel = (r_info >> 16) & 0xFF
        if typ not in (R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16):
            fail("unknown relocation type %d at entry %d" % (typ, i))
        if rw >= len(segs) or rel >= len(segs):
            fail("bad segment in relocation %d (rw=%d rel=%d)" % (i, rw, rel))
        if r_offset + 4 > segs[rw]["filesz"]:
            fail("relocation %d targets non-file-backed memory" % i)
        entries.append({"off": r_offset, "type": typ, "rw": rw, "rel": rel})

    # Snapshot every word before writing: a HI16 needs its LO16's original
    # immediate even if the LO16 is applied first.
    orig = [u32(data, segs[e["rw"]]["off"] + e["off"]) for e in entries]

    # Pair each HI16 with the next unused LO16 in the relocation table. The
    # linker emits the pair adjacently in the stream even when the two
    # instructions are far apart in the code; code order is not the pairing
    # (branches can reach a lui's low half from another path).
    hi_entries = [(i, e, orig[i]) for i, e in enumerate(entries)
                  if e["type"] == R_MIPS_HI16]
    pair = {}
    used_lo = set()
    for i, e, w in hi_entries:
        if (w >> 26) != 0x0F:
            fail("HI16 at entry %d is not a lui" % i)
        j = None
        for k in range(i + 1, len(entries)):
            if entries[k]["type"] == R_MIPS_LO16 and k not in used_lo:
                j = k
                break
        if j is None:
            fail("HI16 at entry %d has no LO16 after it in the table" % i)
        used_lo.add(j)
        pair[i] = j

    counts = {R_MIPS_32: 0, R_MIPS_26: 0, R_MIPS_HI16: 0, R_MIPS_LO16: 0}
    for i, e in enumerate(entries):
        typ, op = e["type"], orig[i]
        relocate_to = base + old_vaddr[e["rel"]]
        if typ == R_MIPS_32:
            new = op + relocate_to
        elif typ == R_MIPS_26:
            new = (op & 0xFC000000) | (((op & 0x03FFFFFF) +
                                        (relocate_to >> 2)) & 0x03FFFFFF)
        elif typ == R_MIPS_LO16:
            new = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocate_to) & 0xFFFF)
        else:
            t = pair[i]
            lo = orig[t]
            lo_to = base + old_vaddr[entries[t]["rel"]]
            lo_s = struct.unpack("<h", struct.pack("<H", lo & 0xFFFF))[0]
            # MIPS %hi/%lo: the 16-bit low half is sign-extended by the
            # instruction, so the high half must include its carry. Using
            # (A - (A & 0xFFFF)) >> 16 here instead would be 0x10000 low for
            # every address whose low half is >= 0x8000.
            addr = (((op & 0xFFFF) << 16) + lo_s + lo_to) & 0xFFFFFFFF
            hi = ((addr + 0x8000) & 0xFFFFFFFF) >> 16
            new = (op & 0xFFFF0000) | (hi & 0xFFFF)
        pu32(data, segs[e["rw"]]["off"] + e["off"], new)
        counts[typ] += 1

    # ---- independent read-back checks -------------------------------------
    for i, e in enumerate(entries):
        got = u32(data, segs[e["rw"]]["off"] + e["off"])
        op = orig[i]
        relocate_to = base + old_vaddr[e["rel"]]
        if e["type"] == R_MIPS_32:
            want = (op + relocate_to) & 0xFFFFFFFF
            ok = got == want
        elif e["type"] == R_MIPS_26:
            want = (op & 0xFC000000) | (((op & 0x03FFFFFF) +
                                         (relocate_to >> 2)) & 0x03FFFFFF)
            target = (got & 0x03FFFFFF) << 2
            ok = got == want and target >= base and target < end and (target & 3) == 0
        elif e["type"] == R_MIPS_LO16:
            want = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocate_to) & 0xFFFF)
            ok = got == want
        else:
            # Reconstruct the address from the written pair with the same
            # signed-low rule the instruction uses.
            t = pair[i]
            lo = u32(data, segs[entries[t]["rw"]]["off"] + entries[t]["off"])
            lo_s = struct.unpack("<h", struct.pack("<H", orig[t] & 0xFFFF))[0]
            want = (((op & 0xFFFF) << 16) + lo_s + relocate_to) & 0xFFFFFFFF
            lo_p = struct.unpack("<h", struct.pack("<H", lo & 0xFFFF))[0]
            got_addr = (((got & 0xFFFF) << 16) + lo_p) & 0xFFFFFFFF
            ok = got_addr == want
        if not ok:
            fail("relocation %d (type %d) failed verification" % (i, e["type"]))

    vstub_count = resolve_vstub_imports(data, segs, _EXPORT_ELFS)
    if vstub_count:
        print("  vstub %d variable import fixups" % vstub_count)
    open(argv[2], "wb").write(data)
    print("relocated: %d segments, base 0x%08X, %d relocations" % (len(segs), base, n))
    for i, s in enumerate(segs):
        print("  segment %d: vaddr 0x%08X memsz 0x%X" % (i, s["vaddr"], s["memsz"]))
    print("  R_MIPS_32 %d, R_MIPS_26 %d, HI16 %d, LO16 %d" %
          (counts[R_MIPS_32], counts[R_MIPS_26], counts[R_MIPS_HI16], counts[R_MIPS_LO16]))
    print("  end 0x%08X" % end)
    return 0


_EXPORT_ELFS = []


def _elf_segs(buf):
    e_phoff = struct.unpack_from("<I", buf, 0x1C)[0]
    e_phnum = struct.unpack_from("<H", buf, 0x2C)[0]
    out = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _ = struct.unpack_from("<6I", buf, e_phoff + i * 32)
        if p_type == 1:
            out.append((p_vaddr, p_off, p_filesz))
    return out


def _elf_v2o(segs, va):
    for bva, off, fsz in segs:
        if bva <= va < bva + fsz:
            return off + (va - bva)
    return None


def _elf_sections(buf):
    e_shoff = struct.unpack_from("<I", buf, 0x20)[0]
    e_shnum = struct.unpack_from("<H", buf, 0x30)[0]
    e_shstrndx = struct.unpack_from("<H", buf, 0x32)[0]
    if not e_shoff or not e_shnum or e_shstrndx >= e_shnum:
        return {}
    str_off = struct.unpack_from("<I", buf, e_shoff + e_shstrndx * 40 + 16)[0]
    secs = {}
    for i in range(e_shnum):
        sh_name, sh_type, _, sh_addr, sh_off, sh_size = struct.unpack_from("<6I", buf, e_shoff + i * 40)
        end = buf.find(b"\0", str_off + sh_name)
        name = buf[str_off + sh_name:end].decode("ascii", "replace")
        secs[name] = (sh_type, sh_addr, sh_off, sh_size)
    return secs


def _read_var_exports(elf_path):
    buf = open(elf_path, "rb").read()
    secs = _elf_sections(buf)
    if ".lib.ent" not in secs:
        return {}
    esegs = _elf_segs(buf)
    _, _, off, size = secs[".lib.ent"]
    exports = {}
    pos = off
    while pos < off + size:
        libnamep, _, _, sl, nvar, nfunc, tbl = struct.unpack_from("<IHHBBHI", buf, pos)
        if libnamep and nvar:
            no = _elf_v2o(esegs, libnamep)
            libn = buf[no:buf.find(b"\0", no)].decode("ascii", "replace") if no is not None else ""
            to = _elf_v2o(esegs, tbl)
            if libn and to is not None:
                for vi in range(nvar):
                    nid = struct.unpack_from("<I", buf, to + (nfunc + vi) * 4)[0]
                    addr = struct.unpack_from("<I", buf, to + (nfunc + nvar + nfunc + vi) * 4)[0]
                    exports[(libn, nid)] = addr
        pos += max(sl, 4) * 4
    return exports


def resolve_vstub_imports(data, segs, export_elfs):
    if not export_elfs:
        return 0
    exports = {}
    for ep in export_elfs:
        exports.update(_read_var_exports(ep))
    secs = _elf_sections(data)
    if ".lib.stub" not in secs:
        return 0
    vsegs = _elf_segs(data) if segs else []
    if not vsegs:
        return 0
    _, _, off, size = secs[".lib.stub"]
    total = 0
    pos = off
    while pos < off + size:
        libnamep, _, _, sl, vsc, _, _, _ = struct.unpack_from("<IHHBBHII", data, pos)
        vstubp = u32(data, pos + 20) if sl >= 6 else 0
        if vsc and vstubp:
            no = _elf_v2o(vsegs, libnamep) if libnamep else None
            libn = data[no:data.find(b"\0", no)].decode("ascii", "replace") if no is not None else ""
            vso = _elf_v2o(vsegs, vstubp)
            if vso is None:
                fail("vstubp 0x%08X outside segments" % vstubp)
            for vi in range(vsc):
                vp = u32(data, vso + vi * 8)
                vnid = u32(data, vso + vi * 8 + 4)
                if (libn, vnid) not in exports:
                    fail("missing exported variable %s:0x%08X" % (libn, vnid))
                target = exports[(libn, vnid)]
                vpo = _elf_v2o(vsegs, vp)
                if vpo is None:
                    fail("vstub list 0x%08X outside segments" % vp)
                fixups = []
                cur = vpo
                while True:
                    w = u32(data, cur)
                    if w == 0:
                        break
                    typ = (w >> 26) & 0x3F
                    va = ((w & 0x03FFFFFF) << 2) & 0xFFFFFFFF
                    fo = _elf_v2o(vsegs, va)
                    if fo is None:
                        fail("vstub target 0x%08X outside segments" % va)
                    fixups.append((typ, va, fo, u32(data, fo)))
                    cur += 4
                hi_queue = []
                for idx, (typ, va, fo, orig_op) in enumerate(fixups):
                    if typ == R_MIPS_HI16:
                        hi_queue.append(idx)
                    elif typ == R_MIPS_LO16:
                        lo_s = struct.unpack("<h", struct.pack("<H", orig_op & 0xFFFF))[0]
                        full = (((orig_op & 0xFFFF) + target) & 0xFFFFFFFF)
                        for hi_i in hi_queue:
                            _, _, hfo, hop = fixups[hi_i]
                            addr = (((hop & 0xFFFF) << 16) + lo_s + target) & 0xFFFFFFFF
                            hi = ((addr + 0x8000) & 0xFFFFFFFF) >> 16
                            pu32(data, hfo, (hop & 0xFFFF0000) | (hi & 0xFFFF))
                        hi_queue.clear()
                        pu32(data, fo, (orig_op & 0xFFFF0000) | (full & 0xFFFF))
                    elif typ == R_MIPS_32:
                        pu32(data, fo, (orig_op + target) & 0xFFFFFFFF)
                    else:
                        fail("unsupported vstub relocation type %d at 0x%08X" % (typ, va))
                if hi_queue:
                    fail("unpaired R_MIPS_HI16 in vstub %s:0x%08X" % (libn, vnid))
                total += len(fixups)
        pos += max(sl, 5) * 4
    return total


def _parse_export_args(argv):
    clean = [argv[0]]
    i = 1
    while i < len(argv):
        if argv[i] == "--export-elf":
            if i + 1 >= len(argv):
                fail("--export-elf requires a path")
            _EXPORT_ELFS.append(argv[i + 1])
            i += 2
        elif argv[i].startswith("--export-elf="):
            _EXPORT_ELFS.append(argv[i].split("=", 1)[1])
            i += 1
        else:
            clean.append(argv[i])
            i += 1
    return clean


if __name__ == "__main__":
    sys.exit(main(_parse_export_args(sys.argv)))
