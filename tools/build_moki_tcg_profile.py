#!/usr/bin/env python3
"""Build a standalone TCG pack with Moki's card PNGs in its moki-tcg/ folder."""

import argparse
import ctypes
import re
from shutil import copy2
from pathlib import Path

from PIL import Image

from adapt_tf3_texture_pack import read_hashes
from build_gx_carddb import resolve_cip_block
from mix_tf1_tf3_texture_pack import add_deck_sheets


def copy_fallback_assets(light_pack, tcg_pack, ini):
    for entry in set(read_hashes(ini).values()):
        path = Path(entry.split("#", 1)[0].split("|", 1)[0].strip())
        if path == Path("."):
            continue
        if path.is_absolute() or path.anchor or ".." in path.parts:
            raise ValueError(f"Texture path must be relative to the pack: {path}")
        if path.parts[0] == "moki-tcg":
            continue
        target = tcg_pack / path
        target.parent.mkdir(parents=True, exist_ok=True)
        copy2(light_pack / path, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("light_pack", type=Path, help="existing Light pack directory")
    parser.add_argument("pack", type=Path, help="separate TCG directory containing moki-tcg/")
    parser.add_argument("--gx-data", type=Path, default=Path("build/gx_data"))
    parser.add_argument("--xxhash-library", default="C:/msys64/ucrt64/bin/libxxhash.dll")
    args = parser.parse_args()
    light_root, tcg_root = args.light_pack.resolve(), args.pack.resolve()
    if light_root.is_relative_to(tcg_root) or tcg_root.is_relative_to(light_root):
        parser.error("the Light and TCG directories must be separate")

    library = ctypes.CDLL(args.xxhash_library)
    xxh64 = library.XXH64
    xxh64.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64)
    xxh64.restype = ctypes.c_uint64
    xxh32 = library.XXH32
    xxh32.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32)
    xxh32.restype = ctypes.c_uint32

    light = (args.light_pack / "textures.ini").read_text(encoding="utf-8-sig")
    mapping = (args.gx_data / "gx_id_map.h").read_text()
    mapping = mapping.split("g_gx_tf1_to_tf3_kid[", 1)[1].split("{", 1)[1].split("}", 1)[0]
    small = (args.gx_data / "cardm_e.cip").read_bytes()
    large = (args.gx_data / "cardh_e.cip").read_bytes()
    thumbnails = args.pack / "moki-tcg/small"
    thumbnails.mkdir(parents=True, exist_ok=True)

    card_paths = {}
    aliases = {}
    ambiguous = set()

    def add_alias(cachekey, payload, path):
        key = (cachekey << 32) | (xxh64(payload, len(payload), 0xBACD7814) & 0xFFFFFFFF)
        if aliases.setdefault(key, path) != path:
            ambiguous.add(key)

    for kid, donor in enumerate(re.findall(r"\d+", mapping)):
        front = f"moki-tcg/{donor}.png"
        source = args.pack / front
        if not source.is_file():
            continue
        thumb = f"moki-tcg/small/{donor}.png"
        with Image.open(source) as card:
            if card.size != (813, 1185):
                raise ValueError(f"Unexpected Moki card size in {source}")
            if not (args.pack / thumb).is_file():
                card.convert("RGB").resize((256, 256), Image.Resampling.LANCZOS).save(args.pack / thumb)
        card_paths[kid] = thumb
        if medium := resolve_cip_block(small, kid, 1):
            add_alias(0x0606, medium, thumb)
        if detail := resolve_cip_block(large, kid, 2):
            if detail.startswith(b"MIG."):
                # Private cache key for the full front. The packed art keeps its light mapping.
                add_alias(0x4D4F4B49, detail[0x80:], front)

    if not aliases:
        parser.error("could not find Moki card PNGs for this Pure GX build")
    aliases.update(add_deck_sheets(
        args.pack, (args.gx_data / "deck_all_e.ehp").read_bytes(),
        read_hashes(light), card_paths, xxh32, xxh64, small, base_pack=args.light_pack,
    ))
    lines = [f"{key:024x} = {aliases[key]}" for key in sorted(aliases.keys() - ambiguous)]
    ini = light.rstrip() + "\n\n[hashes]\n" + "\n".join(lines) + "\n"
    copy_fallback_assets(args.light_pack, args.pack, ini)
    output = args.pack / "textures.ini"
    temp = output.with_suffix(".tmp")
    temp.write_text(ini, encoding="utf-8")
    temp.replace(output)
    print(f"Moki TCG: {len(card_paths)} fronts, {len(ambiguous)} ambiguous aliases kept on light.")


if __name__ == "__main__":
    main()
