/* Phase B ABI & ID translation bridge between TF1 duel modules and TF3 rel_duel_eng.
 *
 * Keeps Phase B isolated from host_window.c, GPU backend, and seto_net.c.
 */
#include "tf3_bridge.h"
#include "gx_trunk.h"

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include "build/gx_data/gx_id_map.h"
#include "build/gx_data/tf3_card_meta.h"
#include "build/gx_data/tf3_card_names.h"
#include "build/gx_data/tf3_arch_tables.h"
#include "build/gx_data/tf3_c_tables.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t psp_display_vblanks(void);

static inline int tf3_env_flag(int *slot, const char *name) {
    if (*slot < 0) *slot = getenv(name) != NULL ? 1 : 0;
    return *slot;
}
static int tf3_trace_bridge(void)   { static int f = -1; return tf3_env_flag(&f, "TF3_BRIDGE_TRACE"); }
static int tf3_trace_abi_ev(void)   { static int f = -1; return tf3_env_flag(&f, "SETO_ABI_TRACE_EVENT"); }
static int tf3_trace_abi_vis(void)  { static int f = -1; return tf3_env_flag(&f, "SETO_ABI_TRACE_VISUAL"); }
static int tf3_trace_abi_stats(void){ static int f = -1; return tf3_env_flag(&f, "SETO_ABI_TRACE_STATS"); }
static int tf3_trace_abi_calls(void){ static int f = -1; return tf3_env_flag(&f, "SETO_ABI_TRACE_CALLS"); }
static int tf3_trace_step(void)     { static int f = -1; return tf3_env_flag(&f, "SETO_TRACE_STEP"); }
static int tf3_trace_ff(void)       { static int f = -1; return tf3_env_flag(&f, "SETO_TRACE_FF"); }
static int tf3_trace_prompt(void)   { static int f = -1; return tf3_env_flag(&f, "SETO_TRACE_PROMPT"); }
static int tf3_trace_nova(void)     { static int f = -1; return tf3_env_flag(&f, "SETO_TRACE_NOVA"); }
static int tf3_trace_post_deck(void){ static int f = -1; return tf3_env_flag(&f, "SETO_TRACE_POST_DECK"); }
static int tf3_test_gx_deck(void)   { static int f = -1; return tf3_env_flag(&f, "SETO_TEST_GX_DECK"); }

typedef struct {            /* TF1, 0x2C */
    int id;                 /* +0x00 */
    int attr;               /* +0x04 */
    int kind;               /* +0x08 */
    int mask;               /* +0x0C */
    int level;              /* +0x10 */
    int atk;                /* +0x14 */
    int def;                /* +0x18 */
    int batk;               /* +0x1C */
    int bdef;               /* +0x20 */
    int imm;                /* +0x24 */
    int imm2;               /* +0x28 */
} Tf1DuelStats;

typedef struct {            /* TF3, >= 0x34 */
    int id;                 /* +0x00 */
    int unk4;               /* +0x04 */
    int attr;               /* +0x08 */
    int kind;               /* +0x0C */
    int mask;               /* +0x10 */
    int level;              /* +0x14 */
    int atk;                /* +0x18 */
    int def;                /* +0x1C */
    int batk;               /* +0x20 */
    int bdef;               /* +0x24 */
    int imm;                /* +0x28 */
    int imm2;               /* +0x2C */
    int unk30;              /* +0x30 */
} Tf3DuelStats;

/* Persistent GX window between module_htmlview (<=0x06104C10) and EBOOT (0x08000000). */
#define GX_EXTRA_BASE            0x06200000u
#define GX_EXTRA_SIZE            0x01E00000u
#define GX_TF3_DLG_TEXT_ADDR     0x06620000u /* TF3 DLG_Text_E.bin (~59 KB) */
#define GX_EMPTY_WSTR_ADDR       0x06630000u /* 16 zero bytes */
#define GX_TF3_DUEL_INFO_ADDR    0x06631000u /* 4 KB TF1->TF3 translated 0x88B6C7F0 struct */
#define GX_CIP_TBL_BASE          0x06640000u /* 8 x 64 KB .cip header+index table slots */
#define GX_CIP_TBL_SLOTS         8u
#define GX_CIP_CACHE_BASE        0x066C0000u /* 5 x 256 KB small-buffer LRU cache slots */
#define GX_CIP_CACHE_SLOTS       5u
#define GX_CARDH_PRELOAD_ADDR    0x06800000u /* 15 MB cardh_e.cip full preload buffer */
#define GX_CARDM_PRELOAD_ADDR    0x07700000u /* 8.5 MB cardm_e.cip full preload buffer */
#define GX_TF3_CARD_NAMES_ADDR   0x07F80000u /* TF3 CARD_Name_E.bin (~126 KB, tail 512 KB) */

/* Free 64 KB scratch window right above tf3eng .bss (0x0384BF80..0x03860000). */
#define TF3_SCRATCH_BASE         0x03850000u
#define TF3_SCRATCH_SIZE         0x00010000u
#define TF3_SCRATCH_STATS_ADDR   0x03850000u /* 4 KB for 0xd46615d9 */
#define TF3_SCRATCH_PARTNER_ADDR 0x03851000u /* 4 KB for 0x9a381a51 */
#define TF3_SCRATCH_EMPTY_WSTR   0x03852000u /* 16 zero bytes */
#define TF3_SCRATCH_CHAIN_ADDR   0x03858000u /* ~2 KB translated TF1 duel_chain_get */
#define TF3_SCRATCH_SPECIAL_ADDR 0x03858800u /* ~256 B translated TF1 duel_special_get */
#define TF3_SCRATCH_RESULTS_ADDR 0x03858C00u /* TF3 game_state2 during duel_finish */
#define TF3_RESULTS_SIZE         0x398u
#define TF3_SCRATCH_NET_RECV_ADDR 0x03859000u /* 4 x 524 B (2096 B) translated 0x82249188 recv slots */
#define TF3_NET_RECV_SLOT_SIZE    524u

static int g_tf3_scratch_mapped;
static int g_duel_bonuses_translated;
static int g_duel_finish_in_progress;
static int g_gx_extra_mapped;

void gx_runtime_init(void) {
    if (!g_gx_extra_mapped) {
        if (psp_mem_map_extra(GX_EXTRA_BASE, GX_EXTRA_SIZE) != 0) {
            fprintf(stderr, "tf3_bridge: failed to map GX extra window 0x%08X\n", GX_EXTRA_BASE);
            return;
        }
        g_gx_extra_mapped = 1;
    }
    /* Copy TF3 DLG_Text_E.bin once so 0xCB6030CF/0xE3C8F657/0x375A7F03 return real UTF-16LE strings. */
    if (psp_mem_write_block(GX_TF3_DLG_TEXT_ADDR, g_tf3_dlg_text, (uint32_t)sizeof(g_tf3_dlg_text)) != 0) {
        fprintf(stderr, "tf3_bridge: failed to populate TF3 DLG_Text_E\n");
    }
    /* Copy TF3 CARD_Name_E.bin so TF3-only cards (e.g. 0x18C9) also resolve UTF-16LE names. */
    if (psp_mem_write_block(GX_TF3_CARD_NAMES_ADDR, g_tf3_card_names, (uint32_t)sizeof(g_tf3_card_names)) != 0) {
        fprintf(stderr, "tf3_bridge: failed to populate TF3 CARD_Name_E\n");
    }
    /* Pre-seed extended Trunk (card_idx 2480..3479) with the save entry layout:
     * flags 0x05, owned count 3 (byte 1, read by CARD_GetNum), max 3 (byte 2).
     * The old pre-seed left byte 1 zero, so the game saw all 1,000 as unowned. */
    gx_trunk_defaults(psp_mem_ptr(GX_EXT_TRUNK_ADDR, GX_EXT_TRUNK_SIZE));
}

/* Out-of-band CIP/CPM index tables & preload/LRU cache in GX_EXTRA_BASE so PRXs with
 * smaller TF1 allocations (moddeck 10.67MB, modcardalbum 6.16MB, modfield 29KB, etc.)
 * never overflow when reading the 32,344-byte table or 14.5MB/7.3MB Pure GX .cip files. */
static uint32_t g_cip_s0[GX_CIP_TBL_SLOTS];
static uint32_t g_cip_next;

uint32_t gx_cip_tbl_addr(uint32_t s0) {
    for (uint32_t i = 0; i < GX_CIP_TBL_SLOTS; i++) {
        if (g_cip_s0[i] == s0) return GX_CIP_TBL_BASE + i * 0x10000u;
    }
    uint32_t idx = (g_cip_next++) % GX_CIP_TBL_SLOTS;
    g_cip_s0[idx] = s0;
    return GX_CIP_TBL_BASE + idx * 0x10000u;
}

uint32_t gx_cip_prepare_tbl_read(uint32_t s0) {
    uint32_t tbl = gx_cip_tbl_addr(s0);
    for (uint32_t off = 0; off < 2048u; off += 4u) {
        psp_write32(tbl + off, psp_read32(s0 + 172u + off));
    }
    return tbl + 2048u;
}

void gx_cip_setup_cache(uint32_t s0) {
    uint32_t buf_size     = psp_read32(s0 + 132u);
    uint32_t slot_sectors = psp_read32(s0 + 204u);
    uint8_t  flags        = psp_read8(s0 + 136u);
    uint32_t is_preload   = (flags >> 2) & 1u;
    uint32_t cache_buf    = 0;
    uint32_t num_slots    = 0;
    if (is_preload) {
        cache_buf = (slot_sectors >= 2u) ? GX_CARDH_PRELOAD_ADDR : GX_CARDM_PRELOAD_ADDR;
        num_slots = 4096u;
    } else if (buf_size >= 16384u) {
        cache_buf = (s0 + 220u + 15u) & ~15u;
        uint32_t bytes = (s0 + buf_size) - cache_buf;
        num_slots = slot_sectors ? (bytes / (slot_sectors << 11)) : 0u;
    } else {
        uint32_t slot = 0;
        for (uint32_t i = 0; i < GX_CIP_TBL_SLOTS; i++) {
            if (g_cip_s0[i] == s0) { slot = i % GX_CIP_CACHE_SLOTS; break; }
        }
        cache_buf = GX_CIP_CACHE_BASE + slot * 0x40000u;
        num_slots = slot_sectors ? (0x40000u / (slot_sectors << 11)) : 0u;
    }
    psp_write32(s0 + 148u, cache_buf);
    psp_write32(s0 + 152u, num_slots);
}

static uint32_t tf3_dlg_str_ptr(uint32_t dlg_id) {
    if (dlg_id >= TF3_DLG_COUNT) {
        return GX_EMPTY_WSTR_ADDR;
    }
    uint32_t off = (uint32_t)g_tf3_dlg_indx[dlg_id * 4u] |
                   ((uint32_t)g_tf3_dlg_indx[dlg_id * 4u + 1u] << 8) |
                   ((uint32_t)g_tf3_dlg_indx[dlg_id * 4u + 2u] << 16) |
                   ((uint32_t)g_tf3_dlg_indx[dlg_id * 4u + 3u] << 24);
    if (off >= sizeof(g_tf3_dlg_text)) {
        return GX_EMPTY_WSTR_ADDR;
    }
    return GX_TF3_DLG_TEXT_ADDR + off;
}

static uint16_t gx_tf1_to_tf3(uint16_t kid) {
    if (kid <= GX_TF1_EXT_KID_MAX) {
        return g_gx_tf1_to_tf3_kid[kid];
    }
    return 0u;
}

static uint16_t gx_tf3_to_tf1(uint16_t kid) {
    if (kid <= GX_TF3_KID_MAX) {
        return g_gx_tf3_to_tf1_kid[kid];
    }
    return 0u;
}

static uint16_t tf3_card_index(uint16_t kid) {
    if (kid < TF3_CARD_BASE_ID || kid > TF3_CARD_MAX_ID) {
        return 0u;
    }
    return g_tf3_intid_by_kid[kid - TF3_CARD_BASE_ID];
}

static uint64_t tf3_card_prop(uint16_t kid) {
    uint16_t idx = tf3_card_index(kid);
    if (idx >= TF3_CARD_SLOTS) {
        return 0ULL;
    }
    return g_tf3_prop_by_idx[idx];
}

static void call_guest_addr(uint32_t addr, uint32_t gp) {
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}

static int call_tf3_export_nid(uint32_t nid) {
    uint32_t gp = 0;
    uint32_t addr = psp_duel_export(nid, &gp);
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20) {
            fprintf(stderr, "tf3_bridge: missing TF3 export 0x%08X\n", nid);
        }
        psp_ret(0);
        return 0;
    }
    call_guest_addr(addr, gp);
    return 1;
}

static uint32_t tf3_card_name_ptr(uint16_t kid) {
    const uint16_t idx = tf3_card_index(kid);
    if (idx < TF3_CARD_NAME_SLOTS) {
        if (!g_gx_extra_mapped) gx_runtime_init();
        const uint32_t off = g_tf3_name_off_by_idx[idx];
        if (off < TF3_CARD_NAME_BYTES)
            return GX_TF3_CARD_NAMES_ADDR + off;
    }
    const uint16_t tf1_kid = gx_tf3_to_tf1(kid);
    uint32_t gp = 0;
    const uint32_t addr = duel_eboot_export_addr(0xAD0E1E03u, &gp);
    if (!tf1_kid || !addr)
        return TF3_SCRATCH_EMPTY_WSTR;
    psp_cpu.r[PSP_REG_A0] = tf1_kid;
    call_guest_addr(addr, gp);
    const uint32_t ret = psp_cpu.r[PSP_REG_V0];
    return (ret >= 0x08000000u) ? ret : TF3_SCRATCH_EMPTY_WSTR;
}

void duel_tf3_on_duel_load(void) {
    g_duel_bonuses_translated = 0;
    if (!g_tf3_scratch_mapped) {
        if (psp_mem_map_extra(TF3_SCRATCH_BASE, TF3_SCRATCH_SIZE) != 0) {
            fprintf(stderr, "tf3_bridge: failed to map scratch window 0x%08X\n", TF3_SCRATCH_BASE);
            return;
        }
        g_tf3_scratch_mapped = 1;
    }
    for (uint32_t off = 0; off < 0x3000u; off += 4u) {
        psp_write32(TF3_SCRATCH_BASE + off, 0u);
    }
    for (uint32_t off = 0; off < 4u * TF3_NET_RECV_SLOT_SIZE; off += 4u) {
        psp_write32(TF3_SCRATCH_NET_RECV_ADDR + off, 0u);
    }
}

/* TF1 DuelRecipe (264 bytes):
 *   +0x000 n_main, +0x004 n_side, +0x008 n_extra,
 *   +0x00C main_ids[80], +0x0AC side_ids[15], +0x0CA extra_ids[31].
 * TF3 duel_deck_set4 reads +0x000/+0x00C (main) and +0x008/+0x0A2 (extra/zone_c).
 */
#define RECIPE_BYTES 264u
#define RECIPE_MAIN  0x00Cu
#define RECIPE_SIDE  0x0ACu
#define RECIPE_EXTRA 0x0CAu

static int recipe_convert_ids(uint32_t buf) {
    int changed = 0;
    const uint32_t main_n  = psp_read32(buf + 0x0u);
    const uint32_t side_n  = psp_read32(buf + 0x4u);
    const uint32_t extra_n = psp_read32(buf + 0x8u);
    const struct { uint32_t off, n; } groups[] = {
        { RECIPE_MAIN,  main_n  > 80u ? 80u : main_n  },
        { RECIPE_SIDE,  side_n  > 15u ? 15u : side_n  },
        { RECIPE_EXTRA, extra_n > 31u ? 31u : extra_n },
    };
    for (unsigned g = 0; g < 3u; g++) {
        for (uint32_t i = 0; i < groups[g].n; i++) {
            const uint32_t at = buf + groups[g].off + i * 2u;
            const uint16_t id = psp_read16(at);
            const uint16_t to = gx_tf1_to_tf3(id);
            if (to && to != id) { psp_write16(at, to); changed = 1; }
        }
    }
    return changed;
}

static struct {
    uint32_t player;
    uint32_t zone;
    int pending;
    int badges_pushed;
} g_flip_preview;

static void handle_deck_load(void) {
    g_flip_preview.pending = 0;
    g_flip_preview.badges_pushed = 0;
    g_duel_bonuses_translated = 0;
    uint32_t tf1_ptr = psp_cpu.r[PSP_REG_A1];
    uint32_t main_n  = psp_read32(tf1_ptr + 0u);
    uint32_t side_n  = psp_read32(tf1_ptr + 4u);
    uint32_t extra_n = psp_read32(tf1_ptr + 8u);

    if (tf3_test_gx_deck() && psp_cpu.r[PSP_REG_A0] == 0u) {
        static const uint16_t gx_main[] = {
            7661, /* Elemental Hero Prisma */
            7277, /* Crystal Beast Amethyst Cat */
            7280, /* Crystal Beast Ruby Carbuncle */
            7473, /* Gladiator Beast Murmillo */
            7474, /* Gladiator Beast Bestiari */
            7717, /* Arcana Force 0 - The Fool */
            7710, /* Honest */
            7706, /* Allure of Darkness */
            6979, /* Fossil Fusion */
            7285, /* Ancient City - Rainbow Ruins */
            7580, /* Yubel */
            7372, /* Rainbow Dragon */
        };
        static const uint16_t gx_extra[] = {
            7227, /* Elemental Hero Air Neos */
            7613, /* Rainbow Neos */
            6976, /* Cenozoic Fossil Knight - Skullger */
            7443, /* Paleozoic Fossil Knight - Skullking */
        };
        uint32_t at = tf1_ptr;
        const uint32_t n = 40u;
        psp_write32(at + 0x0u, n);
        psp_write32(at + 0x4u, 0u);
        psp_write32(at + 0x8u, 12u);
        for (uint32_t i = 0; i < n; i++)
            psp_write16(at + RECIPE_MAIN + i * 2u,
                        gx_tf1_to_tf3(gx_main[i % (sizeof(gx_main) / sizeof(gx_main[0]))]));
        for (uint32_t i = 0; i < 12u; i++)
            psp_write16(at + RECIPE_EXTRA + i * 2u,
                        gx_tf1_to_tf3(gx_extra[i % (sizeof(gx_extra) / sizeof(gx_extra[0]))]));
        main_n = n;
        side_n = 0u;
        extra_n = 12u;
    }

    if (tf3_trace_post_deck()) g_tf3_post_deck_trace = 1;
    if (g_tf3_post_deck_trace) {
        const uint32_t player = psp_cpu.r[PSP_REG_A0];
        fprintf(stderr, "[DECKLOAD] player=%u main=%u side=%u extra=%u first=%u,%u,%u"
                        " f84=%u,%u,%u fAC=%u,%u,%u fCA=%u,%u,%u\n",
                player, main_n, side_n, extra_n,
                psp_read16(tf1_ptr + RECIPE_MAIN),
                psp_read16(tf1_ptr + RECIPE_MAIN + 2u),
                psp_read16(tf1_ptr + RECIPE_MAIN + 4u),
                psp_read16(tf1_ptr + 0x84u), psp_read16(tf1_ptr + 0x86u),
                psp_read16(tf1_ptr + 0x88u),
                psp_read16(tf1_ptr + 0xACu), psp_read16(tf1_ptr + 0xAEu),
                psp_read16(tf1_ptr + 0xB0u),
                psp_read16(tf1_ptr + 0xCAu), psp_read16(tf1_ptr + 0xCCu),
                psp_read16(tf1_ptr + 0xCEu));
        fprintf(stderr, "[MAINDECK] player=%u ids=", player);
        const uint32_t trace_main_limit = tf3_trace_nova() ? 80u : 10u;
        for (uint32_t i = 0; i < main_n && i < trace_main_limit; i++)
            fprintf(stderr, " %u", psp_read16(tf1_ptr + RECIPE_MAIN + i * 2u));
        fprintf(stderr, "\n");
    }

    /* Convert TF1 -> TF3 IDs on the original TF1 layout first, then clamp counts
     * to 15 and place extra IDs where TF3 duel_deck_set4 reads them (+0xA2/+0xAC). */
    const uint32_t out_sp = psp_cpu.r[PSP_REG_SP] - RECIPE_BYTES;
    psp_cpu.r[PSP_REG_SP] = out_sp;
    for (uint32_t i = 0; i < RECIPE_BYTES; i += 4u)
        psp_write32(out_sp + i, psp_read32(tf1_ptr + i));

    const int converted = recipe_convert_ids(out_sp);
    if (g_tf3_post_deck_trace && converted)
        fprintf(stderr, "[DECKLOAD] player=%u remapped ids\n", psp_cpu.r[PSP_REG_A0]);

    {
        const uint32_t n_extra = psp_read32(out_sp + 8u) > 15u
                                     ? 15u : psp_read32(out_sp + 8u);
        for (uint32_t i = 0; i < n_extra; i++) {
            const uint16_t extra_tf3 = psp_read16(out_sp + RECIPE_EXTRA + i * 2u);
            psp_write16(out_sp + 0xACu + i * 2u, extra_tf3);
            psp_write16(out_sp + 0xA2u + i * 2u, extra_tf3);
        }
    }
    if (psp_read32(out_sp + 4u) > 15u) psp_write32(out_sp + 4u, 15u);
    if (psp_read32(out_sp + 8u) > 15u) psp_write32(out_sp + 8u, 15u);

    psp_cpu.r[PSP_REG_A1] = out_sp;
    call_tf3_export_nid(0x27BCA267u);
    psp_cpu.r[PSP_REG_SP] = out_sp + RECIPE_BYTES;

    /* Post-load state, per the TF3 layout: DuelPlayer +0x10 n_d, +0x304 zone_d
     * (4 bytes/card, id in the low 14 bits), +0x18 n_18, +0x4E4 zone_c. */
    if (g_tf3_post_deck_trace) {
        const uint32_t player = psp_cpu.r[PSP_REG_A0] & 1u;
        const uint32_t base = 0x03849128u + player * 2876u;
        fprintf(stderr, "[DECKSTATE] p=%u n_main=%u n_d=%u n_18=%u zone_d=",
                player, psp_read32(tf1_ptr + 0u), psp_read32(base + 0x10u),
                psp_read32(base + 0x18u));
        for (uint32_t i = 0; i < 5u; i++)
            fprintf(stderr, " %u", psp_read16(base + 0x304u + i * 4u) & 0x3FFFu);
        fprintf(stderr, " zone_c=");
        for (uint32_t i = 0; i < 5u; i++)
            fprintf(stderr, " %u", psp_read16(base + 0x4E4u + i * 4u) & 0x3FFFu);
        fprintf(stderr, " recipe_ids=");
        for (uint32_t i = 0; i < 5u; i++)
            fprintf(stderr, " %u", psp_read16(tf1_ptr + RECIPE_MAIN + i * 2u));
        fputc('\n', stderr);
        fflush(stderr);
    }
}

/* Event 62 has no TF1 draw handler. The bridge consumes it at dispatch. */
#define BRIDGE_FLIP_PREVIEW_EVENT 62u

/* TF3 draw/mgr event id -> TF1 event id. 0 = no TF1 equivalent (drop event).
 * Generated from the draw tables (TF1 0x71AD4, TF3 0x90680) by aligning the
 * handler size and argc; still inferred, not yet validated in-game. */
static const unsigned char g_ev_tf3_to_tf1[0x4F] = {
  0x00, 0x01, 0x02, 0x00, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
  0x0b, 0x0c, 0x00, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
  0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
  0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
  0x00, 0x00, 0x2e, BRIDGE_FLIP_PREVIEW_EVENT, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
  0x37, 0x38, 0x3a, 0x39, 0x3b, 0x3d, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43,
  0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
};

static uint32_t tf3_event_to_tf1(uint32_t ev) {
    return ev < sizeof(g_ev_tf3_to_tf1) ? (uint32_t)g_ev_tf3_to_tf1[ev] : 0u;
}

static uint32_t tf3_req_flag_to_tf1(uint32_t flag) {
    switch (flag) {
    case 12u: /* attack */
    case 34u: /* activation popup: TF3 0x037D9774 waits on the flag passed by draw */
    case 52u: /* flip popup or cut-in: TF3 0x037DB844 */
    case 54u: /* prompt */
    case 56u:
    case 57u: /* summon and set banners */
        return tf3_event_to_tf1(flag);
    default:
        return flag;
    }
}

/* TF1 duel_0884AF20 emits (49, 4, 0xE7, phase_mask); TF3 0x037E64CC
 * emits (54, 5, 0xE7, phase_mask). Keep the engine's mask intact. */
#define PHASE_PROMPT_TEXT_ID 0xE7u
static uint32_t tf3_prompt_kind_to_tf1_raw(uint32_t kind, uint32_t text) {
    switch (kind) {
    case 0:  return 0;  /* notice from a DLG text id -> one OK button */
    case 1:  return 0;  /* notice from a string pointer -> one OK button.
                         * Corpse of Yata-Garasu (0x1776): the NPC picks an
                         * option and this event announces it to the player
                         * ("Draw 1 card from your Deck."). Falling through to
                         * `default` made it kind 1, which dd_ev_49 renders as
                         * Yes/No, asking the player about the opponent's own
                         * choice. */
    case 2:  return 1;  /* formatted yes/no */
    case 3:  return 2;  /* chain confirmation */
    case 4:  return 3;  /* four-entry choice list */
    case 5:  return text == PHASE_PROMPT_TEXT_ID ? 4u : kind;
    case 6:  return text == 0xF1u ? 5u : kind; /* type */
    case 7:  return text == 0xF2u ? 6u : kind; /* attribute */
    case 8:  return text == 0xF7u ? 7u : kind; /* summon position */
    case 9:  return 8;  /* coin */
    case 10: return 9;  /* dice */
    case 11: return 10; /* numeric declaration */
    case 12: return 11; /* clear dialog */
    case 13: return 12; /* selection notice */
    case 14: return 13; /* ritual progress */
    default: return kind;
    }
}

/* TF1 dd_ev_50 kinds 1/2 open a Yes/No with a fixed DLG text; kinds 3/4 open
 * an OK-only notice. Log the translation so a spurious prompt can be traced
 * back to the kind/text the TF3 engine asked for. */
static uint32_t tf3_prompt_kind_to_tf1(uint32_t kind, uint32_t text) {
    const uint32_t out = tf3_prompt_kind_to_tf1_raw(kind, text);
    if (g_tf3_post_deck_trace || tf3_trace_prompt()) {
        static unsigned n;
        if (n++ < 120u)
            fprintf(stderr, "[PROMPT] vb=%llu tf3_kind=%u text=0x%X -> tf1_kind=%u%s\n",
                    (unsigned long long)psp_display_vblanks(), kind, text, out,
                    (out == 1u || out == 2u) ? " (yes/no)" : (out == 3u || out == 4u) ? " (ok)" : "");
    }
    return out;
}

static uint16_t tf3_choice_to_tf1(uint16_t entry) {
    /* TF3 uses 14 bits for the label and bit 15 for availability. */
    if (entry & 0x2000u) return 0; /* TF1 cannot encode this label. */
    return (uint16_t)((entry & 0x1FFFu) | ((entry & 0x8000u) >> 1));
}

/* Last card action menu; independent of the phase mask. */
#define DD_ACT_MENU_ADDR 0x034542B8u
#define DD_ACT_TAB_ADDR  0x03072110u
#define DD_TEXT_ADDR     0x03019BC8u
#define DD_DRAW_GP       0x030807E0u

static uint32_t duel_text_ptr(uint32_t text) {
    if ((text & 0xFFFF0000u) != 0u) return text;
    uint32_t gp = 0;
    const uint32_t addr = duel_eboot_export_addr(0xCB6030CFu, &gp); /* dlg_get_text */
    if (!addr) return 0u;
    const uint32_t saved_v0 = psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_A0] = text;
    call_guest_addr(addr, gp);
    const uint32_t p = psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_V0] = saved_v0;
    return p;
}

/* TF3 and TF1 number their dialog strings independently. An id that exists in
 * TF3 can be empty in the TF1 table (0x14F, the tribute prompt), so the draw
 * shows a blank box. Fall back to the TF3 string for that id. */
static int duel_text_is_empty(uint32_t text) {
    const uint32_t p = duel_text_ptr(text);
    return p == 0u || psp_read16(p) == 0u;
}

/* TF3 passes text pointers into the guest stack (0x09FFF...). The draw reads
 * them later, after that frame is reused, which leaves blank or stale prompts.
 * Copy pointer texts into a rotating scratch slot at event time. */
#define TF3_TEXT_SLOT_BASE (TF3_SCRATCH_BASE + 0x4000u)
#define TF3_TEXT_SLOT_SIZE 0x800u
#define TF3_TEXT_SLOTS     8u

static uint32_t tf3_stable_text(uint32_t text) {
    if ((text & 0xFFFF0000u) == 0u) return text; /* DLG id: already stable */
    if (text < 0x00100000u || text >= 0x0B000000u) return text;
    static unsigned slot;
    const uint32_t dst =
        TF3_TEXT_SLOT_BASE + (slot++ % TF3_TEXT_SLOTS) * TF3_TEXT_SLOT_SIZE;
    const uint32_t limit = (TF3_TEXT_SLOT_SIZE / 2u) - 1u;
    uint32_t i = 0;
    for (; i < limit; i++) {
        const uint16_t ch = psp_read16(text + i * 2u);
        psp_write16(dst + i * 2u, ch);
        if (!ch) break;
    }
    if (i == limit) psp_write16(dst + i * 2u, 0u);
    return dst;
}

static void duel_dump_text(const char *tag, uint32_t text) {
    const uint32_t p = duel_text_ptr(text);
    fprintf(stderr, "%s text=0x%X ptr=0x%X \"", tag, text, p);
    if (p >= 0x00100000u && p < 0x0B000000u) {
        for (unsigned i = 0; i < 120u; i++) {
            const uint16_t ch = psp_read16(p + i * 2u);
            if (!ch) break;
            fputc(ch < 128u ? (int)ch : '?', stderr);
        }
    }
    fprintf(stderr, "\"\n");
}

static void duel_dump_act(void) {
    static unsigned act_log;
    if (act_log >= 20u) return;
    act_log++;
    const uint32_t n = psp_read32(DD_ACT_MENU_ADDR + 0x04u);
    const uint32_t bits = psp_read32(DD_ACT_MENU_ADDR + 0x94u);
    const uint32_t w = psp_read32(DD_ACT_MENU_ADDR + 0x8Cu);
    fprintf(stderr, "[ACT] vb=%llu n=%u bits=0x%X w=0x%X acts=",
            (unsigned long long)psp_display_vblanks(), n, bits, w);
    for (uint32_t i = 0; i < n && i < 32u; i++)
        fprintf(stderr, " %u", psp_read32(DD_ACT_MENU_ADDR + 0x08u + i * 4u));
    fprintf(stderr, "\n");
    fprintf(stderr, "[ACT] hook=0x%X src=", psp_read32(DD_ACT_MENU_ADDR - 4u));
    for (uint32_t i = 0; i < 8u; i++)
        fprintf(stderr, " %08X", psp_read32(DD_ACT_MENU_ADDR + i * 4u));
    fprintf(stderr, "\n");
    for (uint32_t r = 0; r < 15u; r++) {
        const uint32_t base = DD_ACT_TAB_ADDR + r * 16u;
        const uint32_t text = psp_read32(base + 0u);
        const uint32_t u = psp_read32(base + 4u);
        const uint32_t v = psp_read32(base + 8u);
        const uint32_t act = psp_read32(base + 12u);
        const uint32_t saved_v0 = psp_cpu.r[PSP_REG_V0];
        psp_cpu.r[PSP_REG_A0] = text;
        call_guest_addr(DD_TEXT_ADDR, DD_DRAW_GP);
        const uint32_t p = psp_cpu.r[PSP_REG_V0];
        psp_cpu.r[PSP_REG_V0] = saved_v0;
        fprintf(stderr, "[ACT] row %u text=%u u=%u v=%u act=%u \"",
                r, text, u, v, act);
        if (p >= 0x00100000u && p < 0x0B000000u) {
            for (unsigned i = 0; i < 80u; i++) {
                const uint16_t ch = psp_read16(p + i * 2u);
                if (!ch) break;
                fputc(ch < 128u ? (int)ch : '?', stderr);
            }
        }
        fprintf(stderr, "\"\n");
    }
    fflush(stderr);
}

/* Last game_state2 pointer seen from the TF3 side. Lets the result dump read
 * the struct without calling back into the guest. */
static uint32_t g_gs2_base;

uint32_t tf3_bridge_gs2_base(void) { return g_gs2_base; }

/* game_state2_get() is a pure getter (src/BOOT/cfg.c:302 -> g_game.game_state2)
 * but it only reaches the bridge as an import during the manual flow. In the
 * automated duel it never did, g_gs2_base stayed 0 and the whole result-screen
 * fix was skipped. Resolve it once here. */
static uint32_t resolve_gs2(void) {
    if (g_gs2_base) return g_gs2_base;
    uint32_t gp = 0;
    const uint32_t addr = duel_eboot_export_addr(0x885E0882u, &gp);
    if (!addr) return 0;
    call_guest_addr(addr, gp);
    const uint32_t v = psp_cpu.r[PSP_REG_V0];
    if (v >= 0x08000000u && v < 0x0C000000u) g_gs2_base = v;
    if (g_tf3_post_deck_trace)
        fprintf(stderr, "[VICTORY] gs2 resolved late: 0x%X\n", g_gs2_base);
    return g_gs2_base;
}

static int tf3_bridge_apply_victory_bonus(uint32_t gs2, uint32_t source) {
    if (g_duel_bonuses_translated) return 0;
    const uint32_t result = psp_read32(source + 0x20u);
    if (result < 1u || result > 3u) return 0;

    for (uint32_t off = 0; off < 0x0DCu; off += 4u)
        psp_write32(gs2 + off, psp_read32(source + off));
    for (uint32_t k = 0; k < 82u; k++)
        psp_write32(gs2 + 0x0DCu + k * 4u, psp_read32(source + 0x100u + k * 4u));
    psp_write32(gs2 + 0x224u, 0u);
    for (uint32_t k = 0; k < 118u; k++)
        psp_write8(gs2 + 0x228u + k, psp_read8(source + 0x24Cu + k));

    /* Preserve native bonuses and fill the standard rows if they came back empty. */
    if (result == 2u || result == 3u) {
        const uint32_t base_row = gs2 + (result == 2u ? 0x0E8u : 0x0ECu);
        if (!psp_read32(base_row))
            psp_write32(base_row, result == 2u ? 10u : 50u);
    }

    const uint32_t turn_row = gs2 + 0x0F0u;
    if (!psp_read32(turn_row)) {
        /* TF3's victory totals (0x03601BEC) always use 2 per turn. */
        uint32_t multiplier = 2u;
        if (result != 1u) {
            uint32_t gp = 0;
            const uint32_t get_mode = duel_eboot_export_addr(0x60ABAEE7u, &gp);
            if (get_mode) {
                const uint32_t saved_a0 = psp_cpu.r[PSP_REG_A0];
                psp_cpu.r[PSP_REG_A0] = 6u;
                call_guest_addr(get_mode, gp);
                if ((psp_cpu.r[PSP_REG_V0] & 0xFFFFu) == 13u)
                    multiplier = 1u;
                psp_cpu.r[PSP_REG_A0] = saved_a0;
            } else {
                multiplier = 0u;
            }
        }
        psp_write32(turn_row, multiplier * (psp_read32(0x0384BDE0u) + 1u));
    }
    if (g_tf3_post_deck_trace)
        fprintf(stderr, "[DUEL_BONUS] result=%u loss=%u draw=%u turn=%u\n", result,
                psp_read32(gs2 + 0x0E8u), psp_read32(gs2 + 0x0ECu), psp_read32(turn_row));

    g_duel_bonuses_translated = 1;
    return 1;
}

/* One-shot deck snapshot taken once the engine has run its own deck load. */
static void dump_deck_live(void) {
    static int done;
    if (done || !g_tf3_post_deck_trace) return;
    done = 1;
    for (uint32_t p = 0; p < 2u; p++) {
        const uint32_t b = 0x03849128u + p * 2876u;
        char line[512];
        int n = snprintf(line, sizeof line, "[DECKLIVE] p=%u n_d=%u n_18=%u zone_d=",
                         p, psp_read32(b + 0x10u), psp_read32(b + 0x18u));
        for (uint32_t i = 0; i < 5u && n > 0 && (size_t)n < sizeof line; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %u",
                          psp_read16(b + 0x304u + i * 4u) & 0x3FFFu);
        n += snprintf(line + n, sizeof line - (size_t)n, " zone_c=");
        for (uint32_t i = 0; i < 15u && (size_t)n < sizeof line; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %u",
                          psp_read16(b + 0x4E4u + i * 4u) & 0x3FFFu);
        /* One write, so a concurrent thread cannot splice its own output
         * into the middle of the deck list. */
        /* Leading newline: other threads write multi-part lines (e.g. the
         * [DETAIL] dump), so ours could land inside a partial line and become
         * unparseable. */
        fprintf(stderr, "\n%s\n", line);
    }
    fflush(stderr);
}

/* TF3 draw event 20 refreshes one hand object by UID (zero means all).
 * TF1 event 18 discards that UID and applies a single flag to the whole hand. */
static void update_hand_visibility(uint32_t player, uint32_t uid) {
    psp_cpu.r[PSP_REG_A0] = player;
    call_guest_addr(0x03062744u, DD_DRAW_GP); /* dv_player_to_side */
    psp_cpu.r[PSP_REG_A0] = psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_A1] = 0x0Bu;
    call_guest_addr(0x0300C600u, DD_DRAW_GP); /* dd_zone_list */
    const uint32_t head = psp_cpu.r[PSP_REG_V0];
    if (!head || !psp_mem_unwatched_ptr(head, 8u)) return;

    /* The TF3 hand starts at DuelPlayer+0x124 and ends at +0x304. */
    uint32_t card = psp_read32(head + 4u);
    for (uint32_t index = 0; card != head && index < (0x304u - 0x124u) / 4u; ++index) {
        if (!card || !psp_mem_unwatched_ptr(card, 0xB0u)) break;
        const uint32_t card_uid = psp_read32(card + 0x88u);
        if (uid == 0 || card_uid == uid) {
            psp_cpu.r[PSP_REG_A0] = player;
            psp_cpu.r[PSP_REG_A1] = index;
            if (call_tf3_export_nid(0xD89D3F3Bu)) { /* per-index hand face-up */
                const uint32_t face = psp_cpu.r[PSP_REG_V0] & 1u;
                const uint32_t flags = psp_read8(card + 0x8Du);
                psp_write8(card + 0x8Du, (uint8_t)((flags & ~8u) | (face << 3)));
                if (tf3_trace_abi_vis())
                    fprintf(stderr, "[HAND_VIS] vb=%llu player=%u requested_uid=%u uid=%u index=%u face=%u\n",
                            (unsigned long long)psp_display_vblanks(), player, uid, card_uid, index, face);
            }
        }
        card = psp_read32(card + 4u);
    }
}

static void trace_future_fusion_slots(uint64_t vb, uint32_t step) {
    /* TF3 0x03607FE8 traverses 10-byte records and reads next at +8. */
    const uint32_t pool = 0x0384B3CCu;
    const uint32_t limit = (0x0384BDD8u - pool) / 10u;
    uint32_t present = 0;
    for (uint32_t player = 0; player < 2u; ++player) {
        const uint32_t field = 0x03849128u + 2876u * player + 0x34u;
        for (uint32_t zone = 0; zone < 11u; ++zone) {
            const uint32_t slot = field + 0x14u * zone;
            if ((psp_read16(slot) & 0x3FFFu) != 0x1A7Eu) continue;
            present++;
            uint32_t index = psp_read16(slot + 0xAu);
            fprintf(stderr, "[FF_SLOT] vb=%llu step=%u player=%u zone=%u slot_flags=0x%04X head=%u\n",
                    (unsigned long long)vb, step, player, zone, psp_read16(slot + 8u), index);
            for (uint32_t seen = 0; index && index < limit && seen < limit; ++seen) {
                const uint32_t effect = pool + 10u * index;
                const uint32_t next = psp_read16(effect + 8u);
                fprintf(stderr, "[FF_EFFECT] vb=%llu index=%u src=0x%04X flags=0x%04X arg=0x%04X raw6=0x%04X next=%u\n",
                        (unsigned long long)vb, index, psp_read16(effect), psp_read16(effect + 2u),
                        psp_read16(effect + 4u), psp_read16(effect + 6u), next);
                index = next;
            }
            if (index)
                fprintf(stderr, "[FF_EFFECT] vb=%llu traversal stopped at index=%u\n",
                        (unsigned long long)vb, index);
        }
    }
    if (!present)
        fprintf(stderr, "[FF_SLOT] vb=%llu step=%u present=0\n", (unsigned long long)vb, step);
}

static void start_flip_preview(uint32_t player, uint32_t zone, uint32_t mode) {
    /* TF3 event 51 defers mode 1 until a later preview requests the cut-in. */
    if (mode == 1u) {
        g_flip_preview.player = player;
        g_flip_preview.zone = zone;
        g_flip_preview.pending = 1;
        return;
    }
    if (mode == 2u) {
        psp_cpu.r[PSP_REG_A0] = g_flip_preview.pending ? g_flip_preview.player : player;
        psp_cpu.r[PSP_REG_A1] = g_flip_preview.pending ? g_flip_preview.zone : zone;
        psp_cpu.r[PSP_REG_A2] = 1u;
        call_guest_addr(0x03014EE4u, DD_DRAW_GP); /* enable the TF1 flip cut-in */
    }
    psp_cpu.r[PSP_REG_A0] = player;
    call_guest_addr(0x03062744u, DD_DRAW_GP); /* dv_player_to_side */
    psp_cpu.r[PSP_REG_A0] = psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_A1] = zone;
    psp_cpu.r[PSP_REG_A2] = mode;
    call_guest_addr(0x0300916Cu, DD_DRAW_GP); /* dd_hl_push */
    g_flip_preview.badges_pushed = 1;
    g_flip_preview.pending = 0;
}

static void clear_finished_chain_badges(void) {
    if ((!g_flip_preview.badges_pushed && !g_flip_preview.pending) ||
        psp_read32(0x03847A34u) != 0u)
        return;

    /* TF3 drops its last entry after the pop animation is acknowledged. */
    if (g_flip_preview.badges_pushed) {
        for (uint32_t addr = 0x03450388u; addr <= 0x03450450u; addr += 4u)
            psp_write32(addr, 0u); /* badge cells, history and depth */
    }
    g_flip_preview.badges_pushed = 0;
    g_flip_preview.pending = 0;
}

static int duel_tf3_handle_export_body(uint32_t nid) {
    switch (nid) {
    /* Deck loading: 264-byte TF1 struct -> 192-byte TF3 struct + Konami IDs */
    case 0x27BCA267u:
        handle_deck_load();
        return 1;

    case 0xACB25738u: /* The manager still has all three TF3 event arguments. */
        if (psp_cpu.r[PSP_REG_RA] != 0x02201BBCu)
            return 0;
        if (psp_arg(0) == BRIDGE_FLIP_PREVIEW_EVENT) {
            start_flip_preview(psp_arg(1), psp_arg(2), psp_arg(3));
        } else if (psp_arg(0) == 47u && g_flip_preview.pending) {
            if (tf3_trace_abi_vis())
                fprintf(stderr, "[FLIP_PREVIEW] vb=%llu deferred player=%u zone=%u mode=%u\n",
                        (unsigned long long)psp_display_vblanks(), psp_arg(1), psp_arg(2), psp_arg(3));
        } else if (psp_arg(0) == 18u && psp_arg(1) <= 1u && psp_lookup(0x0300C600u)) {
            update_hand_visibility(psp_arg(1), psp_arg(3));
        } else {
            return 0;
        }
        psp_ret(1u);
        return 1;

    /* Pop duel event from tf3eng and translate its ID to the TF1 numbering.
     * The engine keeps the raw event in 0x03820A14; only the caller's buffer
     * is rewritten, so the double pop in modduel_mgr stays idempotent. */
    case 0x04F712B8u: {
        uint32_t out_ptr = psp_cpu.r[PSP_REG_A0];
        const int popped = call_tf3_export_nid(nid);
        clear_finished_chain_badges();
        dump_deck_live();
        /* Future Fusion (card 0x1A7E) never resolves. Its effect handler
         * 0x036DAFF4 switches on the engine step at 0x03847B60; log every
         * change around a duel so we can see whether it ever reaches 121+. */
        {
            static uint32_t last_step = 0xFFFFFFFFu;
            const uint32_t step = psp_read32(0x03847B60u);
            if (step != last_step) {
                if (tf3_trace_ff() && (step == 109u || (last_step == 109u && step == 0u)))
                    trace_future_fusion_slots(psp_display_vblanks(), step);
                last_step = step;
                if (g_tf3_post_deck_trace || tf3_trace_step())
                    fprintf(stderr, "[STEP] vb=%llu step=%u turn=%u phase=%u active_player=%u\n",
                            (unsigned long long)psp_display_vblanks(), step,
                            psp_read32(0x0384BDE0u), psp_read32(0x0384BDE8u),
                            psp_read32(0x0384BDD8u));
            }
        }
        if (popped && out_ptr >= 0x08000000u && out_ptr < 0x0C000000u) {
            uint32_t tf3_ev = psp_read32(out_ptr + 0u);
            uint32_t tf1_ev = tf3_event_to_tf1(tf3_ev);
            if (tf3_trace_ff()) {
                static uint32_t last_turn = UINT32_MAX, last_phase = UINT32_MAX, last_player = UINT32_MAX;
                const uint32_t turn = psp_read32(0x0384BDE0u);
                const uint32_t phase = psp_read32(0x0384BDE8u);
                const uint32_t player = psp_read32(0x0384BDD8u);
                if (turn != last_turn || phase != last_phase || player != last_player) {
                    last_turn = turn;
                    last_phase = phase;
                    last_player = player;
                    fprintf(stderr, "[FF_PHASE] vb=%llu turn=%u phase=%u active_player=%u\n",
                            (unsigned long long)psp_display_vblanks(), turn, phase, player);
                    trace_future_fusion_slots(psp_display_vblanks(), psp_read32(0x03847B60u));
                }
            }
            if (g_tf3_post_deck_trace || tf3_trace_bridge()) {
                static unsigned ev_log;
                if (ev_log < 12000u) {
                    ev_log++;
                    fprintf(stderr, "[EVT_MAP] vb=%llu tf3=%u -> tf1=%u%s args=[0x%X,0x%X,0x%X]\n",
                            (unsigned long long)psp_display_vblanks(), tf3_ev, tf1_ev,
                            tf1_ev ? "" : " (drop)",
                            psp_read32(out_ptr + 4u), psp_read32(out_ptr + 8u),
                            psp_read32(out_ptr + 12u));
                    fflush(stderr);
                }
            }
            if ((g_tf3_post_deck_trace || tf3_trace_abi_ev()) &&
                (tf3_ev == 4u || tf3_ev == 55u || tf3_ev == 60u)) {
                static unsigned abi_ev_log;
                if (abi_ev_log < 200u) {
                    abi_ev_log++;
                    fprintf(stderr, "[ABI_EVT] vb=%llu tf3=%u tf1=%u args=[0x%X,0x%X,0x%X] state=%u active=%u view=%u turn=%u phase=%u\n",
                            (unsigned long long)psp_display_vblanks(), tf3_ev, tf1_ev,
                            psp_read32(out_ptr + 4u), psp_read32(out_ptr + 8u),
                            psp_read32(out_ptr + 12u),
                            psp_read32(0x0384BE4Cu), psp_read32(0x0384BE54u),
                            psp_read32(0x03821594u), psp_read32(0x0384BDD8u),
                            psp_read32(0x0384BDE8u));
                    fprintf(stderr, "[ABI_EVT] prompt=");
                    for (int i = 0; i < 16; i++)
                        fprintf(stderr, " %08X", psp_read32(0x0384BE40u + (uint32_t)i * 4u));
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
            }
            if (tf3_ev == 12u && tf1_ev == 11u) {
                /* TF3 packs attacker as player | zone << 8; TF1 tracks player separately. */
                const uint32_t attacker = psp_read32(out_ptr + 4u);
                const uint32_t zone = (attacker >> 8) & 0xFFu;
                if (g_tf3_post_deck_trace || tf3_trace_abi_ev())
                    fprintf(stderr, "[ABI_ATTACK] vb=%llu attacker=0x%X player=%u zone=%u target=[0x%X,0x%X]\n",
                            (unsigned long long)psp_display_vblanks(), attacker, attacker & 0xFFu,
                            zone, psp_read32(out_ptr + 8u), psp_read32(out_ptr + 12u));
                psp_write32(out_ptr + 4u, zone);
            }
            if (tf3_ev == 56u && tf1_ev == 51u) {
                /* TF3 0x037DDDEC passes [packed location, uid, flags]. TF1 wants [player, zone, uid]. */
                const uint32_t loc = psp_read32(out_ptr + 4u);
                const uint32_t uid = psp_read32(out_ptr + 8u);
                const uint32_t player = loc & 1u;
                const uint32_t zone = (loc >> 1) & 0x1Fu;
                if (g_tf3_post_deck_trace || tf3_trace_abi_ev()) {
                    const uint32_t field = 0x03849128u + 2876u * player + 0x34u;
                    const uint32_t card = zone < 11u ? psp_read16(field + 0x14u * zone) & 0x3FFFu : 0;
                    fprintf(stderr, "[ABI_BANNER] vb=%llu loc=0x%X player=%u zone=%u uid=%u field_card=0x%04X\n",
                            (unsigned long long)psp_display_vblanks(), loc, player, zone, uid, card);
                }
                psp_write32(out_ptr + 4u, player);
                psp_write32(out_ptr + 8u, zone);
                psp_write32(out_ptr + 12u, uid);
            }
            if (tf3_ev == 34u || tf3_ev == 72u) {
                /* args[1] (out_ptr + 8) é card_id TF3 -> converter para TF1 */
                const uint16_t tf3_kid = (uint16_t)(psp_read32(out_ptr + 8u) & 0x3FFFu);
                if (tf3_kid != 0u)
                    psp_write32(out_ptr + 8u, (uint32_t)gx_tf3_to_tf1(tf3_kid));
            }
            if ((tf3_ev == 68u && tf1_ev == 64u) || (tf3_ev == 71u && tf1_ev == 67u)) {
                /* TF3 envia UID (ex.: 0x8005, 0x8037) em out_ptr+8; TF1 events 64 e 67 esperam card_id TF1 */
                const uint32_t raw_a1 = psp_read32(out_ptr + 8u);
                const uint32_t uid = raw_a1 & 0x7FFFu;
                if (uid != 0u && uid < 256u) {
                    const uint32_t saved_a0 = psp_cpu.r[PSP_REG_A0];
                    const uint32_t saved_v0 = psp_cpu.r[PSP_REG_V0];
                    psp_cpu.r[PSP_REG_A0] = uid;
                    if (call_tf3_export_nid(0x7F9F4CA7u)) { /* duel_list_id(uid) */
                        const uint16_t tf3_kid = (uint16_t)(psp_cpu.r[PSP_REG_V0] & 0x3FFFu);
                        if (tf3_kid != 0u) {
                            const uint32_t tf1_kid = (uint32_t)gx_tf3_to_tf1(tf3_kid);
                            psp_write32(out_ptr + 8u, tf1_kid);
                            if (g_tf3_post_deck_trace || tf3_trace_bridge() || tf3_trace_abi_ev()) {
                                fprintf(stderr, "[ABI_CUTIN] vb=%llu tf3=%u->tf1=%u a1=0x%X uid=%u tf3_kid=0x%04X -> tf1_kid=%u\n",
                                        (unsigned long long)psp_display_vblanks(),
                                        tf3_ev, tf1_ev, raw_a1, uid, tf3_kid, tf1_kid);
                                fflush(stderr);
                            }
                        }
                    }
                    psp_cpu.r[PSP_REG_A0] = saved_a0;
                    psp_cpu.r[PSP_REG_V0] = saved_v0;
                }
                /* TF1 dmgr_ev_summon (ev 67) só trata args[3] em {0, 1, 2}; evitar lixo na stack */
                if (tf3_ev == 71u && psp_read32(out_ptr + 12u) > 2u)
                    tf1_ev = 0u; /* drop seguro para subtipos exclusivos do TF3 */
            }
            if (tf1_ev == 49u) {
                const uint32_t kind = psp_read32(out_ptr + 4u);
                const uint32_t text = psp_read32(out_ptr + 8u);
                const uint32_t arg = psp_read32(out_ptr + 12u);
                const uint32_t mapped = tf3_prompt_kind_to_tf1(kind, text);
                if (g_tf3_post_deck_trace || tf3_trace_abi_ev()) {
                    static unsigned ev49_log;
                    if (ev49_log < 4000u) {
                        ev49_log++;
                        fprintf(stderr, "[EV49] vb=%llu tf3=%u kind=%u text=0x%X arg=0x%X -> kind=%u\n",
                                (unsigned long long)psp_display_vblanks(), tf3_ev,
                                kind, text, arg, mapped);
                        duel_dump_text("[EV49] msg", text);
                        duel_dump_act();
                        fflush(stderr);
                    }
                }
                if (mapped != kind)
                    psp_write32(out_ptr + 4u, mapped);
                /* Resolve the text the way the TF1 draw will, and swap in the
                 * TF3 string when that id is empty in the TF1 table. */
                uint32_t display = text;
                if ((text & 0xFFFF0000u) == 0u && duel_text_is_empty(text)) {
                    display = tf3_dlg_str_ptr(text);
                    if ((g_tf3_post_deck_trace || tf3_trace_abi_ev()))
                        fprintf(stderr, "[EV49] TF1 text %u empty, using TF3 0x%X\n", text, display);
                }
                /* The draw reads pointer text after the guest stack frame that
                 * holds it is gone: keep a stable copy. */
                const uint32_t stable = tf3_stable_text(display);
                if (stable != text)
                    psp_write32(out_ptr + 8u, stable);
            }
            if (tf1_ev == 50u && (g_tf3_post_deck_trace || tf3_trace_abi_ev())) {
                /* Who is choosing? Thunder Dragon's NPC search opened the
                 * player's selection UI, so measure the turn owner here. */
                fprintf(stderr, "[EV50] vb=%llu player=%u kind=%u arg2=0x%X turn_player=%u turn=%u phase=%u\n",
                        (unsigned long long)psp_display_vblanks(),
                        psp_read32(out_ptr + 4u), psp_read32(out_ptr + 8u),
                        psp_read32(out_ptr + 12u),
                        psp_read32(0x0384BDD8u), psp_read32(0x0384BDE0u),
                        psp_read32(0x0384BDE8u));
            }
            if (tf1_ev != tf3_ev)
                psp_write32(out_ptr + 0u, tf1_ev);
        }
        return 1;
    }

    case 0xB24BFFEBu: { /* duel_list2_get, only the TF1 choice widget */

        const uint32_t ra = psp_cpu.r[PSP_REG_RA];
        if (ra != 0x0301F74Cu && ra != 0x0301F75Cu && ra != 0x0301F93Cu)
            return 0;
        if (!g_tf3_scratch_mapped) return 0;
        const uint32_t src = 0x03848514u; /* return of TF3 0x036009F4 */
        const uint32_t dst = TF3_SCRATCH_BASE + 0x3100u;
        const uint32_t n = psp_read32(src + 0x0Cu);
        psp_write32(dst + 0x0Cu, n <= 4u ? n : 0u);
        /* TF3 added a header word at +0x10, before the four entries. */
        for (uint32_t off = 0x10u; off < 0x20u; off += 4u)
            psp_write32(dst + off, tf3_choice_to_tf1(psp_read16(src + off + 4u)));
        psp_ret(dst);
        return 1;
    }

    /* duel_field_stats: TF3 0x03623EF8 fills the TF3 layout. Run it into the
     * mapped TF3 scratch shadow, then translate to the caller's TF1 buffer. */
    case 0x22CC13E9u: {
        const uint32_t player = psp_cpu.r[PSP_REG_A0];
        const uint32_t zone = psp_cpu.r[PSP_REG_A1];
        const uint32_t out_ptr = psp_cpu.r[PSP_REG_A2];
        const uint32_t shadow = TF3_SCRATCH_BASE + 0x2000u;
        for (uint32_t i = 0; i < 0x40u; i += 4u) psp_write32(shadow + i, 0u);
        psp_cpu.r[PSP_REG_A0] = player;
        psp_cpu.r[PSP_REG_A1] = zone;
        psp_cpu.r[PSP_REG_A2] = shadow;
        call_guest_addr(0x03623EF8u, 0x03828A00u);
        const uint32_t v = psp_cpu.r[PSP_REG_V0];
        uint32_t dst = out_ptr;
        uint8_t *dp = (dst >= 0x00100000u && dst < 0x0B000000u)
                          ? psp_mem_unwatched_ptr(dst, sizeof(Tf1DuelStats)) : NULL;
        if (!dp && v >= 0x00100000u && v < 0x0B000000u) {
            dst = v;
            dp = psp_mem_unwatched_ptr(dst, sizeof(Tf1DuelStats));
        }
        const Tf3DuelStats *s3 =
            (const Tf3DuelStats *)psp_mem_unwatched_ptr(shadow, sizeof(Tf3DuelStats));
        if (g_tf3_post_deck_trace || tf3_trace_abi_stats()) {
            static unsigned stats_log;
            if (stats_log < 40u) {
                stats_log++;
                fprintf(stderr, "[STATS] player=%u zone=%u out=0x%X v0=0x%X dst=0x%X s3={id=0x%X lvl=%d atk=%d def=%d}\n",
                        player, zone, out_ptr, v, dst,
                        s3 ? (unsigned)s3->id : 0u, s3 ? s3->level : -1,
                        s3 ? s3->atk : -1, s3 ? s3->def : -1);
                fflush(stderr);
            }
        }
        if (dp && s3) {
            Tf1DuelStats t1;
            t1.id = (s3->id > 0) ? (int)gx_tf3_to_tf1((uint16_t)(s3->id & 0x3FFFu)) : 0;
            t1.attr = s3->attr;
            t1.kind = s3->kind;
            t1.mask = s3->mask;
            t1.level = s3->level;
            t1.atk = s3->atk;
            t1.def = s3->def;
            t1.batk = s3->batk;
            t1.bdef = s3->bdef;
            t1.imm = s3->imm;
            t1.imm2 = s3->imm2;
            memcpy(dp, &t1, sizeof t1);
            psp_cpu.r[PSP_REG_V0] = dst;
        } else {
            psp_cpu.r[PSP_REG_V0] = 0u;
        }
        return 1;
    }

    /* Shared TF1/TF3 libduel_eng turn, controller, and zone visibility getters.
     * Without forwarding, TF1 duel.prx reads uninitialized zeroed globals
     * (ctrl=[0,0], zone_flags=0), causing opponent summons and animations to
     * render face-down. */
    case 0x488D2E16u: /* duel_view_player_get */
    case 0xCED49E0Eu: /* duel_view_opponent_get */
    case 0xA2E0910Bu: /* duel_is_view_player */
    case 0x83DF7D79u: /* duel_slot_link */
    case 0xC07D25E0u: /* duel_unk2c */
    case 0x284BC030u: /* duel_slot_faceup(player, zone) */
    case 0x4465C2D2u: /* 0x030628B4 wrapper */
    case 0x46FF14F7u: /* is_human_player(side) */
    case 0x7D3DB7D3u: /* 0x030628E4 wrapper */
        call_tf3_export_nid(nid);
        return 1;

    /* Shared exports that return a Konami ID -> translate TF3 -> TF1_ext */
    case 0x7F9F4CA7u:
    case 0x8D8BC9DEu:
    case 0xFC108C9Fu: {
        if (call_tf3_export_nid(nid)) {
            uint16_t tf3_kid = (uint16_t)(psp_cpu.r[PSP_REG_V0] & 0x3FFFu);
            psp_ret((uint32_t)gx_tf3_to_tf1(tf3_kid));
        }
        return 1;
    }

    case 0xD435B42Du: { /* duel_chain_get */
        if (!g_tf3_scratch_mapped) return 0;
        if (call_tf3_export_nid(nid)) {
            const uint32_t src = psp_cpu.r[PSP_REG_V0];
            if (!src) return 1;
            const uint32_t dst = TF3_SCRATCH_CHAIN_ADDR;
            for (uint32_t off = 0u; off < 0x48Cu; off += 4u)
                psp_write32(dst + off, psp_read32(src + off));
            for (uint32_t i = 0u; i < 16u; i++) {
                const uint32_t e = dst + 0x300u + i * 24u;
                const uint16_t tf3_kid = psp_read16(e) & 0x3FFFu;
                if (tf3_kid)
                    psp_write16(e, gx_tf3_to_tf1(tf3_kid));
            }
            if (psp_cpu.r[PSP_REG_RA] == 0x03014FDCu) {
                /* The TF1 pop cut-in tests bit 2 where TF3 tests bits 2 and 3. */
                const uint32_t count = psp_read32(dst + 0x480u);
                if (count > 0u && count <= 16u) {
                    const uint32_t flags_addr = dst + 0x304u + (count - 1u) * 24u;
                    const uint8_t flags = psp_read8(flags_addr);
                    psp_write8(flags_addr, (uint8_t)(flags | ((flags & 8u) >> 1)));
                }
            }
            for (uint32_t off = 0x48Cu; off < 0x6D0u; off += 4u)
                psp_write32(dst + off, psp_read32(src + off + 4u));
            psp_ret(dst);
        }
        return 1;
    }

    case 0xF2033054u:
    case 0x4EA998D4u: {
        const unsigned reg = nid == 0xF2033054u ? PSP_REG_A3 : PSP_REG_A2;
        const uint32_t pointer = psp_cpu.r[reg];
        const uint32_t offset = pointer - TF3_SCRATCH_CHAIN_ADDR;
        if (!g_tf3_scratch_mapped || offset < 0x300u || offset >= 0x480u ||
            (offset - 0x300u) % 24u != 0u)
            return 0;

        /* TF3 compares the entry against native IDs and flags. */
        psp_cpu.r[reg] = 0x038475B4u + offset;
        call_tf3_export_nid(nid);
        psp_cpu.r[reg] = pointer;
        return 1;
    }

    case 0xE9A64BDFu: { /* duel_special_get -> TF3 (base+0x30, stride 72) to TF1 (base+0x2C, stride 56) */
        if (!g_tf3_scratch_mapped) return 0;
        if (call_tf3_export_nid(nid)) {
            const uint32_t src = psp_cpu.r[PSP_REG_V0];
            if (!src) return 1;
            const uint32_t dst = TF3_SCRATCH_SPECIAL_ADDR;
            for (uint32_t off = 0u; off < 0x100u; off += 4u)
                psp_write32(dst + off, 0u);
            for (uint32_t off = 0u; off < 0x2Cu; off += 4u)
                psp_write32(dst + off, psp_read32(src + off));
            for (uint32_t p = 0u; p < 2u; p++) {
                const uint32_t s3 = src + 0x30u + p * 72u;
                const uint32_t d1 = dst + 0x2Cu + p * 56u;
                for (uint32_t off = 0u; off < 0x10u; off += 4u)
                    psp_write32(d1 + off, psp_read32(s3 + off));
                psp_write32(d1 + 0x1Cu, psp_read32(s3 + 0x24u));
                psp_write32(d1 + 0x20u, psp_read32(s3 + 0x28u));
            }
            for (uint32_t off = 0u; off < 0x24u; off += 4u)
                psp_write32(dst + 0x9Cu + off, psp_read32(src + 0xC0u + off));
            psp_ret(dst);
        }
        return 1;
    }

    /* Missing NIDs mapped to TF3 libduel_eng_rel equivalents */
    case 0xF17833FEu: /* card in zone (player, zone, a2) -> kid */
        if (call_tf3_export_nid(0xA373F1A2u)) {
            uint16_t tf3_kid = (uint16_t)(psp_cpu.r[PSP_REG_V0] & 0x3FFFu);
            psp_ret((uint32_t)gx_tf3_to_tf1(tf3_kid));
        }
        return 1;

    case 0xEA6BA0BAu: /* effective zone card ID (aliases) -> kid */
        if (call_tf3_export_nid(0x7C892733u)) {
            uint16_t tf3_kid = (uint16_t)(psp_cpu.r[PSP_REG_V0] & 0x3FFFu);
            psp_ret((uint32_t)gx_tf3_to_tf1(tf3_kid));
        }
        return 1;

    case 0x5A3EC223u:
        call_tf3_export_nid(0x4C48ABC4u);
        return 1;

    case 0x5030A806u:
        call_tf3_export_nid(0xC6996E11u);
        return 1;

    case 0xAFF0389Du: { /* duel_finish */
        if (g_duel_bonuses_translated) {
            psp_ret(1u);
            return 1;
        }

        const uint32_t saved_a0 = psp_cpu.r[PSP_REG_A0];
        const uint32_t gs2 = resolve_gs2();
        const int use_scratch = gs2 && g_tf3_scratch_mapped;
        if (use_scratch) {
            for (uint32_t off = 0; off < 0x3Cu; off += 4u)
                psp_write32(TF3_SCRATCH_RESULTS_ADDR + off, psp_read32(gs2 + off));
            for (uint32_t off = 0x3Cu; off < TF3_RESULTS_SIZE; off += 4u)
                psp_write32(TF3_SCRATCH_RESULTS_ADDR + off, 0u);
        }

        /* TF1 has no argument. TF3 uses nonzero a0 for a summary-only finish. */
        psp_cpu.r[PSP_REG_A0] = use_scratch ? 0u : 1u;
        g_duel_finish_in_progress = use_scratch;
        const int called = call_tf3_export_nid(nid);
        g_duel_finish_in_progress = 0;
        if (called && use_scratch && g_tf3_post_deck_trace)
            fprintf(stderr, "[DUEL_FINISH_NATIVE] status=%u turn=%u duelist=%u\n",
                    psp_cpu.r[PSP_REG_V0], psp_read32(TF3_SCRATCH_RESULTS_ADDR + 0x114u),
                    psp_read32(TF3_SCRATCH_RESULTS_ADDR + 0x128u));
        if (called && use_scratch &&
            tf3_bridge_apply_victory_bonus(gs2, TF3_SCRATCH_RESULTS_ADDR)) {
            psp_ret(1u);
        }
        psp_cpu.r[PSP_REG_A0] = saved_a0;
        return 1;
    }

    case 0xFCC05B20u: /* duel_zone_faceup(player, zone, index) */
        call_tf3_export_nid(0x8963D369u);
        return 1;

    case 0xB4829A57u: { /* duel_zone_count(player, zone) */
        const uint32_t z = psp_arg(1);
        switch (z) {
        case 0xBu: call_tf3_export_nid(0x989FFE12u); return 1; /* duel_n_hand */
        case 0xCu: call_tf3_export_nid(0x739BD340u); return 1; /* duel_n_18 */
        case 0xDu: call_tf3_export_nid(0xD8A1FE8Bu); return 1; /* duel_n_d */
        case 0xEu: call_tf3_export_nid(0x5150628Du); return 1; /* duel_n_grave */
        case 0xFu: call_tf3_export_nid(0xC16BC388u); return 1; /* duel_n_f */
        default:
            if (z < 11u && call_tf3_export_nid(0x8D8BC9DEu)) { /* duel_slot_id */
                psp_ret((psp_cpu.r[PSP_REG_V0] & 0x3FFFu) ? 1u : 0u);
            } else {
                psp_ret(0u);
            }
            return 1;
        }
    }

    case 0x1EE11568u:
        psp_cpu.r[PSP_REG_A2] = 0u;
        call_tf3_export_nid(0xB64C1C59u);
        return 1;

    case 0xC8124197u:
        psp_cpu.r[PSP_REG_A2] = 1u;
        call_tf3_export_nid(0xB64C1C59u);
        return 1;

    case 0xE54F14CEu: /* Counter card 5395 */
        psp_cpu.r[PSP_REG_A2] = 5395u;
        psp_cpu.r[PSP_REG_A3] = 0u;
        psp_cpu.r[PSP_REG_T0] = 0xFFFFFFFFu;
        call_tf3_export_nid(0x0524C173u);
        return 1;

    case 0xB401022Du: /* Counter card 6800 */
        psp_cpu.r[PSP_REG_A2] = 6800u;
        psp_cpu.r[PSP_REG_A3] = 0u;
        psp_cpu.r[PSP_REG_T0] = 0xFFFFFFFFu;
        call_tf3_export_nid(0x0524C173u);
        return 1;

    case 0x52550FBDu: /* Counter card 5177 */
        psp_cpu.r[PSP_REG_A2] = 5177u;
        psp_cpu.r[PSP_REG_A3] = 0u;
        psp_cpu.r[PSP_REG_T0] = 0xFFFFFFFFu;
        call_tf3_export_nid(0x0524C173u);
        return 1;

    case 0xF14C9EFDu: { /* dmgr_ev_attack / duel_088FE87C */
        if (psp_cpu.r[PSP_REG_RA] != 0x02203350u)
            return 0;
        const uint32_t out = psp_cpu.r[PSP_REG_T0];
        const uint32_t player = psp_arg(0);
        const uint32_t shadow = TF3_SCRATCH_BASE + 0x3000u;
        if (!out || !psp_mem_unwatched_ptr(out, 0x20u)) {
            psp_ret(0u);
            return 1;
        }
        uint32_t damage = 0;
        if (g_tf3_scratch_mapped && player < 2u && psp_arg(1) < 5u && psp_arg(2) <= 5u) {
            psp_cpu.r[PSP_REG_A3] = shadow;
            if (call_tf3_export_nid(0x25035E00u)) {
                /* TF3 LP comparison at 0x03785DCC uses these two words per seat. */
                const uint32_t target = shadow + (1u - player) * 0x18u;
                damage = psp_read32(target + 0x20u) + psp_read32(target + 0x24u);
            }
        }
        /* This caller reads only st[7] (opponent LP damage). */
        psp_write32(out + 0x1Cu, damage);
        if (g_tf3_post_deck_trace)
            fprintf(stderr, "[ABI_ATTACK_GUESS] vb=%llu player=%u damage=%u out=0x%X\n",
                    (unsigned long long)psp_display_vblanks(), player, damage, out);
        psp_ret(0u);
        return 1;
    }

    default:
        return 0;
    }
}

/* Full call trace for the TF1 draw/mgr -> TF3 boundary. Deck, zone and result
 * reads from the TF1 side cross here. */
int duel_tf3_handle_export(uint32_t nid) {
    static int on = -1;
    if (on < 0) on = getenv("SETO_ABI_TRACE_CALLS") != NULL;
    if (!on) return duel_tf3_handle_export_body(nid);
    const uint32_t a0 = psp_arg(0), a1 = psp_arg(1);
    const uint32_t a2 = psp_arg(2), a3 = psp_arg(3);
    const uint32_t ra = psp_cpu.r[PSP_REG_RA];
    const int handled = duel_tf3_handle_export_body(nid);
    /* Per-NID budget: one noisy helper must not starve the rest of the duel. */
    static uint32_t seen_nid[256];
    static uint16_t seen_cnt[256];
    static unsigned nseen;
    if (nid != 0x04F712B8u) { /* event pop is already traced */
        unsigned i = nseen;
        for (unsigned k = 0; k < nseen; k++) {
            if (seen_nid[k] == nid) { i = k; break; }
        }
        if (i == nseen && nseen < 256u) {
            seen_nid[nseen] = nid;
            seen_cnt[nseen] = 0;
            nseen++;
        }
        if (i < nseen && seen_cnt[i]++ < 80u) {
            fprintf(stderr, "[TF1_TO_TF3] vb=%llu nid=0x%08X ra=0x%X args=[0x%X,0x%X,0x%X,0x%X] handled=%d -> 0x%X\n",
                    (unsigned long long)psp_display_vblanks(), nid, ra, a0, a1, a2, a3,
                    handled, psp_cpu.r[PSP_REG_V0]);
        }
    }

    return handled;
}

static void tf3eng_import_forward_body(uint32_t nid) {
    uint16_t kid = (uint16_t)(psp_arg(0) & 0xFFFFu);
    if (g_tf3_post_deck_trace) {
        extern uint64_t psp_display_vblanks(void);
        static uint32_t seen[128];
        static unsigned nseen;
        static uint32_t vb_nid[128];
        static unsigned vb_cnt[128];
        static unsigned vb_n;
        static uint64_t vb_thresh = (uint64_t)-1;
        if (vb_thresh == (uint64_t)-1) {
            const char *e = getenv("SETO_TRACE_DUEL_VB");
            vb_thresh = e ? (uint64_t)strtoull(e, NULL, 10) : (uint64_t)-2;
        }
        if (vb_thresh != (uint64_t)-2 && psp_display_vblanks() >= vb_thresh &&
            nid != 0x6595A5B3u && nid != 0x885E0882u && nid != 0x88B6C7F0u && nid != 0xDFCA450Bu) {
            unsigned idx = vb_n;
            for (unsigned i = 0; i < vb_n; i++) {
                if (vb_nid[i] == nid) { idx = i; break; }
            }
            if (idx == vb_n && vb_n < 128u) { vb_nid[vb_n] = nid; vb_cnt[vb_n] = 0; vb_n++; }
            if (idx < vb_n && vb_cnt[idx] < 5u) {
                vb_cnt[idx]++;
                fprintf(stderr, "[DUEL_VB %llu] tf3_imp nid=0x%08X a0=0x%08X a1=0x%08X\n",
                        (unsigned long long)psp_display_vblanks(), nid, psp_arg(0), psp_arg(1));
                fflush(stderr);
            }
        }
        int found = 0;
        for (unsigned i = 0; i < nseen; i++) {
            if (seen[i] == nid) { found = 1; break; }
        }
        if (!found && nseen < 128u) {
            seen[nseen++] = nid;
            fprintf(stderr, "[POST_DECK] tf3_imp #%u nid=0x%08X a0=0x%08X a1=0x%08X\n",
                    nseen, nid, psp_arg(0), psp_arg(1));
            fflush(stderr);
        }
    }

    /* 0. card_same_name(a, b): two TF3 card ids -> convert both to TF1 before calling EBOOT. */
    if (nid == 0x6F2C8118u) {
        const uint32_t sa = psp_arg(0), sb = psp_arg(1);
        if (sa && sa == sb) {
            psp_ret(1u);
            return;
        }
        const uint16_t tf1_a = gx_tf3_to_tf1((uint16_t)(sa & 0xFFFFu));
        const uint16_t tf1_b = gx_tf3_to_tf1((uint16_t)(sb & 0xFFFFu));
        uint32_t gp2 = 0;
        const uint32_t addr2 = duel_eboot_export_addr(nid, &gp2);
        if (!tf1_a || !tf1_b || !addr2) {
            psp_ret(0u);
            return;
        }
        psp_cpu.r[PSP_REG_A0] = tf1_a;
        psp_cpu.r[PSP_REG_A1] = tf1_b;
        call_guest_addr(addr2, gp2);
        psp_cpu.r[PSP_REG_A0] = sa;
        psp_cpu.r[PSP_REG_A1] = sb;
        if (g_tf3_post_deck_trace) {
            static unsigned same_log;
            if (same_log++ < 300u)
                fprintf(stderr, "[SAME] a=0x%X(%u) b=0x%X(%u) -> %u\n",
                        sa, tf1_a, sb, tf1_b, psp_cpu.r[PSP_REG_V0]);
        }
        return;
    }

    /* The TF3 engine uses this BOOT export to set the card's turn limit. */
    if (nid == 0x32CB2B73u) {
        /* TF3 BOOT returns 2 for Future Fusion. The TF1 value of 1
         * stops the Standby scan before the delayed summon is ready. */
        if (psp_arg(0) == 0x1A7Eu) {
            psp_ret(2u);
            return;
        }
        const uint32_t raw = psp_arg(0);
        const uint16_t tf1_kid = gx_tf3_to_tf1((uint16_t)(raw & 0xFFFFu));
        uint32_t gp3 = 0;
        const uint32_t addr3 = duel_eboot_export_addr(nid, &gp3);
        if (!tf1_kid || !addr3) {
            psp_ret(0u);
            return;
        }
        psp_cpu.r[PSP_REG_A0] = tf1_kid;
        call_guest_addr(addr3, gp3);
        psp_cpu.r[PSP_REG_A0] = raw;
        return;
    }

    /* 0c. 0xD83FB4E8 is a dispatcher, not a predicate: it indexes the table at
     * 0x0009A2D8 with a1 and calls the selected group filter. */
    if (nid == 0xD83FB4E8u) {
        static const uint32_t group_nid[12] = {
            0x84F43A88u, 0x58223517u, 0xAB8CF7CCu, 0xB86A0894u,
            0xE65115B6u, 0x4DB996F6u, 0x6EAF1121u, 0x46F3C6A2u,
            0x93517477u, 0x914562B0u, 0xE618B6BAu, 0x83458494u,
        };
        const uint32_t card = psp_arg(0);
        const uint32_t group = psp_arg(1);
        if (group < 12u) {
            uint32_t val = 0u;
            if (tf3_arch_eval(group_nid[group], (uint16_t)(card & 0xFFFFu), &val)) {
                psp_ret(val);
                return;
            }
            uint32_t gp5 = 0;
            const uint32_t fn = duel_eboot_export_addr(group_nid[group], &gp5);
            if (fn) {
                const uint16_t tf1_card = gx_tf3_to_tf1((uint16_t)(card & 0xFFFFu));
                if (!tf1_card) {
                    psp_ret(0u);
                    return;
                }
                psp_cpu.r[PSP_REG_A0] = tf1_card;
                call_guest_addr(fn, gp5);
                psp_cpu.r[PSP_REG_A0] = card;
                if (g_tf3_post_deck_trace) {
                    static unsigned f2;
                    if (f2++ < 80u)
                        fprintf(stderr, "[GROUP] gi=%u card=0x%X(%u) -> %u\n",
                                group, card, tf1_card, psp_cpu.r[PSP_REG_V0]);
                }
                return;
            }
            /* Groups 10 and 11 have no TF1 export. Both are fixed id lists read
             * from the TF3 functions at 0x0005CD70 and 0x0005CE6C. */
            if (group == 10u || group == 11u) {
                static const uint16_t grp10[] = {
                    7443, 7442, 7275, 7177, 7175, 6990,
                    6989, 6988, 6767, 6766, 6765, 6653,
                };
                static const uint16_t grp11_extra[] = {
                    7806, 7805, 7553, 7447, 7209, 7195, 7163, 6993, 6775,
                };
                const uint16_t c = (uint16_t)psp_arg(0);
                for (unsigned i = 0; i < sizeof grp10 / sizeof grp10[0]; i++) {
                    if (grp10[i] == c) { psp_ret(1u); return; }
                }
                if (group == 11u) {
                    for (unsigned i = 0; i < sizeof grp11_extra / sizeof grp11_extra[0]; i++) {
                        if (grp11_extra[i] == c) { psp_ret(1u); return; }
                    }
                }
                psp_ret(0u);
                return;
            }
        }
        psp_ret(0u);
        return;
    }

    /* 0d. 0xFCB00A02 is card_get_attribute, but the consumer at 0x0360D910
     * compares the result with 13/14/15, which is card_get_type. Fix the call
     * site only: the global attribute getter must stay untouched. */
    if (nid == 0xFCB00A02u && psp_cpu.r[PSP_REG_RA] == 0x0360D910u) {
        const uint64_t tp = tf3_card_prop(kid);
        const uint32_t tv = (uint32_t)((tp >> 49) & 0x1Fu);
        if (g_tf3_post_deck_trace) {
            static unsigned tlog;
            if (tlog++ < 40u)
                fprintf(stderr, "[TYPE] caller-aware kid=%u -> %u\n", kid, tv);
        }
        psp_ret(tv);
        return;
    }

    /* 1. All 102 evaluated card/archetype predicates from tf3_BOOT.BIN */
    {
        uint32_t val = 0;
        if (tf3_arch_eval(nid, kid, &val)) {
            if (tf3_trace_abi_calls()) {
                static unsigned arch_log;
                if (arch_log++ < 3000u)
                    fprintf(stderr, "[ARCH] vb=%llu nid=0x%08X kid=%u idx=%u -> %u\n",
                            (unsigned long long)psp_display_vblanks(), nid, kid,
                            tf3_card_index((uint16_t)kid), val);
            }
            psp_ret(val);
            return;
        }
    }

    /* 2. Native TF3 CARD_Prop / CARD_IntID getters */
    switch (nid) {
    case 0xAD0E1E03u: /* GetCardNamePtr: all 37 tf3eng callers pass v0 to string formatters (0x03603938 / 0x0360383C) */
        psp_ret(tf3_card_name_ptr(kid));
        return;

    case 0x98E07D26u: { /* CARD_Prop byte 6 bits 1..5 -> [49..53] */
        uint64_t p = tf3_card_prop(kid);
        psp_ret((uint32_t)((p >> 49) & 0x1Fu));
        return;
    }
    case 0xE62CCE79u: { /* CARD_Prop [38..41] */
        uint64_t p = tf3_card_prop(kid);
        psp_ret((uint32_t)((p >> 38) & 0x0Fu));
        return;
    }
    case 0xCF199BAEu: { /* CARD_Prop byte 5 bits 2..5 -> [42..45] (Level) */
        uint64_t p = tf3_card_prop(kid);
        psp_ret((uint32_t)((p >> 42) & 0x0Fu));
        return;
    }
    case 0xA72D71BFu: { /* CARD_Prop [46..48] */
        uint64_t p = tf3_card_prop(kid);
        psp_ret((uint32_t)((p >> 46) & 0x07u));
        return;
    }
    case 0xFCB00A02u: { /* card_get_attribute: (byte4 >> 1) & 0x1F, Gods -> 11 */
        if (kid >= 4998u && kid <= 5000u) {
            psp_ret(11u);
            return;
        }
        uint64_t p = tf3_card_prop(kid);
        psp_ret((uint32_t)((p >> 33) & 0x1Fu));
        return;
    }
    case 0x747753B8u: { /* ATK (0 for ?) */
        uint32_t v = (uint32_t)((tf3_card_prop(kid) >> 14) & 0x1FFu);
        psp_ret((v == 511u) ? 0u : (v * 10u));
        return;
    }
    case 0xF203CF01u: { /* DEF (0 for ?) */
        uint32_t v = (uint32_t)((tf3_card_prop(kid) >> 23) & 0x1FFu);
        psp_ret((v == 511u) ? 0u : (v * 10u));
        return;
    }
    case 0x760919BFu: { /* ATK (-1 for ?) */
        uint32_t v = (uint32_t)((tf3_card_prop(kid) >> 14) & 0x1FFu);
        psp_ret((v == 511u) ? 0xFFFFFFFFu : (v * 10u));
        return;
    }
    case 0x249F5AD0u: { /* DEF (-1 for ?) */
        uint32_t v = (uint32_t)((tf3_card_prop(kid) >> 23) & 0x1FFu);
        psp_ret((v == 511u) ? 0xFFFFFFFFu : (v * 10u));
        return;
    }
    case 0xFE91A2ECu: { /* GetCardLimit(kid) -> 0..3 (NOT CardNamePtr, which is 0xAD0E1E03) */
        const uint16_t tf1_kid = gx_tf3_to_tf1(kid);
        uint32_t gp = 0;
        const uint32_t addr = duel_eboot_export_addr(0xFE91A2ECu, &gp);
        uint32_t lim = 3u;
        if (tf1_kid && addr) {
            psp_cpu.r[PSP_REG_A0] = tf1_kid;
            call_guest_addr(addr, gp);
            lim = psp_cpu.r[PSP_REG_V0];
        }
        psp_ret(lim);
        if (tf3_trace_bridge()) {
            static unsigned lim_log;
            if (lim_log++ < 20u) {
                fprintf(stderr, "tf3_bridge: 0xFE91A2EC(GetCardLimit) kid=%u -> tf1=%u -> %u\n",
                        kid, tf1_kid, lim);
                fflush(stderr);
            }
        }
        return;
    }

    case 0x21F9E97Eu: { /* TF3 BOOT 0x0002f638 -> 0x0002f004: CARD_Genre bit check */
        uint32_t tbl_idx = psp_arg(1);
        uint16_t card_idx = tf3_card_index(kid);
        uint32_t res = 0u;
        if (tbl_idx < TF3_GENRE_TBL_LEN && card_idx < TF3_GENRE_CARDS) {
            uint8_t bit = g_tf3_genre_bit_tbl[tbl_idx];
            const uint8_t *entry = &g_tf3_card_genre[(uint32_t)card_idx * 6u];
            uint32_t word_off = (uint32_t)(bit >> 4) * 2u;
            uint16_t w = (uint16_t)entry[word_off] | ((uint16_t)entry[word_off + 1u] << 8);
            res = (w & (1u << (bit & 15u))) ? 1u : 0u;
        }
        if (tf3_trace_bridge()) {
            static unsigned genre_log;
            if (genre_log++ < 25u) {
                fprintf(stderr, "tf3_bridge: 0x21F9E97E kid=%u idx=%u tbl=%u -> %u\n",
                        kid, card_idx, tbl_idx, res);
                fflush(stderr);
            }
        }
        psp_ret(res);
        return;
    }

    case 0xCB6030CFu: { /* TF3 BOOT 0x0002ec94: DLG_Text_E[DLG_Indx_E[id]] */
        uint32_t id = psp_arg(0);
        uint32_t ptr = tf3_dlg_str_ptr(id);
        if (tf3_trace_bridge()) {
            static unsigned dlg_log;
            if (dlg_log++ < 20u) {
                fprintf(stderr, "tf3_bridge: 0xCB6030CF(%u) -> 0x%08X\n", id, ptr);
                fflush(stderr);
            }
        }
        psp_ret(ptr);
        return;
    }

    case 0xE3C8F657u: { /* TF3 BOOT 0x0002ecb8: id ? DLG[id + 500] : "" */
        uint32_t id = psp_arg(0);
        uint32_t ptr = id ? tf3_dlg_str_ptr(id + 500u) : GX_EMPTY_WSTR_ADDR;
        if (tf3_trace_bridge()) {
            static unsigned dlg2_log;
            if (dlg2_log++ < 20u) {
                fprintf(stderr, "tf3_bridge: 0xE3C8F657(%u) -> 0x%08X\n", id, ptr);
                fflush(stderr);
            }
        }
        psp_ret(ptr);
        return;
    }

    case 0x375A7F03u: { /* TF3 BOOT 0x0002ed58: id ? DLG[id + 530] : "" */
        uint32_t id = psp_arg(0);
        uint32_t ptr = id ? tf3_dlg_str_ptr(id + 530u) : GX_EMPTY_WSTR_ADDR;
        if (tf3_trace_bridge()) {
            static unsigned dlg3_log;
            if (dlg3_log++ < 20u) {
                fprintf(stderr, "tf3_bridge: 0x375A7F03(%u) -> 0x%08X\n", id, ptr);
                fflush(stderr);
            }
        }
        psp_ret(ptr);
        return;
    }

    case 0xF2B4771Cu: { /* TF3 BOOT 0x0002ece8: attribute string -> DLG[mapped + 550] */
        uint32_t attr = psp_arg(0);
        if (!attr) {
            psp_ret(GX_EMPTY_WSTR_ADDR);
            return;
        }
        uint32_t m = 1u;
        if (attr == 18u) m = 13u;
        else if (attr == 17u) m = 12u;
        else if (attr == 16u || attr == 15u) m = 11u;
        else if (attr < 11u) m = attr;
        psp_ret(tf3_dlg_str_ptr(m + 550u));
        return;
    }

    /* 3. Non-archetype missing libehsys_rel helpers (18 NIDs, va < 0x05BF00) */
    case 0xAB18E04Au: /* stripped printf */
    case 0xBCCFD219u: /* save card unlock flag write */
    case 0xCC32BE69u: /* save EXP add */
    case 0xD98EA579u: /* save DP/win/loss record update */
    case 0xC3542E68u: /* save 6-bit option flags */
    case 0x26BDF70Fu: /* duel option flag bit 1 */
    case 0x46EACB7Cu: /* duel option flag bit 2 */
    case 0x7002278Eu: /* duel option flag bit 4 */
    case 0x34A7E8C2u: /* banlist match count */
    case 0x639AE71Fu: /* banlist restricted bool */
        psp_ret(0u);
        return;

    case 0x90281239u: /* duelist level */
    case 0x8477BA04u: /* card seen/unlocked bool */
        psp_ret(1u);
        return;

    case 0xA263D2AFu: /* banlist limit status (5 = no active restriction) */
        psp_ret(5u);
        return;

    case 0x8CE72107u: { /* read DP/record triplet -> zero out 3 uint32s */
        uint32_t out_ptr = psp_arg(1);
        if (out_ptr) {
            psp_write32(out_ptr + 0u, 0u);
            psp_write32(out_ptr + 4u, 0u);
            psp_write32(out_ptr + 8u, 0u);
        }
        psp_ret(0u);
        return;
    }

    case 0xD46615D9u: /* scratch PSP stats buffer (+248..) */
        psp_ret(TF3_SCRATCH_STATS_ADDR);
        return;

    case 0x9A381A51u: { /* scratch PSP partner/rental slot ptr */
        uint32_t idx = psp_arg(0);
        if (idx >= 128u) idx = 0u;
        psp_ret(TF3_SCRATCH_PARTNER_ADDR + idx * 24u);
        return;
    }

    case 0x5D9E6261u: { /* 462-entry table from TF3 BOOT 0x001103F0 */
        int32_t idx = (int32_t)psp_arg(0);
        if (g_duel_finish_in_progress && idx >= 0) {
            uint32_t gp = 0;
            const uint32_t addr = duel_eboot_export_addr(0x3CD5EF4Cu, &gp);
            if (addr) {
                call_guest_addr(addr, gp); /* rank for the TF1 roster index */
                return;
            }
        }
        if (idx >= 0 && idx < 462) {
            psp_ret((uint32_t)g_tf3_tbl_5d9e6261[idx]);
        } else {
            psp_ret(1u);
        }
        return;
    }

    case 0x88B6C7F0u: {
        /* Translate TF1 EBOOT duel info struct (modes at +280..+282, 4x392 slots at +288)
         * into TF3 layout (modes at +400..+402, 4x428 slots at +408, +128->+152). */
        uint32_t gp = 0;
        uint32_t addr = duel_eboot_export_addr(nid, &gp);
        if (!addr) {
            psp_ret(0u);
            return;
        }
        call_guest_addr(addr, gp);
        uint32_t src = psp_cpu.r[PSP_REG_V0];
        if (!src) return;
        if (!g_gx_extra_mapped) gx_runtime_init();
        uint32_t dst = GX_TF3_DUEL_INFO_ADDR;
        const uint8_t *sp = (const uint8_t *)psp_mem_ptr(src, 288u + 4u * 392u);
        uint8_t *dp = (uint8_t *)psp_mem_ptr(dst, 2120u);
        if (sp && dp) {
            memcpy(dp, sp, 280u);
            memset(dp + 280u, 0, 2120u - 280u);
            memcpy(dp + 400u, sp + 280u, 8u);
            for (uint32_t i = 0; i < 4u; i++) {
                const uint8_t *s_slot = sp + 288u + i * 392u;
                uint8_t *d_slot = dp + 408u + i * 428u;
                memcpy(d_slot, s_slot, 128u);
                memcpy(d_slot + 152u, s_slot + 128u, 264u);
                /* Online comm-ID / peer slot index: TF1 s_slot+8 (+296) -> TF3 d_slot+24 (+432). */
                memcpy(d_slot + 24u, s_slot + 8u, 2u);
                if (g_duel_finish_in_progress) {
                    memcpy(d_slot + 12u, s_slot, 2u);
                    memcpy(d_slot + 20u, s_slot + 4u, 2u);
                }
            }
        } else {
            for (uint32_t off = 0; off < 280u; off += 4u) {
                psp_write32(dst + off, psp_read32(src + off));
            }
            for (uint32_t off = 280u; off < 2120u; off += 4u) {
                psp_write32(dst + off, 0u);
            }
            for (uint32_t off = 0; off < 8u; off++) {
                psp_write8(dst + 400u + off, psp_read8(src + 280u + off));
            }
            for (uint32_t i = 0; i < 4u; i++) {
                uint32_t s_slot = src + 288u + i * 392u;
                uint32_t d_slot = dst + 408u + i * 428u;
                for (uint32_t off = 0; off < 128u; off += 4u) {
                    psp_write32(d_slot + off, psp_read32(s_slot + off));
                }
                for (uint32_t off = 0; off < 264u; off += 4u) {
                    psp_write32(d_slot + 152u + off, psp_read32(s_slot + 128u + off));
                }
                /* Online comm-ID / peer slot index: TF1 s_slot+8 (+296) -> TF3 d_slot+24 (+432). */
                psp_write16(d_slot + 24u, psp_read16(s_slot + 8u));
                if (g_duel_finish_in_progress) {
                    /* The finish path reads the participant kind at +0x1A4
                     * and the roster index eight bytes later. */
                    psp_write16(d_slot + 12u, psp_read16(s_slot));
                    psp_write16(d_slot + 20u, psp_read16(s_slot + 4u));
                }
            }
        }
        psp_ret(dst);
        return;
    }

    case 0x82249188u: {
        const uint32_t slot = psp_arg(0);
        uint32_t gp = 0;
        const uint32_t addr = duel_eboot_export_addr(nid, &gp);
        if (!addr) {
            psp_ret(0u);
            return;
        }
        call_guest_addr(addr, gp);
        const uint32_t src = psp_cpu.r[PSP_REG_V0];
        if (!src) return;
        if (!g_tf3_scratch_mapped) duel_tf3_on_duel_load();
        const uint32_t dst = TF3_SCRATCH_NET_RECV_ADDR + (slot & 3u) * TF3_NET_RECV_SLOT_SIZE;
        const void *sp = psp_mem_ptr(src + 4u, 252u);
        uint8_t *dp = (uint8_t *)psp_mem_ptr(dst, TF3_NET_RECV_SLOT_SIZE);
        if (sp && dp) {
            memcpy(dp, sp, 252u);
            memset(dp + 252u, 0, TF3_NET_RECV_SLOT_SIZE - 252u);
        } else {
            for (uint32_t off = 0; off < 252u; off += 4u) {
                psp_write32(dst + off, psp_read32(src + 4u + off));
            }
            for (uint32_t off = 252u; off < TF3_NET_RECV_SLOT_SIZE; off += 4u) {
                psp_write32(dst + off, 0u);
            }
        }
        psp_ret(dst);
        return;
    }

    case 0x0CEF10F0u: {
        uint32_t gp = 0;
        const uint32_t addr = duel_eboot_export_addr(nid, &gp);
        if (!addr) {
            psp_ret(0u);
            return;
        }
        if (psp_arg(1) > 252u) {
            static unsigned clamped_log;
            if (clamped_log++ < 10u) {
                fprintf(stderr, "tf3_bridge: 0x0CEF10F0 clamp len %u -> 252\n", psp_arg(1));
            }
            psp_cpu.r[PSP_REG_A1] = 252u;
        }
        call_guest_addr(addr, gp);
        return;
    }

    default:
        break;
    }

    /* 4. Remaining shared libehsys NIDs (libc, sys, audio, strings, chara, 0x5777192d) */
    uint32_t gp = 0;
    uint32_t addr = duel_eboot_export_addr(nid, &gp);
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20) {
            fprintf(stderr, "tf3eng PRX: import NID 0x%08X has no EBOOT export\n", nid);
        }
        psp_ret(0u);
        return;
    }
    if (nid == 0x6595A5B3u) { /* req_flag_get */
        const uint32_t flag = psp_arg(0);
        const uint32_t mapped = tf3_req_flag_to_tf1(flag);
        psp_cpu.r[PSP_REG_A0] = mapped;
        call_guest_addr(addr, gp);
        psp_cpu.r[PSP_REG_A0] = flag;
        if (flag != mapped && psp_cpu.r[PSP_REG_V0] &&
            (g_tf3_post_deck_trace || tf3_trace_abi_ev())) {
            static unsigned busy_logs;
            static unsigned popup_busy_logs;
            unsigned *budget = flag == 34u || flag == 52u ? &popup_busy_logs : &busy_logs;
            if ((*budget)++ < 40u)
                fprintf(stderr, "[ABI_WAIT] vb=%llu flag=%u->%u busy=%u\n",
                        (unsigned long long)psp_display_vblanks(), flag, mapped, psp_cpu.r[PSP_REG_V0]);
        }
        return;
    }
    call_guest_addr(addr, gp);
    if (nid == 0x885E0882u) { /* game_state2_get */
        const uint32_t gs2 = psp_cpu.r[PSP_REG_V0];
        if (gs2 >= 0x08000000u && gs2 < 0x0C000000u) g_gs2_base = gs2;
        if (g_duel_finish_in_progress)
            psp_ret(TF3_SCRATCH_RESULTS_ADDR);
    }
}

/* Full call trace for the TF3 -> BOOT/ehsys boundary. This is where a card
 * predicate that cannot see the deck answers 0 and blocks activation. */
void tf3eng_import_forward(uint32_t nid) {
    static int on = -1;
    if (on < 0) on = getenv("SETO_ABI_TRACE_CALLS") != NULL;
    if (!on) {
        tf3eng_import_forward_body(nid);
        return;
    }
    const uint32_t a0 = psp_arg(0), a1 = psp_arg(1);
    const uint32_t a2 = psp_arg(2), a3 = psp_arg(3);
    const uint32_t ra = psp_cpu.r[PSP_REG_RA]; /* caller: identifies the effect */
    tf3eng_import_forward_body(nid);
    /* Per-NID budget: one noisy helper must not starve the rest of the duel. */
    static uint32_t seen_nid[1024];
    static uint16_t seen_cnt[1024];
    static unsigned nseen;
    unsigned i = nseen;
    for (unsigned k = 0; k < nseen; k++) {
        if (seen_nid[k] == nid) { i = k; break; }
    }
    if (i == nseen && nseen < 1024u) {
        seen_nid[nseen] = nid;
        seen_cnt[nseen] = 0;
        nseen++;
    } else if (i == nseen) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[TF3_CALL] tabela de NIDs cheia\n"); }
    }
    if (i < nseen && seen_cnt[i]++ < 80u) {
        fprintf(stderr, "[TF3_CALL] vb=%llu nid=0x%08X ra=0x%X args=[0x%X,0x%X,0x%X,0x%X] -> 0x%X\n",
                (unsigned long long)psp_display_vblanks(), nid, ra, a0, a1, a2, a3,
                psp_cpu.r[PSP_REG_V0]);
    }
}
