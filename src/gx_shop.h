#ifndef SETO_GX_SHOP_H
#define SETO_GX_SHOP_H

typedef struct {
    const uint16_t *text[3];
    uint16_t price;
    uint16_t size;
    struct { const uint16_t *cards; uint16_t count; } rarity[4];
} gx_shop_pack_t;

#include "tf3-shop-catalog.h"

enum {
    SHOP_SLOTS = 48,
    SHOP_ROWS = 0x0361BE88,
    SHOP_SELECTION = 0x03C1C53C,
    SHOP_CURSOR = 0x03C1C59D,
    SHOP_HISTORY = 0x03C1C600,
    GX_SHOP_BASE = 0x06632000,
    GX_SHOP_END = 0x06640000,
    GX_SHOP_COVERS = 0x06200000,
    GX_SHOP_COVER_ROOM = 0x00200000,
    GX_SHOP_COVER_BYTES = 9424 + 3280 + 34000,
};

typedef struct {
    uint8_t flags, completion;
    uint16_t reserved, price, unlock;
    uint32_t pool;
} gx_shop_row_t;

_Static_assert(sizeof(gx_shop_row_t) == 12, "modshop row layout");

static gx_shop_row_t g_shop_rows[2][SHOP_SLOTS];
static uint8_t g_shop_history[2][SHOP_SLOTS];
static uint32_t g_shop_texts[SHOP_SLOTS][3];
static unsigned g_shop_tab, g_shop_tf3_count;
static int g_shop_refresh;
static uint32_t g_shop_write, g_shop_grid;
typedef struct { uint32_t list, original, total; } gx_shop_weight_t;
static gx_shop_weight_t g_shop_weights[SHOP_SLOTS * 4u];
static unsigned g_shop_weight_count;

static uint32_t gx_shop_append(const void *src, uint32_t size) {
    if (size > GX_SHOP_END - 8u - g_shop_write) return 0;
    uint32_t addr = g_shop_write;
    if (size && psp_mem_write_block(addr, src, size) != 0) return 0;
    g_shop_write = (addr + size + 3u) & ~3u;
    return addr;
}

static uint32_t gx_shop_pool(const gx_shop_pack_t *pack, uint32_t original, int weighted) {
    uint8_t seen[GX_TF1_EXT_KID_MAX + 1u] = {0};
    uint32_t table[4][2] = {{0}};
    uint16_t cards[GX_TF1_EXT_KID_MAX + 1u];
    if (original) {
        for (unsigned rarity = 0; rarity < 4; rarity++) {
            uint32_t addr = psp_read32(original + rarity * 8u);
            uint32_t count = psp_read32(original + rarity * 8u + 4u);
            if (count > GX_TF1_EXT_KID_MAX) return 0;
            for (uint32_t i = 0; i < count; i++) {
                uint16_t kid = psp_read16(addr + i * 2u);
                if (!kid || kid > GX_TF1_EXT_KID_MAX) return 0;
                seen[kid] = 1;
            }
        }
    }
    for (unsigned rarity = 0; rarity < 4; rarity++) {
        uint32_t addr = original ? psp_read32(original + rarity * 8u) : 0;
        uint32_t count = original ? psp_read32(original + rarity * 8u + 4u) : 0;
        const uint32_t base_count = count;
        for (uint32_t i = 0; i < count; i++) cards[i] = psp_read16(addr + i * 2u);
        for (uint32_t i = 0; i < pack->rarity[rarity].count; i++) {
            uint16_t tf3 = pack->rarity[rarity].cards[i];
            if (tf3 > GX_TF3_KID_MAX) continue;
            uint16_t kid = g_gx_tf3_to_tf1_kid[tf3];
            if (!kid || seen[kid]) continue;
            seen[kid] = 1;
            cards[count++] = kid;
        }
        if (count) {
            table[rarity][0] = gx_shop_append(cards, count * 2u);
            if (!table[rarity][0]) return 0;
            table[rarity][1] = count;
            if (weighted && base_count && base_count < count) {
                g_shop_weights[g_shop_weight_count++] =
                    (gx_shop_weight_t){table[rarity][0], base_count, count};
            }
        }
    }
    return gx_shop_append(table, sizeof table);
}

static void install_tf3_shop_pack(void) {
    g_shop_tab = g_shop_tf3_count = 0;
    g_shop_weight_count = 0;
    g_shop_refresh = 0;
    if (!psp_mem_ptr(GX_SHOP_BASE, GX_SHOP_END - GX_SHOP_BASE) ||
        psp_read32(SHOP_ROWS + 37u * 12u + 8u) != 0x0361A37Cu) return;
    g_shop_write = GX_SHOP_BASE;
    memset(g_shop_rows, 0, sizeof g_shop_rows);
    memset(g_shop_history, 0, sizeof g_shop_history);
    if (psp_mem_read_block(g_shop_rows[0], SHOP_ROWS, sizeof g_shop_rows[0]) != 0) return;
    if (!psp_mem_ptr(GX_SHOP_COVERS, GX_SHOP_COVER_ROOM)) return;

    unsigned next = 0;
    for (unsigned i = 0; i < sizeof gx_shop_packs / sizeof gx_shop_packs[0]; i++) {
        const gx_shop_pack_t *pack = &gx_shop_packs[i];
        unsigned same = g_shop_equivalent[i];
        gx_shop_row_t *row = same ? &g_shop_rows[0][same - 1] : &g_shop_rows[1][next];
        const int weighted = same && same != SHOP_SLOTS; /* Checkered Flag uses its full pool. */
        uint32_t pool = gx_shop_pool(pack, row->pool, weighted);
        if (!pool) return;
        row->pool = pool;
        if (same) continue;
        row->flags = (uint8_t)((pack->size << 2) | 2u);
        row->price = pack->price;
        for (unsigned kind = 0; kind < 3; kind++) {
            uint32_t units = 1;
            while (pack->text[kind][units - 1]) units++;
            g_shop_texts[next][kind] = gx_shop_append(pack->text[kind], units * 2u);
            if (!g_shop_texts[next][kind]) return;
        }
        if ((next + 1u) * GX_SHOP_COVER_BYTES > GX_SHOP_COVER_ROOM ||
            psp_mem_write_block(GX_SHOP_COVERS + next * GX_SHOP_COVER_BYTES,
                                gx_shop_cover_files[i], GX_SHOP_COVER_BYTES) != 0) return;
        next++;
    }
    g_shop_grid = GX_SHOP_COVERS + next * GX_SHOP_COVER_BYTES;
    if (next * GX_SHOP_COVER_BYTES + sizeof gx_shop_cover_grid > GX_SHOP_COVER_ROOM ||
        psp_mem_write_block(g_shop_grid, gx_shop_cover_grid, sizeof gx_shop_cover_grid) != 0) return;
    if (psp_mem_write_block(SHOP_ROWS, g_shop_rows[0], sizeof g_shop_rows[0]) == 0)
        g_shop_tf3_count = next;
}

static uint32_t gx_shop_reweight(uint32_t list, uint32_t count, uint32_t roll) {
    for (unsigned i = 0; i < g_shop_weight_count; i++) {
        const gx_shop_weight_t *weight = &g_shop_weights[i];
        if (weight->list != list || weight->total != count) continue;
        /* Keep the native rarity decision and give each added card two tickets. */
        uint32_t ticket = (roll & 0x7FFFFFFFu) % (count * 2u - weight->original);
        return ticket < weight->original ? ticket : weight->original + (ticket - weight->original) / 2u;
    }
    return roll;
}

static void gx_shop_handle_tabs(uint32_t input) {
    if (!g_shop_tf3_count || !input || (psp_read8(0x03C1C59Cu) & 0x8Fu)) return;
    uint32_t pressed = psp_read32(input + 4u) & 0x300u;
    if (pressed != 0x100u && pressed != 0x200u) return;
    unsigned target = pressed == 0x200u;
    if (target == g_shop_tab) return;
    for (unsigned i = 0; i < SHOP_SLOTS; i++)
        if (psp_read16(SHOP_SELECTION + i * 2u)) return;

    psp_mem_read_block(g_shop_rows[g_shop_tab], SHOP_ROWS, sizeof g_shop_rows[0]);
    psp_mem_read_block(g_shop_history[g_shop_tab], SHOP_HISTORY, SHOP_SLOTS);
    if (psp_mem_write_block(SHOP_ROWS, g_shop_rows[target], sizeof g_shop_rows[0]) != 0) return;
    psp_mem_write_block(SHOP_HISTORY, g_shop_history[target], SHOP_SLOTS);
    psp_write8(SHOP_CURSOR, 0);
    g_shop_tab = target;
    g_shop_refresh = 1;
}

int gx_shop_refresh_cursor(void) {
    if (g_shop_tab && psp_read8(SHOP_CURSOR) >= g_shop_tf3_count)
        psp_write8(SHOP_CURSOR, 0);
    int refresh = g_shop_refresh;
    g_shop_refresh = 0;
    return refresh;
}

uint32_t gx_shop_text(unsigned slot, unsigned kind) {
    if (!g_shop_tab) return 0;
    return slot < g_shop_tf3_count ? g_shop_texts[slot][kind] : 0x06630000u;
}

uint32_t gx_shop_cover(uint32_t material, unsigned slot) {
    static const char *const names[] = {"box_tex", "box_side", "pack_tex"};
    static const uint32_t offsets[] = {0, 9424, 9424 + 3280};
    if (!g_shop_tab || slot >= g_shop_tf3_count) return 0;
    const char *name = psp_mem_ptr(material, 8);
    if (!name) return 0;
    for (unsigned i = 0; i < 3; i++)
        if (strncmp(name, names[i], strlen(names[i])) == 0)
            return GX_SHOP_COVERS + slot * GX_SHOP_COVER_BYTES + offsets[i];
    return 0;
}

uint32_t gx_shop_grid(unsigned resource) {
    return g_shop_tab && resource == 0x2006u ? g_shop_grid : 0;
}

void store_import_forward(uint32_t nid);

static uint32_t gx_shop_random(uint32_t limit) {
    store_import_forward(0xD4DE5DD8u);
    return (psp_cpu.r[PSP_REG_V0] & 0x7FFFFFFFu) % limit;
}

int gx_shop_draw(unsigned slot, uint32_t result) {
    if (!g_shop_tab || slot >= g_shop_tf3_count) return 0;
    const gx_shop_row_t *row = &g_shop_rows[1][slot];
    uint32_t counts[4];
    for (unsigned i = 0; i < 4; i++) counts[i] = psp_read32(row->pool + i * 8u + 4u);
    unsigned size = row->flags >> 2;
#if GX_SHOP_BONUS_COUNT > 0
    if (slot + 1u == g_shop_tf3_count) {
        uint16_t remaining[GX_SHOP_BONUS_COUNT];
        uint32_t list = psp_read32(row->pool);
        if (counts[0] != GX_SHOP_BONUS_COUNT || size > counts[0] ||
            psp_mem_read_block(remaining, list, sizeof remaining) != 0) return 0;
        /* Draw from the remaining cards to guarantee distinct cards within a pack. */
        for (unsigned i = 0, left = counts[0]; i < size; i++) {
            unsigned index = gx_shop_random(left);
            psp_write16(result + i * 2u, remaining[index]);
            remaining[index] = remaining[--left];
        }
        return 1;
    }
#endif
    /* Match the shop's common slots plus a weighted rare slot, skipping empty tiers. */
    for (unsigned i = 0; i < size; i++) {
        unsigned rarity = 0;
        if (i == size - 1u) {
            unsigned roll = gx_shop_random(100);
            rarity = roll < 10u ? 3u : roll < 30u ? 2u : 1u;
        }
        while (!counts[rarity] && rarity) rarity--;
        while (rarity < 4u && !counts[rarity]) rarity++;
        if (rarity == 4u) return 0;
        uint32_t cards = psp_read32(row->pool + rarity * 8u);
        psp_write16(result + i * 2u, psp_read16(cards + gx_shop_random(counts[rarity]) * 2u));
    }
    return 1;
}

#endif
