#!/usr/bin/env python3
"""Add Pure GX card art to an extracted MikeyMK TF1 texture pack."""

import argparse
import ctypes
import re
import struct
from pathlib import Path

from PIL import Image

from adapt_tf3_texture_pack import quick_hash, read_hashes
from build_gx_carddb import (
    TF3_CARDM_LBN,
    TF3_CARDM_SIZE,
    TF1_EXT_KID_START,
    _parse_cis_gim,
    _unswizzle_8bpp,
    decode_psp_dxt1,
    parse_ehp,
    read_iso_slice,
    resolve_cip_block,
)


def deck_atlas_key(palette, sheet, xxh32, xxh64):
    clut_key = xxh32(palette, len(palette), 0xC0108888) ^ 0xC500FF03 ^ 0x0909
    data_hash = xxh64(sheet, len(sheet), 0xBACD7814) & 0xFFFFFFFF
    return clut_key << 32 | data_hash


def add_deck_sheets(pack, deck_ehp, base_hashes, card_paths, xxh32, xxh64,
                    small_cip, base_pack=None):
    deck = dict(parse_ehp(deck_ehp))
    palettes, sheets = _parse_cis_gim(deck["cis_all.gim"])
    file_table = deck["fileTbl.dat"]
    slots, = struct.unpack_from("<I", file_table)
    kids = (0, *struct.unpack_from(f"<{slots - 1}H", file_table, 4))
    first_extended = next(i for i, kid in enumerate(kids) if kid >= TF1_EXT_KID_START)
    # The editor selects 23 by 17 tiles of 22 by 30 pixels in each 512px atlas.
    columns, slots_per_sheet, scale = 23, 23 * 17, 4
    tile_width, tile_height = 22 * scale, 30 * scale
    base_sheets = {}
    for path in base_hashes.values():
        match = re.fullmatch(r"cards/tiny[^/]+/(\d+)_\d+\.png", path, re.I)
        if match:
            base_sheets[int(match[1])] = path

    aliases = {}
    tcg = base_pack is not None
    base_pack = base_pack or pack
    hd_tiles = missing_tiles = 0
    for sheet_i, (palette, sheet) in enumerate(zip(palettes, sheets)):
        sheet_kids = kids[sheet_i * slots_per_sheet:(sheet_i + 1) * slots_per_sheet]
        if not tcg and not any(kid >= TF1_EXT_KID_START for kid in sheet_kids):
            continue

        # Match a 512x512 T8 atlas with an RGBA8888 CLUT and a full 8-bit index.
        key = deck_atlas_key(palette, sheet, xxh32, xxh64)

        colors = [palette[i:i + 4] for i in range(0, len(palette), 4)]
        rgba = b"".join(colors[index] for index in _unswizzle_8bpp(sheet))
        atlas = Image.frombytes("RGBA", (512, 512), rgba).resize(
            (512 * scale, 512 * scale), Image.Resampling.NEAREST
        )
        base_path = base_hashes.get(key) or base_sheets.get(sheet_kids[0])
        base_atlas = None
        if base_path and (base_pack / base_path).is_file():
            with Image.open(base_pack / base_path) as source:
                base_atlas = source.convert("RGBA").resize(atlas.size, Image.Resampling.LANCZOS)

        for slot, kid in enumerate(sheet_kids):
            x = (slot % columns) * tile_width
            y = (slot // columns) * tile_height
            card_path = card_paths.get(kid)
            source_pack = pack
            if not card_path and sheet_i * slots_per_sheet + slot >= first_extended:
                if payload := resolve_cip_block(small_cip, kid, 1):
                    key_small = 0x0606 << 32 | (xxh64(payload, len(payload), 0xBACD7814) & 0xFFFFFFFF)
                    card_path = base_hashes.get(key_small)
                    source_pack = base_pack
            if not card_path and (tcg or kid < TF1_EXT_KID_START):
                if base_atlas:
                    atlas.paste(base_atlas.crop((x, y, x + tile_width, y + tile_height)), (x, y))
                continue
            if not card_path:
                missing_tiles += 1
                continue
            with Image.open(source_pack / card_path) as source:
                tile = source.convert("RGBA").resize((tile_width, tile_height), Image.Resampling.LANCZOS)
                atlas.paste(tile, (x, y))
            hd_tiles += 1

        folder = "moki-tcg/deck" if tcg else "tf3-extra/deck"
        path = f"{folder}/sheet-{sheet_i}.png"
        target = pack / path
        target.parent.mkdir(parents=True, exist_ok=True)
        atlas.save(target)
        aliases[key] = path
    print(f"Deck editor: {len(aliases)} atlases, {hd_tiles} HD Pure GX tiles, {missing_tiles} original tiles.")
    return aliases


def moki_texture(source, large_payload):
    with Image.open(source) as card:
        if card.size != (813, 1185):
            raise ValueError(f"Unexpected Moki card layout: {source}")
        if large_payload is None:
            # Keep thumbnails inside the renderer's idle cache size limit.
            return card.convert("RGBA").resize((421, 421), Image.Resampling.LANCZOS)
        art = card.crop((100, 219, 715, 834)).convert("RGBA")

    # The stock large texture carries text and stats alongside three art tiles.
    # Decode PSP DXT1 by moving each block's color endpoints before its indices.
    if len(large_payload) != 4096 - 128:
        raise ValueError(f"Unexpected large texture payload for {source}")
    scale = 8
    native = decode_psp_dxt1(large_payload, 128, 64)
    result = native.resize((128 * scale, 64 * scale), Image.Resampling.NEAREST)
    art = art.resize((78 * scale, 78 * scale), Image.Resampling.LANCZOS)
    for box, position in (
        ((0, 0, 78, 60), (0, 0)),
        ((0, 60, 48, 78), (80, 0)),
        ((48, 60, 78, 78), (80, 20)),
    ):
        result.paste(art.crop(tuple(n * scale for n in box)),
                     tuple(n * scale for n in position))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_ini", type=Path)
    parser.add_argument("tf3_pack", type=Path)
    parser.add_argument("--gx-data", type=Path, default=Path("build/gx_data"))
    parser.add_argument("--tf3-iso", type=Path, default=Path("TF3.iso"))
    parser.add_argument("--xxhash-library", default="C:/msys64/ucrt64/bin/libxxhash.dll")
    parser.add_argument("--moki-pack", type=Path, help="optional ULES01183 TCG card textures from Moki")
    args = parser.parse_args()

    output = args.base_ini.with_name("textures.ini")
    if output.resolve() == args.base_ini.resolve():
        parser.error("use a separate copy of MikeyMK's original INI as the base")
    base = args.base_ini.read_text(encoding="utf-8-sig")
    if not re.search(r"^hash[ \t]*=[ \t]*xxh64[ \t]*$", base, re.M | re.I):
        parser.error("MikeyMK's base INI must use xxh64")
    # Keep the original in-game sprites so the renderer upscales them.
    base, original_sprites = re.subn(
        r"^[0-9a-f]{24}[ \t]*=[ \t]*characters[/\\]sprites[/\\][^\n]*\n?",
        "",
        base,
        flags=re.M | re.I,
    )
    base_hashes = read_hashes(base)
    tcg_ini = args.tf3_pack / "textures.iniUSE_THIS_FOR_TCG_CARDS_INSTEAD"
    if not tcg_ini.is_file():
        tcg_ini = args.tf3_pack / "textures.ini"
    tcg_hashes = read_hashes(tcg_ini.read_text(encoding="utf-8-sig"))
    moki_hashes = {}
    if args.moki_pack:
        moki_ini = args.moki_pack / "~Packs (ini)/TCG/textures.ini"
        moki_hashes = read_hashes(moki_ini.read_text(encoding="utf-8-sig"))

    library = ctypes.CDLL(args.xxhash_library)
    xxh32 = library.XXH32
    xxh32.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32)
    xxh32.restype = ctypes.c_uint32
    xxh64 = library.XXH64
    xxh64.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64)
    xxh64.restype = ctypes.c_uint64

    gx_kids = (args.gx_data / "gx_id_map.h").read_text()
    gx_kids = gx_kids.split("g_gx_tf1_to_tf3_kid[", 1)[1].split("{", 1)[1].split("}", 1)[0]
    tf3_kids = [int(kid) for kid in re.findall(r"\d+", gx_kids)]
    seto_small = (args.gx_data / "cardm_e.cip").read_bytes()
    seto_large = (args.gx_data / "cardh_e.cip").read_bytes()
    tf3_small = read_iso_slice(args.tf3_iso, TF3_CARDM_LBN, TF3_CARDM_SIZE)
    aliases = {}
    ambiguous = set()
    card_paths = {}
    coverage = {"TF1 large": 0, "TF3 small": 0, "TF3 large": 0}
    moki_coverage = {"small": 0, "large": 0}

    def texture_key(dim, payload):
        return dim << 32 | (xxh64(payload, len(payload), 0xBACD7814) & 0xFFFFFFFF)

    def add_alias(dim, payload, path, group):
        if not (output.parent / path).is_file():
            return
        key = texture_key(dim, payload)
        existing = base_hashes.get(key, aliases.get(key))
        if existing is not None and existing != path:
            ambiguous.add(key)
            return
        if key not in base_hashes:
            aliases[key] = path
        coverage[group] += 1

    def fill_from_moki(dim, payload, tf3_kid):
        if not moki_hashes:
            return None
        key = texture_key(dim, payload)
        if key in base_hashes or key in aliases:
            return None
        relative = f"TF3 TCG Cards with TF3 Rarity/{tf3_kid}.png"
        if moki_hashes.get(key) != relative:
            return None
        source = args.moki_pack / relative
        if not source.is_file():
            return None
        kind = "large" if dim == 0x0607 else "small"
        path = f"moki-extra/{kind}/{tf3_kid}.png"
        target = output.parent / path
        target.parent.mkdir(parents=True, exist_ok=True)
        moki_texture(source, payload if kind == "large" else None).save(target)
        aliases[key] = path
        moki_coverage[kind] += 1
        return path

    for kid, tf3_kid in enumerate(tf3_kids):
        if kid >= TF1_EXT_KID_START and not tf3_kid:
            continue
        large = resolve_cip_block(seto_large, kid, 2)
        if large and large.startswith(b"MIG."):
            # The final 128 GE texture bytes are beyond the card's CIP block.
            if kid < TF1_EXT_KID_START:
                add_alias(0x0607, large[0x80:], f"cards/large/{kid}.png", "TF1 large")
            else:
                add_alias(0x0607, large[0x80:], f"tf3-extra/cardslarge/{tf3_kid}.png", "TF3 large")
                fill_from_moki(0x0607, large[0x80:], tf3_kid)
        if kid < TF1_EXT_KID_START:
            continue
        source = resolve_cip_block(tf3_small, tf3_kid, 1)
        target = resolve_cip_block(seto_small, kid, 1)
        if source and target:
            path = tcg_hashes.get(0x0606 << 32 | quick_hash(source), "")
            if path.startswith("texturepack_hd_tcg_cards/"):
                path = f"tf3-extra/{path}"
                if (output.parent / path).is_file():
                    card_paths[kid] = path
                    add_alias(0x0606, target, path, "TF3 small")
            fill = fill_from_moki(0x0606, target, tf3_kid)
            if fill:
                card_paths[kid] = fill

    if not all(coverage.values()):
        parser.error("could not locate TF1 and TF3 PNGs in the hybrid directory")

    aliases.update(add_deck_sheets(
        output.parent, (args.gx_data / "deck_all_e.ehp").read_bytes(),
        base_hashes | aliases, card_paths, xxh32, xxh64, seto_small,
    ))

    # These TF3 atlas variants are absent from MikeyMK's TF1 mappings.
    for key, path in (
        (0xA311164BCF3AAFB0, "tf3-extra/hud/cardicons.png"),
        (0x3BB2DDFCEE3B1A72, "tf3-extra/hud/cardhoriztonal.png"),
    ):
        if key not in base_hashes and (output.parent / path).is_file():
            aliases.setdefault(key, path)

    lines = ["", "[options]", "skipLastDXT1Blocks128x64 = 16", "", "[hashes]"]
    lines.extend(f"{key:024x} = {aliases[key]}" for key in sorted(aliases.keys() - ambiguous))
    output.write_text(base.rstrip() + "\n" + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"Card coverage before deduplication: {coverage}")
    if args.moki_pack:
        print(f"Missing textures filled from Moki: {moki_coverage}")
    print(f"Added {len(aliases.keys() - ambiguous)} aliases. Ambiguous keys skipped: {len(ambiguous)}.")
    print(f"Kept original textures for {original_sprites} sprite mappings.")


if __name__ == "__main__":
    main()
