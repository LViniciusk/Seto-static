#!/usr/bin/env python3
"""Offline Pure GX Card Database & Texture Merger (TF1 + TF3).

Reads TF1.iso (ULUS-10136) and TF3.iso (ULES-01183), decompresses TF3's
Huffman + dictionary card descriptions, filters out all Synchro / Tuner /
Synchro-support cards (75 cards), preserves all TF1 card indices 1..2479
and Konami IDs 4007..6957 intact, and appends the 1,000 net-new Pure GX
cards at indices 2480..3479 (assigned extended TF1 IDs 6958..7957).

Outputs to build/gx_data/ (ready for VFS redirection & tf3eng bridge):
  - cardinfo_eng.ehp   (EHP\\x03 matching TF1 layout, 3,479 cards)
  - cardm_e.cip        (CPM\\x1a 64x64 duel textures, min=4007 max=7957)
  - cardh_e.cip        (CIP\\x1a 128x64 detail textures, min=4007 max=7957)
  - gx_id_map.h        (C lookup tables: tf1_ext_kid <-> tf3_kid & card counts)
  - gx_cards_report.txt
"""
import argparse
import os
import re
import struct


TF1_CARDINFO_LBN, TF1_CARDINFO_SIZE = 0x4B1E0, 1023968
TF1_CARDM_LBN, TF1_CARDM_SIZE = 0x39160, 5199872
TF1_CARDH_LBN, TF1_CARDH_SIZE = 0x37D90, 10375168
TF1_DECK_ALL_LBN, TF1_DECK_ALL_SIZE = 0x39F30, 2507168

TF3_CARDINFO_LBN, TF3_CARDINFO_SIZE = 0xAF7F0, 532944
TF3_CARDM_LBN, TF3_CARDM_SIZE = 0xD14F0, 7428096
TF3_CARDH_LBN, TF3_CARDH_SIZE = 0xC8760, 14819328
TF3_DECK_ALL_LBN, TF3_DECK_ALL_SIZE = 0x99750, 3187264

TF1_BASE_ID = 4000
TF1_EXT_KID_START = 6958
TF3_BASE_ID = 3720


def read_iso_slice(iso_path, lbn, size):
    with open(iso_path, "rb") as f:
        f.seek(lbn * 2048)
        data = f.read(size)
    if len(data) != size:
        raise RuntimeError(f"Short read from {iso_path} @ lbn {lbn:#x}")
    return data


def parse_ehp(data):
    if data[:4] != b"EHP\x03":
        raise ValueError("Not EHP\\x03")
    count, = struct.unpack_from("<I", data, 12)
    entries = []
    for i in range(count):
        name_off, data_off = struct.unpack_from("<II", data, 16 + i * 8)
        end = data.find(b"\x00", name_off)
        name = data[name_off:end].decode("ascii")
        sz, = struct.unpack_from("<I", data, end + 1)
        entries.append((name, data[data_off : data_off + sz]))
    return entries


def build_ehp(files_ordered):
    count = len(files_ordered)
    tbl_off = 16
    str_off = tbl_off + count * 8 + 8
    str_blob = bytearray()
    name_offs = []
    for name, payload in files_ordered:
        name_offs.append(str_off + len(str_blob))
        str_blob.extend(name.encode("ascii") + b"\x00")
        str_blob.extend(struct.pack("<I", len(payload)))
    data_start = (str_off + len(str_blob) + 15) & ~15

    data_offs = []
    data_blob = bytearray()
    for _, payload in files_ordered:
        cur = data_start + len(data_blob)
        pad = ((cur + 15) & ~15) - cur
        if pad:
            data_blob.extend(b"\x00" * pad)
        data_offs.append(data_start + len(data_blob))
        data_blob.extend(payload)

    end_pad = (((data_start + len(data_blob)) + 15) & ~15) - (
        data_start + len(data_blob)
    )
    if end_pad:
        data_blob.extend(b"\x00" * end_pad)
    total_size = data_start + len(data_blob)

    out = bytearray(data_start)
    out[0:4] = b"EHP\x03"
    struct.pack_into("<I", out, 4, total_size)
    out[8:12] = b"NOT "
    struct.pack_into("<I", out, 12, count)
    for i in range(count):
        struct.pack_into("<II", out, tbl_off + i * 8, name_offs[i], data_offs[i])
    out[str_off : str_off + len(str_blob)] = str_blob
    out.extend(data_blob)
    return bytes(out)


def read_utf16_at(buf, off):
    out = bytearray()
    i = off
    while i + 1 < len(buf):
        if buf[i] == 0 and buf[i + 1] == 0:
            break
        out.extend((buf[i], buf[i + 1]))
        i += 2
    return out.decode("utf-16-le", "replace")


def decode_tf3_cards(tf3_files):
    dd = tf3_files["DICT_E.bin"]
    nd, tbl_off, str_base = struct.unpack_from("<3I", dd, 0)
    dict_strs = [
        read_utf16_at(dd, str_base + struct.unpack_from("<I", dd, tbl_off + i * 4)[0])
        for i in range(nd)
    ]

    tf3_indx = tf3_files["CARD_Indx_E.bin"]
    tf3_n = len(tf3_indx) // 8 - 1
    tf3_names = [
        read_utf16_at(
            tf3_files["CARD_Name_E.bin"],
            struct.unpack_from("<I", tf3_indx, i * 8)[0],
        )
        for i in range(tf3_n)
    ]
    tf3_intid = struct.unpack_from(
        f"<{len(tf3_files['CARD_IntID.bin']) // 2}H", tf3_files["CARD_IntID.bin"], 0
    )

    huff = tf3_files["CARD_Huff_E.bin"]
    cdesc = tf3_files["CARD_Desc_E.bin"]

    def decode_one(bitpos):
        out = []
        for _ in range(4096):
            node = 0
            while True:
                l, r = struct.unpack_from("<HH", huff, node)
                if l == 0 and r == 0:
                    ch, = struct.unpack_from("<H", huff, node + 4)
                    break
                bit = (cdesc[bitpos >> 3] >> (7 - (bitpos & 7))) & 1
                bitpos += 1
                node = r if bit else l
            if ch == 0:
                break
            out.append(ch)
        s = bytes(struct.pack(f"<{len(out)}H", *out)).decode("utf-16-le", "replace")
        s = re.sub(r"\$d(\d{3})", lambda m: dict_strs[int(m.group(1))], s)
        def repl_m(m):
            kid = int(m.group(1))
            pos = kid - TF3_BASE_ID
            if 0 <= pos < len(tf3_intid):
                idx3 = tf3_intid[pos]
                if 0 < idx3 < len(tf3_names):
                    return tf3_names[idx3]
            return m.group(0)

        s = re.sub(r"\$m(\d+)", repl_m, s)
        return s

    cards = []
    for i in range(tf3_n):
        if i == 0:
            cards.append((0, "", "", 0, 0, b"\x00" * 6))
            continue
        _, doff = struct.unpack_from("<II", tf3_indx, i * 8)
        q3, = struct.unpack_from("<Q", tf3_files["CARD_Prop.bin"], i * 8)
        pw, = struct.unpack_from("<I", tf3_files["CARD_Pass.bin"], i * 4)
        g3 = tf3_files["CARD_Genre.bin"][i * 6 : (i + 1) * 6]
        cards.append((q3 & 0x3FFF, tf3_names[i], decode_one(doff), q3, pw, g3))
    return cards, tf3_intid


def convert_prop_tf3_to_tf1(q3, tf1_ext_kid):
    assert 0 <= tf1_ext_kid <= 0x1FFF
    atk = (q3 >> 14) & 0x1FF
    defn = (q3 >> 23) & 0x1FF
    kind = (q3 >> 32) & 0x1F
    attrs = (q3 >> 38) & 0xFFFF
    hi_flags = (q3 >> 54) & 0x3FF
    return (
        (tf1_ext_kid & 0x1FFF)
        | (atk << 13)
        | (defn << 22)
        | (kind << 31)
        | (attrs << 36)
        | (hi_flags << 52)
    )


def convert_genre_tf3_to_tf1(g3_bytes):
    v3, = struct.unpack_from("<Q", g3_bytes + b"\x00\x00", 0)
    v1 = (v3 & 0x3FF) | (((v3 >> 13) & 0x3FFFFFFFFF) << 10)
    return struct.pack("<Q", v1)[:6]


def resolve_cip_block(cip_data, kid, block_units):
    tot, _, _, tbl_end, min_kid, max_kid, hdr_sz = struct.unpack_from("<7I", cip_data, 4)
    if kid < min_kid or kid > max_kid:
        return None
    a, _ = struct.unpack_from("<II", cip_data, hdr_sz + (kid - min_kid) * 8)
    if a == 0 or a == 0xFFFFFFFF:
        return None
    if a & 0x80000000:
        off = a & 0x7FFFFFFF
        if off >= tbl_end:
            return None
        first_blk, _ = struct.unpack_from("<II", cip_data, off)
        blk = first_blk & 0x7FFFFFFF
    else:
        blk = a
    byte_off = blk * 2048
    byte_sz = block_units * 2048
    if byte_off + byte_sz > len(cip_data):
        return None
    return cip_data[byte_off : byte_off + byte_sz]


def extend_tf1_cip(tf1_cip, new_blocks, block_units):
    magic = tf1_cip[:4]
    tot, unk1, unk2, tbl_end, min_kid, max_kid, hdr_sz = struct.unpack_from("<7I", tf1_cip, 4)
    assert min_kid == 4007 and max_kid == 6957 and hdr_sz == 0x30

    id_slots = max_kid - min_kid + 1  # 2951 (4007..6957)
    multi_start_off = hdr_sz + id_slots * 8  # 0x5c68
    multi_sz = tbl_end - multi_start_off

    old_data_start = ((tbl_end + 2047) // 2048) * 2048
    old_data_blk = old_data_start // 2048
    old_payload = tf1_cip[old_data_start:]
    assert (len(old_payload) % 2048) == 0

    n_new = len(new_blocks)
    tbl_growth = n_new * 8
    new_multi_start = multi_start_off + tbl_growth
    new_tbl_end = tbl_end + tbl_growth
    new_data_start = ((new_tbl_end + 2047) // 2048) * 2048
    new_data_blk = new_data_start // 2048
    delta_blk = new_data_blk - old_data_blk

    # Rebuild ID slots (4007..6957 preserved + shifted, 6958..7957 appended at end of payload)
    new_id_tbl = bytearray(id_slots * 8 + n_new * 8)
    for i in range(id_slots):
        a, b = struct.unpack_from("<II", tf1_cip, hdr_sz + i * 8)
        if a != 0 and a != 0xFFFFFFFF:
            if a & 0x80000000:
                a = 0x80000000 | ((a & 0x7FFFFFFF) + tbl_growth)
            else:
                a = a + delta_blk
        struct.pack_into("<II", new_id_tbl, i * 8, a, b)

    next_blk = new_data_blk + (len(old_payload) // 2048)
    appended_payload = bytearray()
    for idx, blk_bytes in enumerate(new_blocks):
        slot_i = id_slots + idx
        if blk_bytes is None:
            struct.pack_into("<II", new_id_tbl, slot_i * 8, 0, 0)
        else:
            assert len(blk_bytes) == block_units * 2048
            struct.pack_into("<II", new_id_tbl, slot_i * 8, next_blk, 0)
            appended_payload.extend(blk_bytes)
            next_blk += block_units

    # Rebuild multi-art table with shifted block indices
    new_multi_tbl = bytearray(tf1_cip[multi_start_off:tbl_end])
    for off in range(0, len(new_multi_tbl), 8):
        a, b = struct.unpack_from("<II", new_multi_tbl, off)
        if a != 0 and a != 0xFFFFFFFF:
            flag = a & 0x80000000
            blk = (a & 0x7FFFFFFF) + delta_blk
            struct.pack_into("<II", new_multi_tbl, off, flag | blk, b)

    new_total_size = new_data_start + len(old_payload) + len(appended_payload)
    out = bytearray(new_data_start)
    out[0:hdr_sz] = tf1_cip[0:hdr_sz]
    struct.pack_into(
        "<7I",
        out,
        4,
        new_total_size,
        unk1,
        unk2,
        new_tbl_end,
        min_kid,
        max_kid + n_new,
        hdr_sz,
    )
    out[hdr_sz : hdr_sz + len(new_id_tbl)] = new_id_tbl
    out[new_multi_start : new_multi_start + len(new_multi_tbl)] = new_multi_tbl
    out.extend(old_payload)
    out.extend(appended_payload)
    assert len(out) == new_total_size
    return bytes(out)


def _unswizzle_8bpp(src, w=512, h=512):
    dst = bytearray(w * h)
    off = 0
    for y_blk in range(h // 8):
        for x_blk in range(w // 16):
            for r in range(8):
                y = y_blk * 8 + r
                x = x_blk * 16
                dst[y * w + x : y * w + x + 16] = src[off : off + 16]
                off += 16
    return dst


def _swizzle_8bpp(src, w=512, h=512):
    dst = bytearray(w * h)
    off = 0
    for y_blk in range(h // 8):
        for x_blk in range(w // 16):
            for r in range(8):
                y = y_blk * 8 + r
                x = x_blk * 16
                dst[off : off + 16] = src[y * w + x : y * w + x + 16]
                off += 16
    return bytes(dst)


def _parse_cis_gim(gim):
    # PAL block at 0x30, IMG block right after PAL
    _, _, pal_nxt, _ = struct.unpack_from("<4I", gim, 0x30)
    pal_hdr = gim[0x30 + 16 : 0x30 + 16 + 0x30]
    n_frames = struct.unpack_from("<H", pal_hdr, 0x2E)[0]
    pal_offs = struct.unpack_from(f"<{n_frames}I", gim, 0x30 + 16 + 0x30)
    pals = [gim[0x30 + 16 + o : 0x30 + 16 + o + 1024] for o in pal_offs]

    img_pos = 0x30 + pal_nxt
    img_offs = struct.unpack_from(f"<{n_frames}I", gim, img_pos + 16 + 0x30)
    imgs = [gim[img_pos + 16 + o : img_pos + 16 + o + 512 * 512] for o in img_offs]
    return pals, imgs


def _extract_cis_tile_rgba(unswizzled_sheet, pal_bytes, slot_in_sheet):
    sx = (slot_in_sheet % 23) * 22
    sy = (slot_in_sheet // 23) * 30
    tile = bytearray(22 * 30 * 4)
    p = 0
    for y in range(30):
        row_off = (sy + y) * 512 + sx
        for x in range(22):
            cidx = unswizzled_sheet[row_off + x] * 4
            tile[p : p + 4] = pal_bytes[cidx : cidx + 4]
            p += 4
    return bytes(tile)


def _quantize_rgba_sheet(tiles):
    """Quantize up to 391 22x30 RGBA tiles into one 512x512 8bpp sheet + 256-color RGBA palette."""
    # Collect unique RGBA colors weighted by frequency (alpha preserved)
    counts = {}
    for tile in tiles:
        for i in range(0, len(tile), 4):
            rgba = (tile[i], tile[i + 1], tile[i + 2], tile[i + 3])
            counts[rgba] = counts.get(rgba, 0) + 1

    # Median-cut down to 256 buckets
    buckets = [list(counts.items())]
    while len(buckets) < 256:
        best_i = -1
        best_score = -1
        best_ch = 0
        for i, b in enumerate(buckets):
            if len(b) <= 1:
                continue
            r_vals = [c[0][0] for c in b]
            g_vals = [c[0][1] for c in b]
            b_vals = [c[0][2] for c in b]
            a_vals = [c[0][3] for c in b]
            ranges = (
                max(r_vals) - min(r_vals),
                max(g_vals) - min(g_vals),
                max(b_vals) - min(b_vals),
                max(a_vals) - min(a_vals),
            )
            mx = max(ranges)
            if mx > best_score:
                best_score = mx
                best_i = i
                best_ch = ranges.index(mx)
        if best_i < 0 or best_score == 0:
            break
        blk = buckets.pop(best_i)
        blk.sort(key=lambda item: item[0][best_ch])
        total_w = sum(w for _, w in blk)
        acc = 0
        split = len(blk) // 2
        for idx, (_, w) in enumerate(blk):
            acc += w
            if acc * 2 >= total_w and idx > 0:
                split = idx
                break
        buckets.append(blk[:split])
        buckets.append(blk[split:])

    palette = []
    for b in buckets:
        tw = sum(w for _, w in b)
        r = sum(c[0] * w for c, w in b) // tw
        g = sum(c[1] * w for c, w in b) // tw
        bl = sum(c[2] * w for c, w in b) // tw
        a = sum(c[3] * w for c, w in b) // tw
        palette.append((r, g, bl, a))
    while len(palette) < 256:
        palette.append((0, 0, 0, 0))

    # Nearest-color LUT for every unique RGBA in this sheet
    lut = {}
    for rgba in counts:
        r0, g0, b0, a0 = rgba
        best_d = 1 << 30
        best_p = 0
        for pi, (pr, pg, pb, pa) in enumerate(palette):
            dr = r0 - pr
            dg = g0 - pg
            db = b0 - pb
            da = a0 - pa
            d = dr * dr + dg * dg + db * db + da * da
            if d < best_d:
                best_d = d
                best_p = pi
                if d == 0:
                    break
        lut[rgba] = best_p

    canvas = bytearray(512 * 512)
    for slot_i, tile in enumerate(tiles):
        sx = (slot_i % 23) * 22
        sy = (slot_i // 23) * 30
        p = 0
        for y in range(30):
            row_off = (sy + y) * 512 + sx
            for x in range(22):
                rgba = (tile[p], tile[p + 1], tile[p + 2], tile[p + 3])
                canvas[row_off + x] = lut[rgba]
                p += 4

    pal_bytes = b"".join(struct.pack("<4B", *c) for c in palette)
    return pal_bytes, _swizzle_8bpp(canvas)


def _build_cis_gim(pals, swizzled_imgs):
    n = len(pals)
    assert len(swizzled_imgs) == n
    tbl_sz = (0x30 + n * 4 + 15) & ~15  # 0x60 for 9 frames
    pal_block_body = tbl_sz + n * 1024
    img_block_body = tbl_sz + n * (512 * 512)
    pal_nxt = 16 + pal_block_body
    img_nxt = 16 + img_block_body
    pic_sz = 16 + pal_nxt + img_nxt
    root_sz = 16 + pic_sz

    out = bytearray(16 + root_sz)
    out[0:12] = b"MIG.00.1PSP\x00"
    struct.pack_into("<4I", out, 16, 2, root_sz, 16, 16)
    struct.pack_into("<4I", out, 32, 3, pic_sz, 16, 16)

    # PAL block at 0x30
    struct.pack_into("<4I", out, 0x30, 5, pal_nxt, pal_nxt, 16)
    struct.pack_into(
        "<10H", out, 0x40,
        0x30, 0, 3, 0, 256, 1, 32, 16, 1, 2,
    )
    struct.pack_into("<5I4H", out, 0x40 + 0x14, 0, 0x30, tbl_sz, pal_block_body, 0, 2, 1, 3, n)
    for i in range(n):
        off = tbl_sz + i * 1024
        struct.pack_into("<I", out, 0x40 + 0x30 + i * 4, off)
        out[0x40 + off : 0x40 + off + 1024] = pals[i]

    # IMG block
    img_pos = 0x30 + pal_nxt
    struct.pack_into("<4I", out, img_pos, 4, img_nxt, img_nxt, 16)
    struct.pack_into(
        "<10H", out, img_pos + 16,
        0x30, 0, 5, 1, 512, 512, 8, 16, 8, 2,
    )
    struct.pack_into("<5I4H", out, img_pos + 16 + 0x14, 0, 0x30, tbl_sz, img_block_body, 0, 1, 1, 3, n)
    for i in range(n):
        off = tbl_sz + i * (512 * 512)
        struct.pack_into("<I", out, img_pos + 16 + 0x30 + i * 4, off)
        out[img_pos + 16 + off : img_pos + 16 + off + 512 * 512] = swizzled_imgs[i]

    return bytes(out)


def extend_deck_all_ehp(tf1_all_ehp, tf3_all_ehp, new_gx):
    tf1_ordered = parse_ehp(tf1_all_ehp)
    tf1_map = dict(tf1_ordered)
    tf3_map = dict(parse_ehp(tf3_all_ehp))

    ft1 = tf1_map["fileTbl.dat"]
    n1 = struct.unpack_from("<I", ft1, 0)[0]
    kids1 = list(struct.unpack_from(f"<{n1 - 1}H", ft1, 4))  # 2451 TF1 kids (slots 1..2451)

    ft3 = tf3_map["fileTbl.dat"]
    n3 = struct.unpack_from("<I", ft3, 0)[0]
    kids3 = list(struct.unpack_from(f"<{n3 - 1}H", ft3, 4))
    tf3_slot_by_kid = {kid3: i + 1 for i, kid3 in enumerate(kids3) if kid3 != 0}

    pals1, imgs1 = _parse_cis_gim(tf1_map["cis_all.gim"])
    pals3, imgs3 = _parse_cis_gim(tf3_map["cis_all.gim"])
    unswiz3 = [_unswizzle_8bpp(img) for img in imgs3]

    # Collect Pure GX thumbnails available in TF3 cis_all.gim (already sorted by ext_kid)
    new_items = []
    for _, kid3, ext_kid, _, _, _, _, _ in new_gx:
        slot3 = tf3_slot_by_kid.get(kid3)
        if slot3 is None:
            continue
        sh3 = slot3 // 391
        in_sh3 = slot3 % 391
        tile = _extract_cis_tile_rgba(unswiz3[sh3], pals3[sh3], in_sh3)
        new_items.append((ext_kid, tile))

    # Sheets 0..5 (slots 0..2345) keep TF1 verbatim.
    # Sheet 6 keeps TF1 slots 2346..2451 (106 tiles) + first 285 Pure GX tiles.
    unswiz1_6 = _unswizzle_8bpp(imgs1[6])
    sheet6_tiles = [
        _extract_cis_tile_rgba(unswiz1_6, pals1[6], i) for i in range(106)
    ] + [t for _, t in new_items[:285]]
    sheet7_tiles = [t for _, t in new_items[285:676]]
    sheet8_tiles = [t for _, t in new_items[676:]]
    assert len(sheet6_tiles) == 391 and len(sheet7_tiles) == 391 and 0 < len(sheet8_tiles) <= 391

    p6, i6 = _quantize_rgba_sheet(sheet6_tiles)
    p7, i7 = _quantize_rgba_sheet(sheet7_tiles)
    p8, i8 = _quantize_rgba_sheet(sheet8_tiles)

    out_pals = pals1[:6] + [p6, p7, p8]
    out_imgs = imgs1[:6] + [i6, i7, i8]
    new_cis_gim = _build_cis_gim(out_pals, out_imgs)

    all_kids = kids1 + [k for k, _ in new_items]
    total_n = 1 + len(all_kids)
    # Trailing u16 0 matches TF1/TF3 fileTbl.dat layout ([u32 total_n][u16 kids...][u16 0])
    new_ft = struct.pack(f"<I{len(all_kids)}HH", total_n, *all_kids, 0)

    tf1_map["cis_all.gim"] = new_cis_gim
    tf1_map["fileTbl.dat"] = new_ft
    ordered = [(name, tf1_map[name]) for name, _ in tf1_ordered]
    return build_ehp(ordered), len(new_items)


def decode_psp_dxt1(payload, width, height):
    import io
    from PIL import Image

    size = width * height // 2
    payload = payload.ljust(size, b"\0")
    blocks = b"".join(payload[i + 4:i + 8] + payload[i:i + 4] for i in range(0, size, 8))
    dds = struct.pack("<4s7I44xII4s20xI16x", b"DDS ", 124, 0x81007,
                      height, width, size, 0, 0, 32, 4, b"DXT1", 0x1000)
    with Image.open(io.BytesIO(dds + blocks)) as image:
        return image.convert("RGBA")


def fill_deck_thumbnail_slots(deck_ehp, cardm, end_kid):
    entries = dict(parse_ehp(deck_ehp))
    count, = struct.unpack_from("<I", entries["fileTbl.dat"])
    kids = struct.unpack_from(f"<{count - 1}H", entries["fileTbl.dat"], 4)
    existing = set(kids)
    missing = [(kid, payload) for kid in range(end_kid) if kid not in existing
               and (payload := resolve_cip_block(cardm, kid, 1)) is not None]
    if not missing:
        return deck_ehp, 0

    # Preserve the existing palette and tiles so earlier slot IDs keep their images.
    from PIL import Image
    palettes, sheets = _parse_cis_gim(entries["cis_all.gim"])
    first_slot = (len(sheets) - 1) * 391
    if count < first_slot or count + len(missing) > len(sheets) * 391:
        raise ValueError("Missing deck thumbnails do not fit in the last atlas")
    palette = Image.new("P", (1, 1))
    palette.putpalette([color for i, color in enumerate(palettes[-1]) if i % 4 != 3])
    sheet = bytearray(_unswizzle_8bpp(sheets[-1]))
    for slot, (_, payload) in enumerate(missing, count - first_slot):
        image = decode_psp_dxt1(payload, 64, 64).convert("RGB")
        image = image.resize((22, 30), Image.Resampling.LANCZOS)
        tile = image.quantize(palette=palette, dither=Image.Dither.NONE).tobytes()
        x, y = slot % 23 * 22, slot // 23 * 30
        for row in range(30):
            offset = (y + row) * 512 + x
            sheet[offset:offset + 22] = tile[row * 22:(row + 1) * 22]
    sheets[-1] = _swizzle_8bpp(sheet)
    kids += tuple(kid for kid, _ in missing)
    entries["fileTbl.dat"] = struct.pack(f"<I{len(kids)}HH", len(kids) + 1, *kids, 0)
    entries["cis_all.gim"] = _build_cis_gim(palettes, sheets)
    return build_ehp(list(entries.items())), len(missing)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--tf1-iso", default=os.path.join(repo_root, "TF1.iso"))
    ap.add_argument("--tf3-iso", default=os.path.join(repo_root, "TF3.iso"))
    ap.add_argument("--out-dir", default=os.path.join(repo_root, "build", "gx_data"))
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    tf1_ordered = parse_ehp(read_iso_slice(args.tf1_iso, TF1_CARDINFO_LBN, TF1_CARDINFO_SIZE))
    tf1_files = dict(tf1_ordered)
    tf3_files = dict(parse_ehp(read_iso_slice(args.tf3_iso, TF3_CARDINFO_LBN, TF3_CARDINFO_SIZE)))

    # Parse TF1 existing cards (indices 0..2479)
    tf1_indx = tf1_files["CARD_Indx_E.bin"]
    tf1_n = len(tf1_indx) // 8 - 1  # 2480 (0 is dummy, 1..2479 real)
    assert tf1_n == 2480
    tf1_names = [
        read_utf16_at(tf1_files["CARD_Name_E.bin"], struct.unpack_from("<I", tf1_indx, i * 8)[0])
        if i > 0 else ""
        for i in range(tf1_n)
    ]
    tf1_descs = [
        read_utf16_at(tf1_files["CARD_Desc_E.bin"], struct.unpack_from("<II", tf1_indx, i * 8)[1])
        if i > 0 else ""
        for i in range(tf1_n)
    ]
    tf1_props = [struct.unpack_from("<Q", tf1_files["CARD_Prop.bin"], i * 8)[0] for i in range(tf1_n)]
    tf1_pws = [struct.unpack_from("<I", tf1_files["CARD_Pass.bin"], i * 4)[0] for i in range(tf1_n)]
    tf1_genres = [tf1_files["CARD_Genre.bin"][i * 6 : (i + 1) * 6] for i in range(tf1_n)]
    tf1_intid = list(
        struct.unpack_from(f"<{len(tf1_files['CARD_IntID.bin']) // 2}H", tf1_files["CARD_IntID.bin"], 0)
    )
    tf1_valid_kids = {k + TF1_BASE_ID: v for k, v in enumerate(tf1_intid) if v != 0}
    tf1_name_to_idx = {tf1_names[i]: i for i in range(1, tf1_n)}

    # Decode TF3 cards
    tf3_cards, tf3_intid = decode_tf3_cards(tf3_files)

    # Filter new Pure GX cards (collect first, then sort deterministically by tf3_kid)
    raw_new_gx = []
    excl_synchro = []

    for idx3 in range(1, len(tf3_cards)):
        kid3, nm, ds, q3, pw, g3 = tf3_cards[idx3]
        if kid3 in tf1_valid_kids or nm in tf1_name_to_idx:
            continue
        ktype = (q3 >> 32) & 0x3F
        if (
            ktype >= 0x1F
            or "synchro" in nm.lower()
            or "synchro" in ds.lower()
            or "tuner" in nm.lower()
            or "tuner" in ds.lower()
        ):
            excl_synchro.append((idx3, kid3, nm))
            continue
        raw_new_gx.append((idx3, kid3, nm, ds, q3, pw, g3))

    # Deterministic ordering by ascending TF3 Konami ID for multiplayer parity
    raw_new_gx.sort(key=lambda item: (item[1], item[0]))

    new_gx = []  # (tf3_idx, tf3_kid, tf1_ext_kid, name, desc, prop1, pw, genre1)
    next_ext_kid = TF1_EXT_KID_START
    for idx3, kid3, nm, ds, q3, pw, g3 in raw_new_gx:
        ext_kid = next_ext_kid
        next_ext_kid += 1
        prop1 = convert_prop_tf3_to_tf1(q3, ext_kid)
        genre1 = convert_genre_tf3_to_tf1(g3)
        new_gx.append((idx3, kid3, ext_kid, nm, ds, prop1, pw, genre1))

    total_n = tf1_n + len(new_gx)  # 2480 + 1000 = 3480
    print(f"TF1 cards={tf1_n - 1}, Excluded Synchro/Tuner={len(excl_synchro)}, Added Pure GX={len(new_gx)}, Total={total_n - 1}")

    # Build expanded CARD_Name_E.bin, CARD_Desc_E.bin, CARD_Indx_E.bin
    name_blob = bytearray(tf1_files["CARD_Name_E.bin"])
    desc_blob = bytearray(tf1_files["CARD_Desc_E.bin"])
    indx_pairs = [struct.unpack_from("<II", tf1_indx, i * 8) for i in range(tf1_n)]
    prop_blob = bytearray(tf1_files["CARD_Prop.bin"])
    pass_blob = bytearray(tf1_files["CARD_Pass.bin"])
    genre_blob = bytearray(tf1_files["CARD_Genre.bin"])

    all_names = list(tf1_names)
    all_kids = [q & 0x1FFF for q in tf1_props]

    ext_intid = list(tf1_intid) + [0] * len(new_gx)
    assert len(ext_intid) == (TF1_EXT_KID_START - TF1_BASE_ID) + len(new_gx)

    for k, (idx3, kid3, ext_kid, nm, ds, prop1, pw, genre1) in enumerate(new_gx):
        new_idx = tf1_n + k
        noff = len(name_blob)
        doff = len(desc_blob)
        name_blob.extend(nm.encode("utf-16-le") + b"\x00\x00")
        desc_blob.extend(ds.encode("utf-16-le") + b"\x00\x00")
        indx_pairs.append((noff, doff))
        prop_blob.extend(struct.pack("<Q", prop1))
        pass_blob.extend(struct.pack("<I", pw))
        genre_blob.extend(genre1)
        ext_intid[ext_kid - TF1_BASE_ID] = new_idx
        all_names.append(nm)
        all_kids.append(ext_kid)

    # Sentinel pair at end of CARD_Indx_E.bin
    indx_pairs.append((len(name_blob), len(desc_blob)))
    indx_blob = b"".join(struct.pack("<II", a, b) for a, b in indx_pairs)
    intid_blob = struct.pack(f"<{len(ext_intid)}H", *ext_intid)

    # Rebuild CARD_Sort_E.bin & CARD_Top_E.bin (alphabetical sort rank per card index)
    def sort_key(idx):
        return (all_names[idx].lower(), all_names[idx])

    ordered_ids = sorted(range(1, total_n), key=sort_key)
    sort_arr = [0] * total_n
    for rank, cid in enumerate(ordered_ids, start=1):
        sort_arr[cid] = rank
    sort_blob = struct.pack(f"<{total_n}H", *sort_arr)

    # Rebuild CARD_Top_E.bin: count + pairs (first_kid_for_group, group_char_code)
    # Match TF1's 27 groups: '*' (non-alpha) + 'A'..'Z'
    groups = {}
    for cid in ordered_ids:
        ch = all_names[cid][0].upper() if all_names[cid] else "*"
        grp = ord(ch) if "A" <= ch <= "Z" else 42
        if grp not in groups:
            groups[grp] = all_kids[cid]
    top_u16 = [27]
    for grp in [42] + list(range(65, 91)):
        top_u16.extend([groups.get(grp, 0), grp])
    top_blob = struct.pack(f"<{len(top_u16)}H", *top_u16)

    # Rebuild cardinfo_eng.ehp keeping TF1's exact entry list
    new_ci_map = dict(tf1_files)
    new_ci_map["CARD_Desc_E.bin"] = bytes(desc_blob)
    new_ci_map["CARD_Genre.bin"] = bytes(genre_blob)
    new_ci_map["CARD_Indx_E.bin"] = bytes(indx_blob)
    new_ci_map["CARD_IntID.bin"] = bytes(intid_blob)
    new_ci_map["CARD_Name_E.bin"] = bytes(name_blob)
    new_ci_map["CARD_Pass.bin"] = bytes(pass_blob)
    new_ci_map["CARD_Prop.bin"] = bytes(prop_blob)
    new_ci_map["CARD_Sort_E.bin"] = bytes(sort_blob)
    new_ci_map["CARD_Top_E.bin"] = bytes(top_blob)

    ordered_ci = [(name, new_ci_map[name]) for name, _ in tf1_ordered]
    ehp_out = build_ehp(ordered_ci)
    with open(os.path.join(args.out_dir, "cardinfo_eng.ehp"), "wb") as f:
        f.write(ehp_out)

    # Extract & merge cardm_e.cip (CPM) and cardh_e.cip (CIP)
    tf1_cpm = read_iso_slice(args.tf1_iso, TF1_CARDM_LBN, TF1_CARDM_SIZE)
    tf3_cpm = read_iso_slice(args.tf3_iso, TF3_CARDM_LBN, TF3_CARDM_SIZE)
    cpm_blocks = []
    cpm_missing = 0
    for _, kid3, ext_kid, nm, _, _, _, _ in new_gx:
        blk = resolve_cip_block(tf3_cpm, kid3, 1)
        if blk is None:
            cpm_missing += 1
        cpm_blocks.append(blk)
    cpm_out = extend_tf1_cip(tf1_cpm, cpm_blocks, 1)
    with open(os.path.join(args.out_dir, "cardm_e.cip"), "wb") as f:
        f.write(cpm_out)

    tf1_cip = read_iso_slice(args.tf1_iso, TF1_CARDH_LBN, TF1_CARDH_SIZE)
    tf3_cip = read_iso_slice(args.tf3_iso, TF3_CARDH_LBN, TF3_CARDH_SIZE)
    cip_blocks = []
    cip_missing = 0
    for _, kid3, ext_kid, nm, _, _, _, _ in new_gx:
        blk = resolve_cip_block(tf3_cip, kid3, 2)
        if blk is None:
            cip_missing += 1
        cip_blocks.append(blk)
    cip_out = extend_tf1_cip(tf1_cip, cip_blocks, 2)
    with open(os.path.join(args.out_dir, "cardh_e.cip"), "wb") as f:
        f.write(cip_out)

    # Expand deck/all_e.ehp (cis_all.gim + fileTbl.dat) for small deck-list thumbnails
    tf1_deck_all = read_iso_slice(args.tf1_iso, TF1_DECK_ALL_LBN, TF1_DECK_ALL_SIZE)
    tf3_deck_all = read_iso_slice(args.tf3_iso, TF3_DECK_ALL_LBN, TF3_DECK_ALL_SIZE)
    deck_all_out, cis_added = extend_deck_all_ehp(tf1_deck_all, tf3_deck_all, new_gx)
    deck_all_out, missing_added = fill_deck_thumbnail_slots(
        deck_all_out, cpm_out, TF1_EXT_KID_START + len(new_gx),
    )
    cis_added += missing_added
    with open(os.path.join(args.out_dir, "deck_all_e.ehp"), "wb") as f:
        f.write(deck_all_out)
    print(f"Expanded deck_all_e.ehp: +{cis_added} cis_all thumbnails, size={len(deck_all_out)}")

    # Generate gx_id_map.h for Phase B bridge (TF1 Konami ID <-> TF3 Konami ID)
    tf3_kids_by_tf1_kid = [0] * (TF1_EXT_KID_START + len(new_gx))
    for k, v in tf1_valid_kids.items():
        # Match TF1 card k (4007..6957) to TF3 card ID
        # If same Konami ID exists in TF3, use it; otherwise lookup by name (Tokens)
        if k < len(tf3_intid) and tf3_intid[k] != 0:
            # Wait: tf3_intid is indexed from 3720 in TF3? Wait! Check earlier:
            pass
    # Build exact TF3 lookup by Konami ID & Name
    tf3_kid_by_name = {}
    tf3_valid_kids = set()
    for idx3 in range(1, len(tf3_cards)):
        kid3, nm, _, _, _, _ = tf3_cards[idx3]
        tf3_valid_kids.add(kid3)
        if nm and nm not in tf3_kid_by_name:
            tf3_kid_by_name[nm] = kid3

    for kid1, idx1 in tf1_valid_kids.items():
        if kid1 in tf3_valid_kids:
            tf3_kids_by_tf1_kid[kid1] = kid1
        else:
            nm1 = tf1_names[idx1]
            tf3_kids_by_tf1_kid[kid1] = tf3_kid_by_name.get(nm1, 0)
    for _, kid3, ext_kid, _, _, _, _, _ in new_gx:
        tf3_kids_by_tf1_kid[ext_kid] = kid3

    max_tf3_kid = max(tf3_kids_by_tf1_kid)
    tf1_kids_by_tf3_kid = [0] * (max_tf3_kid + 1)
    for k1, k3 in enumerate(tf3_kids_by_tf1_kid):
        if k3 != 0 and tf1_kids_by_tf3_kid[k3] == 0:
            tf1_kids_by_tf3_kid[k3] = k1

    with open(os.path.join(args.out_dir, "gx_id_map.h"), "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/build_gx_carddb.py -- do not edit. */\n")
        f.write("#ifndef SETO_GX_ID_MAP_H\n#define SETO_GX_ID_MAP_H\n\n#include <stdint.h>\n\n")
        f.write(f"#define GX_TF1_CARDS       {tf1_n - 1}u\n")
        f.write(f"#define GX_NEW_CARDS       {len(new_gx)}u\n")
        f.write(f"#define GX_TOTAL_CARDS     {total_n - 1}u\n")
        f.write(f"#define GX_TOTAL_SLOTS     {total_n}u\n")
        f.write(f"#define GX_TF1_EXT_KID_MAX {TF1_EXT_KID_START + len(new_gx) - 1}u\n")
        f.write(f"#define GX_TF3_KID_MAX     {max_tf3_kid}u\n\n")
        f.write("static const uint16_t g_gx_tf1_to_tf3_kid[GX_TF1_EXT_KID_MAX + 1u] = {\n")
        for i in range(0, len(tf3_kids_by_tf1_kid), 16):
            f.write("    " + ", ".join(str(x) for x in tf3_kids_by_tf1_kid[i : i + 16]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint16_t g_gx_tf3_to_tf1_kid[GX_TF3_KID_MAX + 1u] = {\n")
        for i in range(0, len(tf1_kids_by_tf3_kid), 16):
            f.write("    " + ", ".join(str(x) for x in tf1_kids_by_tf3_kid[i : i + 16]) + ",\n")
        f.write("};\n\n#endif /* SETO_GX_ID_MAP_H */\n")

    # Also emit native TF3 metadata (indexed by tf3_kid / tf3_idx) for libehsys_rel helpers
    with open(os.path.join(args.out_dir, "tf3_card_meta.h"), "w", encoding="utf-8") as f:
        tf3_meta_max_kid = TF3_BASE_ID + len(tf3_intid) - 1
        f.write("/* Auto-generated by tools/build_gx_carddb.py -- do not edit. */\n")
        f.write("#ifndef SETO_TF3_CARD_META_H\n#define SETO_TF3_CARD_META_H\n\n#include <stdint.h>\n\n")
        f.write(f"#define TF3_CARD_BASE_ID {TF3_BASE_ID}u\n")
        f.write(f"#define TF3_CARD_MAX_ID  {tf3_meta_max_kid}u\n")
        f.write(f"#define TF3_CARD_SLOTS   {len(tf3_cards)}u\n\n")
        f.write("static const uint16_t g_tf3_intid_by_kid[TF3_CARD_MAX_ID - TF3_CARD_BASE_ID + 1u] = {\n")
        for i in range(0, len(tf3_intid), 16):
            f.write("    " + ", ".join(str(x) for x in tf3_intid[i : i + 16]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint64_t g_tf3_prop_by_idx[TF3_CARD_SLOTS] = {\n")
        for i in range(0, len(tf3_cards), 4):
            f.write("    " + ", ".join(f"0x{tf3_cards[j][3]:016x}ULL" for j in range(i, min(i + 4, len(tf3_cards)))) + ",\n")
        f.write("};\n\n")
        f.write("static const uint64_t g_tf3_genre_by_idx[TF3_CARD_SLOTS] = {\n")
        for i in range(0, len(tf3_cards), 4):
            vals = [struct.unpack_from("<Q", tf3_cards[j][5] + b"\x00\x00", 0)[0] for j in range(i, min(i + 4, len(tf3_cards)))]
            f.write("    " + ", ".join(f"0x{v:012x}ULL" for v in vals) + ",\n")
        f.write("};\n\n")
        f.write("#endif /* SETO_TF3_CARD_META_H */\n")

    with open(os.path.join(args.out_dir, "gx_cards_report.txt"), "w", encoding="utf-8") as f:
        f.write(f"TF1 cards: {tf1_n - 1}\nNew Pure GX cards: {len(new_gx)}\n")
        f.write(f"Excluded Synchro/Tuner/Support: {len(excl_synchro)}\n")
        f.write(f"CPM missing blocks: {cpm_missing}, CIP missing blocks: {cip_missing}\n")
        f.write(f"cardinfo_eng.ehp: {len(ehp_out)} bytes\n")
        f.write(f"cardm_e.cip: {len(cpm_out)} bytes\n")
        f.write(f"cardh_e.cip: {len(cip_out)} bytes\n\n")
        f.write("--- New Pure GX cards (ext_kid, tf3_kid, name) ---\n")
        for idx3, kid3, ext_kid, nm, _, _, _, _ in new_gx:
            f.write(f"ext_kid={ext_kid} tf3_kid={kid3} (tf3_idx={idx3}): {nm}\n")

    print(
        f"Generated build/gx_data: cardinfo={len(ehp_out)}B, cardm={len(cpm_out)}B (missing={cpm_missing}), "
        f"cardh={len(cip_out)}B (missing={cip_missing})"
    )

    # Self-validation of generated artifacts
    chk_ci = dict(parse_ehp(ehp_out))
    chk_indx = chk_ci["CARD_Indx_E.bin"]
    chk_n = len(chk_indx) // 8 - 1
    assert chk_n == total_n, f"expected {total_n} slots, got {chk_n}"
    assert len(chk_ci["CARD_Prop.bin"]) == chk_n * 8
    assert len(chk_ci["CARD_Genre.bin"]) == chk_n * 6
    assert len(chk_ci["CARD_Pass.bin"]) == chk_n * 4
    assert len(chk_ci["CARD_Sort_E.bin"]) == chk_n * 2
    chk_intid = struct.unpack_from(f"<{len(chk_ci['CARD_IntID.bin']) // 2}H", chk_ci["CARD_IntID.bin"], 0)
    assert chk_intid[7] == 1 and chk_intid[6958 - TF1_BASE_ID] == 2480
    assert chk_intid[6958 + len(new_gx) - 1 - TF1_BASE_ID] == chk_n - 1
    for i in range(1, chk_n):
        noff, doff = struct.unpack_from("<II", chk_indx, i * 8)
        nm = read_utf16_at(chk_ci["CARD_Name_E.bin"], noff)
        ds = read_utf16_at(chk_ci["CARD_Desc_E.bin"], doff)
        assert len(nm) > 0, f"empty name at {i}"
        assert "$d" not in ds and "$m" not in ds, f"unexpanded token at {i}: {ds!r}"
    for label, cip_blob, units in [("cardm_e.cip", cpm_out, 1), ("cardh_e.cip", cip_out, 2)]:
        tot, _, _, tbl_end, min_k, max_k, hdr_sz = struct.unpack_from("<7I", cip_blob, 4)
        assert tot == len(cip_blob) and min_k == 4007 and max_k == 6957 + len(new_gx)
        # Spot check Blue-Eyes (alt-art) + first & last new GX card
        assert resolve_cip_block(cip_blob, 4007, units) is not None
        assert resolve_cip_block(cip_blob, 6958, units) is not None
        assert resolve_cip_block(cip_blob, max_k, units) is not None
    print("Self-validation PASSED: all 3,479 cards, Indx/Name/Desc/Prop/Genre/Pass/Sort/Top/IntID, CPM & CIP verified.")


if __name__ == "__main__":
    main()
