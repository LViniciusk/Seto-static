"""Extract the ULES-01183 boosters and fill gaps in the Pure GX catalogue."""

import argparse
import mmap
from pathlib import Path
import re
import struct
from build_gx_carddb import (
    TF1_EXT_KID_START, TF3_DECK_ALL_LBN, TF3_DECK_ALL_SIZE, _swizzle_8bpp, _unswizzle_8bpp,
    parse_ehp, read_iso_slice, read_utf16_at,
)


# Same-name packs and their confirmed renamed counterparts in the TF1 shop.
EQUIVALENTS = {20: 37, 41: 44, 42: 43, 43: 46, 46: 45, 47: 47}
BONUS_COVER = 46


def extract(iso):
    with iso.open("rb") as disc:
        disc.seek(0x73140 * 2048)
        shop = disc.read(181856)
        with mmap.mmap(disc.fileno(), 0, access=mmap.ACCESS_READ) as image:
            first = "Beginner Monsters\0Beginner Spells\0Beginner Traps\0".encode("utf-16-le")
            names_at = image.find(first)
            if names_at < 0:
                raise ValueError("English TF3 booster names missing")
            names = image[names_at:names_at + 4096].decode("utf-16-le", "replace").split("\0")[:48]
    if len(shop) != 181856 or shop[:6] != b"\x7fELF\x01\x01" or b"modshop\0" not in shop:
        raise ValueError("expected the ULES-01183 shop PRX at sector 0x73140")

    if len(set(names)) != 48 or names[20] != "Technology Master" or names[-1] != "Checkered Flag 2":
        raise ValueError("unexpected TF3 booster names")
    boosters = []
    for slot, name in enumerate(names):
        flags, price, _, table = struct.unpack_from("<IHHI", shop, 0x2031C + slot * 12)
        if flags not in (20, 40) or not price or not 0x1AF24 <= table <= 0x20300:
            raise ValueError(f"unexpected TF3 booster record for {name}")
        pools = []
        for rarity in range(4):
            address, count = struct.unpack_from("<II", shop, 0x54 + table + 8 * rarity)
            if count and not 0x1AF24 <= address <= table - 2 * count:
                raise ValueError(f"unexpected TF3 rarity pool for {name}")
            cards = struct.unpack_from(f"<{count}H", shop, 0x54 + address) if count else ()
            if not all(3000 <= card < 9000 for card in cards):
                raise ValueError(f"invalid card ID in TF3 rarity pool for {name}")
            pools.append(cards)
        boosters.append((name, price, flags >> 2, pools))
    return boosters


def uncovered_cards(boosters, gx_data, iso):
    maps = (gx_data / "gx_id_map.h").read_text(encoding="ascii")

    def ids(name):
        payload = maps.split(name + "[", 1)[1].split("{", 1)[1].split("}", 1)[0]
        return [int(kid) for kid in re.findall(r"[0-9]+", payload)]

    to_gx = ids("g_gx_tf3_to_tf1_kid")
    to_tf3 = ids("g_gx_tf1_to_tf3_kid")
    covered = {to_gx[kid] for _, _, _, pools in boosters
               for pool in pools for kid in pool if kid < len(to_gx)}
    deck = dict(parse_ehp(read_iso_slice(iso, TF3_DECK_ALL_LBN, TF3_DECK_ALL_SIZE)))
    table = deck["fileTbl.dat"]
    slots, = struct.unpack_from("<I", table)
    # The stock catalogue excludes tokens that have visual slots in Pure GX.
    collectable = {to_gx[kid] for kid in struct.unpack_from(f"<{slots - 1}H", table, 4)
                   if kid < len(to_gx)}
    return tuple(to_tf3[kid] for kid in sorted(collectable - covered)
                 if TF1_EXT_KID_START <= kid < len(to_tf3) and to_tf3[kid])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--gx-data", type=Path, default=Path("build/gx_data"))
    args = parser.parse_args()
    boosters = extract(args.iso)
    bonus = uncovered_cards(boosters, args.gx_data, args.iso)
    resources = dict(parse_ehp(read_iso_slice(args.iso, 0xB0B30, 2449280)))
    text = resources["pack_explain_str_e.bin"]
    text_start, = struct.unpack_from("<I", text)
    body = []
    catalog = boosters + [("Special Collection", 150, min(5, len(bonus)), (bonus, (), (), ()))] if bonus else boosters
    body.append(f"#define GX_SHOP_BONUS_COUNT {len(bonus)}u")
    for slot, (name, _, _, pools) in enumerate(catalog):
        if slot == len(boosters):
            strings = [name, "Cards from special rewards and promotions.",
                       "Cards from special rewards and promotions."]
        else:
            strings = [name]
            for i in range(2):
                offset, = struct.unpack_from("<I", text, 4 + (slot * 2 + i) * 4)
                strings.append(read_utf16_at(text, text_start + offset))
        for kind, string in enumerate(strings):
            encoded = string.encode("utf-16-le")
            units = struct.unpack(f"<{len(encoded) // 2}H", encoded)
            body.append(f"static const uint16_t tf3_text_{slot}_{kind}[] = {{ " +
                        ", ".join(map(str, units)) + ", 0 };")
        for rarity, cards in enumerate(pools):
            if not cards:
                continue
            body.append(f"static const uint16_t tf3_pack_{slot}_{rarity}[] = {{")
            for start in range(0, len(cards), 20):
                body.append("    " + ", ".join(map(str, cards[start:start + 20])) + ",")
            body.append("};")
    body.append("static const gx_shop_pack_t gx_shop_packs[] = {")
    for slot, (_, price, size, pools) in enumerate(catalog):
        lists = ", ".join(f"{{tf3_pack_{slot}_{rarity}, {len(cards)}}}" if cards else "{0, 0}"
                          for rarity, cards in enumerate(pools))
        texts = ", ".join(f"tf3_text_{slot}_{kind}" for kind in range(3))
        body.append(f"    {{{{{texts}}}, {price}, {size}, {{{lists}}}}},")
    body.append("};")
    body.append(f"static const uint8_t g_shop_equivalent[{len(catalog)}] = {{")
    body.extend(f"    [{tf3}] = {tf1 + 1}," for tf3, tf1 in EQUIVALENTS.items())
    body.append("};")
    for slot in range(48):
        source = resources[f"pack_tex{slot:02}.gim"]
        # The TF3 cover occupies the first square in its material atlas.
        # Give the three TF1 materials their native geometry and the TF3 design.
        pixels = _unswizzle_8bpp(source[0x4D0:], 256, 128)
        cover = b"".join(
            reshape_cover(source, pixels, width, height)
            for width, height in ((128, 64), (32, 64), (128, 256))
        )
        body.extend(emit_image(f"gx_shop_cover_{slot}", cover))
    body.append("static const unsigned char *const gx_shop_cover_files[] = {")
    body.extend(f"    gx_shop_cover_{slot}," for slot in range(48))
    if bonus:
        body.append(f"    gx_shop_cover_{BONUS_COVER},")
    body.append("};")
    body.extend(emit_image("gx_shop_cover_grid", reorder_grid(resources["s_pack.gim"], bool(bonus))))
    args.output.write_text("\n".join(body) + "\n", encoding="ascii")


def reshape_cover(source, pixels, width, height):
    data = bytes(pixels[(y * 128 // height) * 256 + x * 128 // width]
                 for y in range(height) for x in range(width))
    gim = bytearray(source[:0x4D0])
    size = len(gim) + len(data)
    struct.pack_into("<I", gim, 0x14, size - 0x10)
    struct.pack_into("<I", gim, 0x24, size - 0x20)
    struct.pack_into("<II", gim, 0x484, size - 0x480, size - 0x480)
    struct.pack_into("<HH", gim, 0x498, width, height)
    struct.pack_into("<I", gim, 0x4B0, 0x40 + len(data))
    return bytes(gim) + _swizzle_8bpp(data, width, height)


def emit_image(name, data):
    yield f"static const unsigned char {name}[{len(data)}] ="
    for start in range(0, len(data), 64):
        yield '    "' + ''.join(f'\\x{byte:02x}' for byte in data[start:start + 64]) + '"'
    yield ";"


def reorder_grid(source, bonus):
    original = _unswizzle_8bpp(source[0x4D0:], 256, 256)
    output = bytearray(original)
    slots = [i for i in range(48) if i not in EQUIVALENTS]
    if bonus:
        slots.append(BONUS_COVER)
    for to, old in enumerate(slots):
        x, y = to % 12 * 20, to // 12 * 40
        ox, oy = old % 12 * 20, old // 12 * 40
        for row in range(40):
            dst, src = (y + row) * 256 + x, (oy + row) * 256 + ox
            output[dst:dst + 20] = original[src:src + 20]
    for to in range(len(slots), 48):
        x, y = to % 12 * 20, to // 12 * 40
        for row in range(40):
            dst = (y + row) * 256 + x
            output[dst:dst + 20] = bytes(20)
    return source[:0x4D0] + _swizzle_8bpp(output, 256, 256)


if __name__ == "__main__":
    main()
