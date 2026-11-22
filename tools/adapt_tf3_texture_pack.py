#!/usr/bin/env python3
"""Add Seto card texture hashes to a TF3 PPSSPP quick-hash pack."""

import argparse
from pathlib import Path
import re
import struct

from build_gx_carddb import (
    TF3_CARDM_LBN,
    TF3_CARDM_SIZE,
    read_iso_slice,
    resolve_cip_block,
)


def quick_hash(data):
    """PPSSPP quick hash for aligned DXT1 payloads."""
    assert len(data) % 64 == 0
    cursor = [0] * 4
    weights = [0xC00B, 0x9BD9, 0x4B73, 0xB651, 0x4D9B, 0x4309, 0x0083, 1]
    for block in struct.iter_unpack("<16I", data):
        for lane in range(4):
            lo_weight, hi_weight = weights[2 * lane : 2 * lane + 2]
            a, b, c, d = block[lane::4]
            prev = cursor[lane]
            lo = (prev + (a & 0xFFFF) * lo_weight) & 0xFFFF
            hi = ((prev >> 16) + (a >> 16) * hi_weight) & 0xFFFF
            mixed = ((lo | hi << 16) ^ b) + c
            lo = (d & 0xFFFF) * lo_weight & 0xFFFF
            hi = (d >> 16) * hi_weight & 0xFFFF
            cursor[lane] = mixed ^ (lo | hi << 16)
        weights = [(weight + 0x2455) & 0xFFFF for weight in weights]
    return (sum(cursor) + sum(weights[::2]) + (sum(weights[1::2]) << 16)) & 0xFFFFFFFF


def read_hashes(ini):
    return {
        int(key, 16): path.strip()
        for key, path in re.findall(r"^([0-9a-fA-F]{24})[ \t]*=[ \t]*([^\r\n]+)", ini, re.M)
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", type=Path)
    parser.add_argument("output_ini", type=Path)
    parser.add_argument("--gx-data", type=Path, default=Path("build/gx_data"))
    parser.add_argument("--tf3-iso", type=Path, default=Path("TF3.iso"))
    args = parser.parse_args()

    default_ini = args.pack / "textures.ini"
    tcg_ini = args.pack / "textures.iniUSE_THIS_FOR_TCG_CARDS_INSTEAD"
    if args.output_ini.resolve() in (default_ini.resolve(), tcg_ini.resolve()):
        parser.error("write to a separate pack directory")
    source = tcg_ini if tcg_ini.is_file() else default_ini
    ini = source.read_text(encoding="utf-8-sig")
    if not re.search(r"^hash\s*=\s*quick\s*$", ini, re.M | re.I):
        parser.error("this adapter requires a quick-hash TF3 pack")
    hashes = read_hashes(ini)
    kid_map = (args.gx_data / "gx_id_map.h").read_text()
    kid_map = kid_map.split("g_gx_tf1_to_tf3_kid[", 1)[1].split("{", 1)[1].split("}", 1)[0]
    tf3_ids = [int(value) for value in re.findall(r"\d+", kid_map)]
    tf3_field = read_iso_slice(args.tf3_iso, TF3_CARDM_LBN, TF3_CARDM_SIZE)
    seto_field = (args.gx_data / "cardm_e.cip").read_bytes()
    seto_large = (args.gx_data / "cardh_e.cip").read_bytes()
    aliases = {}
    ambiguous = set()
    counts = {"field": 0, "large": 0}

    for kid, tf3_id in enumerate(tf3_ids):
        if not tf3_id:
            continue
        small = resolve_cip_block(seto_field, kid, 1)
        tf3_small = resolve_cip_block(tf3_field, tf3_id, 1)
        large = resolve_cip_block(seto_large, kid, 2)
        # CIP stores two sectors including a 0x80-byte GIM header. The
        # final 0x80 bytes read by the GE are outside the card's own block.
        large_data = large[0x80:] if large and large[:4] == b"MIG." else None
        for kind, dim, data, path in (
            ("field", 0x0606, small, hashes.get(0x0606 << 32 | quick_hash(tf3_small)) if tf3_small else None),
            ("large", 0x0607, large_data, f"cardslarge/{tf3_id}.png"),
        ):
            if not data or not path or not (args.pack / path).is_file():
                continue
            key = dim << 32 | quick_hash(data)
            existing = hashes.get(key, aliases.get(key))
            if existing is not None and existing.lower() != path.lower():
                ambiguous.add(key)
                continue
            counts[kind] += 1
            if key not in hashes:
                aliases[key] = path

    if not all(counts.values()):
        parser.error("the pack or GX card archives did not yield both card sizes")
    # TF1's duel uses the same Spell/Trap atlas and palette in linear order.
    spelltrap = hashes.get(0x7EEDAD239D7919AE)
    if spelltrap and (args.pack / spelltrap).is_file() and 0x7EEDAD23D9151624 not in hashes:
        aliases[0x7EEDAD23D9151624] = spelltrap

    if not (args.pack / "hud/cardhorizontal.png").is_file() and (args.pack / "hud/cardhoriztonal.png").is_file():
        for key, path in hashes.items():
            if path == "hud/cardhorizontal.png":
                aliases[key] = "hud/cardhoriztonal.png"

    # The interface still uses TF1 textures alongside the TF3 card art.
    tf1_pack = args.output_ini.parent / "tf1"
    tf1_ini = tf1_pack / "textures.ini"
    if tf1_ini.is_file():
        for key, path in read_hashes(tf1_ini.read_text(encoding="utf-8-sig")).items():
            # This replacement positions the player outside the portrait's crop.
            if path == "Potrait/Player1.png":
                continue
            if path.startswith(("HUD/", "Potrait/")) and (tf1_pack / path).is_file() and key not in hashes:
                aliases.setdefault(key, "tf1/" + path)

    mikeymk = args.output_ini.parent / "mikeymk"
    portrait_ini = mikeymk / "portraits.ini"
    if portrait_ini.is_file():
        for key, path in read_hashes(portrait_ini.read_text(encoding="utf-8-sig")).items():
            if path.startswith("characters/portraits/duel/") and (mikeymk / path).is_file() and key not in hashes:
                aliases[key] = "mikeymk/" + path

    extra = "".join(f"{key:024x} = {path}\n" for key, path in sorted(aliases.items()) if key not in ambiguous)
    args.output_ini.parent.mkdir(parents=True, exist_ok=True)
    args.output_ini.write_text(
        ini.rstrip() + "\n\n[options]\nskipLastDXT1Blocks128x64 = 16\n\n[hashes]\n" + extra,
        encoding="utf-8",
    )
    print(f"Matches by card size before deduplication: {counts}")
    print(f"Added {len(aliases.keys() - ambiguous)} aliases, skipped {len(ambiguous)} ambiguous hashes")


if __name__ == "__main__":
    main()
