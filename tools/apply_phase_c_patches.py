#!/usr/bin/env python3
"""Apply Phase C VFS & 2480->3480 / 6958->7958 patches idempotently."""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def patch_exact(rel_path, old, new, label):
    p = os.path.join(ROOT, rel_path)
    raw = open(p, "rb").read()
    crlf = b"\r\n" in raw
    txt = raw.decode("utf-8").replace("\r\n", "\n")
    old_n = old.replace("\r\n", "\n")
    new_n = new.replace("\r\n", "\n")
    if new_n in txt and old_n not in txt:
        print(f"  [already] {label} ({rel_path})")
        return
    cnt = txt.count(old_n)
    if cnt != 1:
        raise RuntimeError(f"Expected 1 match for {label} in {rel_path}, got {cnt}")
    out = txt.replace(old_n, new_n, 1)
    if crlf:
        out = out.replace("\n", "\r\n")
    with open(p, "wb") as f:
        f.write(out.encode("utf-8"))
    print(f"  [patched] {label} ({rel_path})")

print("=== 1. iofilemgr.c VFS overrides ===")
iofile_path = os.path.join(ROOT, "build/deps/psprecomp/src/hle/iofilemgr.c")
if "gx_resolve_override" in open(iofile_path, "r", encoding="utf-8").read():
    print("  [already] iofilemgr VFS overrides (included in psprecomp patch)")
else:
    patch_exact(
        "build/deps/psprecomp/src/hle/iofilemgr.c",
        """static FILE *open_lbn(const char *guest, uint64_t *start, uint64_t *end) {
    unsigned long lbn, size;
    if (sscanf(guest, "disc0:/sce_lbn0x%lx_size0x%lx", &lbn, &size) != 2)
        return NULL;
    if (!g_umd[0]) return NULL;
    FILE *f = fopen(g_umd, "rb");
    if (!f) return NULL;
    uint64_t off = (uint64_t)lbn * 2048u;
    if (off > 0x7FFFFFFFull || fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    *start = off;
    *end = off + (uint64_t)size;
    return f;
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    const int raw = !strcmp(guest, "umd0:") || !strcmp(guest, "umd1:");
    FILE *f = NULL;
    uint64_t lbn_start = 0, lbn_end = 0;
    if (!strncmp(guest, "disc0:/sce_lbn0x", 16)) {
        f = open_lbn(guest, &lbn_start, &lbn_end);
        snprintf(host, sizeof host, "%s @ %llu", g_umd[0] ? g_umd : "(no disc image set)",
                 (unsigned long long)lbn_start);
    } else if (raw) {
        if (g_umd[0]) f = fopen(g_umd, "rb");
        snprintf(host, sizeof host, "%s", g_umd[0] ? g_umd : "(no disc image set)");
    } else {
        map_path(guest, host, sizeof host);
        /* The system font library opens its PGF files from flash0. A host
         * directory (a PSP flash0 dump or an emulator asset tree) stands in
         * for it when PSPRECOMP_FLASH0 names one. */
        if (!strncmp(guest, "flash0:/", 8)) {
            const char *root = getenv("PSPRECOMP_FLASH0");
            if (root)
                snprintf(host, sizeof host, "%s/%s", root, guest + 8);
        }""",
        """static int gx_ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static const char *gx_lbn_filename(unsigned long lbn) {
    switch (lbn) {
    case 0x4b1e0ul: return "cardinfo_eng.ehp";
    case 0x37d90ul: return "cardh_e.cip";
    case 0x39160ul: return "cardm_e.cip";
    case 0x39f30ul: return "deck_all_e.ehp";
    default:        return NULL;
    }
}

static const char *gx_guest_filename(const char *guest) {
    const char *p = guest;
    if (!strncmp(p, "disc0:", 6)) p += 6;
    else if (!strncmp(p, "umd0:", 5)) p += 5;
    while (*p == '/' || *p == '\\\\') p++;
    if (gx_ascii_ieq(p, "PSP_GAME/USRDIR/duelsys/cardinfo_eng.ehp")) return "cardinfo_eng.ehp";
    if (gx_ascii_ieq(p, "PSP_GAME/USRDIR/card/cardh_e.cip"))         return "cardh_e.cip";
    if (gx_ascii_ieq(p, "PSP_GAME/USRDIR/card/cardm_e.cip"))         return "cardm_e.cip";
    if (gx_ascii_ieq(p, "PSP_GAME/USRDIR/deck/all_e.ehp"))           return "deck_all_e.ehp";
    return NULL;
}

static int gx_resolve_override(const char *fname, char *out, size_t cap, uint64_t *out_sz) {
    if (!fname) return 0;
    const char *env = getenv("SETO_GX_DATA_DIR");
    const char *candidates[4];
    char buf[4][1024];
    int n = 0;
    if (env && env[0]) {
        snprintf(buf[n], sizeof buf[n], "%s/%s", env, fname);
        candidates[n] = buf[n]; n++;
    }
    snprintf(buf[n], sizeof buf[n], "build/gx_data/%s", fname);
    candidates[n] = buf[n]; n++;
    snprintf(buf[n], sizeof buf[n], "gx_data/%s", fname);
    candidates[n] = buf[n]; n++;
    snprintf(buf[n], sizeof buf[n], "%s/gx_data/%s", g_root, fname);
    candidates[n] = buf[n]; n++;

    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        struct _stat64 st;
        if (_stat64(candidates[i], &st) == 0 && !(st.st_mode & _S_IFDIR)) {
            if (out) snprintf(out, cap, "%s", candidates[i]);
            if (out_sz) *out_sz = (uint64_t)st.st_size;
            return 1;
        }
#else
        struct stat st;
        if (stat(candidates[i], &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (out) snprintf(out, cap, "%s", candidates[i]);
            if (out_sz) *out_sz = (uint64_t)st.st_size;
            return 1;
        }
#endif
    }
    return 0;
}

static FILE *open_lbn(const char *guest, uint64_t *start, uint64_t *end, char *ovr_host, size_t ovr_cap) {
    unsigned long lbn, size;
    if (sscanf(guest, "disc0:/sce_lbn0x%lx_size0x%lx", &lbn, &size) != 2)
        return NULL;
    uint64_t ovr_sz = 0;
    if (ovr_host) ovr_host[0] = '\\0';
    if (gx_resolve_override(gx_lbn_filename(lbn), ovr_host, ovr_cap, &ovr_sz)) {
        FILE *f = fopen(ovr_host, "rb");
        if (f) {
            *start = 0;
            *end = ovr_sz;
            if (getenv("PSPRECOMP_TRACE_IO"))
                fprintf(stderr, "io: VFS override LBN 0x%lx -> %s (%llu bytes)\\n",
                        lbn, ovr_host, (unsigned long long)ovr_sz);
            return f;
        }
    }
    if (!g_umd[0]) return NULL;
    FILE *f = fopen(g_umd, "rb");
    if (!f) return NULL;
    uint64_t off = (uint64_t)lbn * 2048u;
    if (off > 0x7FFFFFFFull || fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    *start = off;
    *end = off + (uint64_t)size;
    return f;
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);

    const int raw = !strcmp(guest, "umd0:") || !strcmp(guest, "umd1:");
    FILE *f = NULL;
    uint64_t lbn_start = 0, lbn_end = 0;
    if (!strncmp(guest, "disc0:/sce_lbn0x", 16)) {
        char ovr[1024] = "";
        f = open_lbn(guest, &lbn_start, &lbn_end, ovr, sizeof ovr);
        if (ovr[0])
            snprintf(host, sizeof host, "%s", ovr);
        else
            snprintf(host, sizeof host, "%s @ %llu", g_umd[0] ? g_umd : "(no disc image set)",
                     (unsigned long long)lbn_start);
    } else if (raw) {
        if (g_umd[0]) f = fopen(g_umd, "rb");
        snprintf(host, sizeof host, "%s", g_umd[0] ? g_umd : "(no disc image set)");
    } else {
        map_path(guest, host, sizeof host);
        uint64_t ovr_sz = 0;
        /* The system font library opens its PGF files from flash0. A host
         * directory (a PSP flash0 dump or an emulator asset tree) stands in
         * for it when PSPRECOMP_FLASH0 names one. */
        if (!strncmp(guest, "flash0:/", 8)) {
            const char *root = getenv("PSPRECOMP_FLASH0");
            if (root)
                snprintf(host, sizeof host, "%s/%s", root, guest + 8);
        } else if (gx_resolve_override(gx_guest_filename(guest), host, sizeof host, &ovr_sz)) {
            lbn_start = 0;
            lbn_end = ovr_sz;
        }""",
        "iofilemgr open_lbn + hle_Open",
    )

    patch_exact(
        "build/deps/psprecomp/src/hle/iofilemgr.c",
        """    unsigned long lbn, lbn_size;
    if (!strncmp(guest, "disc0:/sce_lbn0x", 16) &&
        sscanf(guest, "disc0:/sce_lbn0x%lx_size0x%lx", &lbn, &lbn_size) == 2) {
        fill_io_stat(stat_addr, 0, (uint64_t)lbn_size, 0, 0, 0);
        psp_ret(0);
        return;
    }

    map_path(guest, host, sizeof host);
    if (!strncmp(guest, "flash0:/", 8)) {
        const char *root = getenv("PSPRECOMP_FLASH0");
        if (root) snprintf(host, sizeof host, "%s/%s", root, guest + 8);
    }""",
        """    unsigned long lbn, lbn_size;
    if (!strncmp(guest, "disc0:/sce_lbn0x", 16) &&
        sscanf(guest, "disc0:/sce_lbn0x%lx_size0x%lx", &lbn, &lbn_size) == 2) {
        uint64_t sz = (uint64_t)lbn_size;
        uint64_t ovr_sz = 0;
        if (gx_resolve_override(gx_lbn_filename(lbn), host, sizeof host, &ovr_sz))
            sz = ovr_sz;
        fill_io_stat(stat_addr, 0, sz, 0, 0, 0);
        psp_ret(0);
        return;
    }

    map_path(guest, host, sizeof host);
    uint64_t dummy_sz = 0;
    if (!strncmp(guest, "flash0:/", 8)) {
        const char *root = getenv("PSPRECOMP_FLASH0");
        if (root) snprintf(host, sizeof host, "%s/%s", root, guest + 8);
    } else {
        gx_resolve_override(gx_guest_filename(guest), host, sizeof host, &dummy_sz);
    }""",
        "iofilemgr hle_Getstat",
    )

print("\n=== 2. EBOOT patches (tagforce) ===")
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_054.c",
    """L_08049458: PSP_MARK(0x08049458u);
    /* 08049458  lui        $a2, 0x82C */
    r_a2 = 0x082C0000u;
    /* 0804945C  addiu      $a2, $a2, 20100 */
    r_a2 = r_a2 + 20100;""",
    """L_08049458: PSP_MARK(0x08049458u);
    /* 08049458  relocated 2MB cardinfo_eng.ehp buffer to 0x06400000 (avoiding 0x083E0000 .bss overflow) */
    r_a2 = 0x06400000u;""",
    "EBOOT 0x08049458 cardinfo_eng.ehp buffer -> 0x06400000",
)

patch_exact(
    "build/reloc/tagforce/tagforce_funcs_045.c",
    """L_0803B390: PSP_MARK(0x0803B390u);
    /* 0803B390  addu       $v0, $s1, $v0 */
    r_v0 = r_s1 + r_v0;
    /* 0803B394  addiu      $v0, $v0, 5464 */
    r_v0 = r_v0 + 5464;
L_0803B398: PSP_MARK(0x0803B398u);""",
    """L_0803B390: PSP_MARK(0x0803B390u);
    /* Redirect Pure GX card_idx >= 2480 to persistent extended trunk window (0x06600000) */
    if (r_s0 >= 2480u) {
        r_v0 = (r_s0 < 3480u) ? (0x06600000u + (r_s0 - 2480u) * 4u) : 0u;
        goto L_0803B398;
    }
    /* 0803B390  addu       $v0, $s1, $v0 */
    r_v0 = r_s1 + r_v0;
    /* 0803B394  addiu      $v0, $v0, 5464 */
    r_v0 = r_v0 + 5464;
L_0803B398: PSP_MARK(0x0803B398u);""",
    "EBOOT 0x0803B340 extended trunk 2480..3479",
)

patch_exact(
    "build/reloc/tagforce/tagforce_funcs_046.c",
    """    /* 0803B990  slti       $v0, $s0, 6958 */
    r_v0 = psp_slt(r_s0, (uint32_t)6958);""",
    """    /* 0803B990  slti       $v0, $s0, 7958 */
    r_v0 = psp_slt(r_s0, (uint32_t)7958);""",
    "EBOOT 0x0803B990 unlock-all 6958->7958",
)

patch_exact(
    "build/reloc/tagforce/tagforce_funcs_048.c",
    """    /* 0803DECC  slti       $v0, $s0, 6958 */
    r_v0 = psp_slt(r_s0, (uint32_t)6958);""",
    """    /* 0803DECC  slti       $v0, $s0, 7958 */
    r_v0 = psp_slt(r_s0, (uint32_t)7958);""",
    "EBOOT 0x0803DECC (0x5777192D) 6958->7958",
)

patch_exact(
    "build/reloc/tagforce/tagforce_funcs_054.c",
    """    /* 0804997C  slti       $v0, $s0, 6958 */
    r_v0 = psp_slt(r_s0, (uint32_t)6958);""",
    """    /* 0804997C  slti       $v0, $s0, 7958 */
    r_v0 = psp_slt(r_s0, (uint32_t)7958);""",
    "EBOOT 0x0804997C FindCardByPassword 6958->7958",
)

print("\n=== 3. UI & Duel PRX loop bounds (6958->7958 / 2480->3480) ===")
simple_6958 = [
    ("build/reloc/deck/deck_funcs_000.c", "r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "moddeck 01400274"),
    ("build/reloc/deck/deck_funcs_003.c", "r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "moddeck 0140886C/0140894C"),
    ("build/reloc/deck/deck_funcs_003.c", "r_v1 = psp_slt(r_s1, (uint32_t)6958);", "r_v1 = psp_slt(r_s1, (uint32_t)7958);", "moddeck 01408A04"),
    ("build/reloc/deck/deck_funcs_005.c", "r_v1 = psp_slt(r_s0, (uint32_t)6958);", "r_v1 = psp_slt(r_s0, (uint32_t)7958);", "moddeck 0140DB0C"),
    ("build/reloc/limitlist/limitlist_funcs_001.c", "r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "modlimitlist 094334D8"),
    ("build/reloc/limitlist/limitlist_funcs_002.c", "r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "modlimitlist 09435398"),
    ("build/reloc/selectcard/selectcard_funcs_002.c", "r_v0 = psp_slt(r_s1, (uint32_t)6958);", "r_v0 = psp_slt(r_s1, (uint32_t)7958);", "modselectcard 09465E74/F84"),
    ("build/reloc/selectcard/selectcard_funcs_002.c", "r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "modselectcard 094660A4"),
    ("build/reloc/selectcard/selectcard_funcs_005.c", "r_v1 = psp_slt(r_s0, (uint32_t)6958);", "r_v1 = psp_slt(r_s0, (uint32_t)7958);", "modselectcard 0946E3E8"),
    ("build/reloc/password/password_funcs_000.c", "r_at = psp_slt(r_s1, (uint32_t)6958);", "r_at = psp_slt(r_s1, (uint32_t)7958);", "modpassword 057027DC"),
    ("build/reloc/duel-swap/duelswap_funcs_004.c", "r_v1 = psp_slt(r_s0, (uint32_t)6958);", "r_v1 = psp_slt(r_s0, (uint32_t)7958);", "moddeck_swap 0350B5F8"),
]
for relp, old, new, label in simple_6958:
    p = os.path.join(ROOT, relp)
    raw = open(p, "rb").read()
    crlf = b"\r\n" in raw
    txt = raw.decode("utf-8").replace("\r\n", "\n")
    if new in txt and old not in txt:
        print(f"  [already] {label}")
        continue
    assert old in txt, f"Missing {old} in {relp}"
    out = txt.replace(old, new)
    if crlf: out = out.replace("\n", "\r\n")
    open(p, "wb").write(out.encode("utf-8"))
    print(f"  [patched] {label}")

print("\n=== 4. modduel_draw patches (funcs 040/041/073) ===")
patch_exact("build/reloc/duel-draw/dueldraw_funcs_040.c",
    "r_v0 = r_zero + 6957;", "r_v0 = r_zero + 7957;", "modduel_draw 03054A30")

for old, new, lbl in [
    ("r_v0 = psp_slt(r_v0, (uint32_t)6958);", "r_v0 = psp_slt(r_v0, (uint32_t)7958);", "draw041 6958 v0"),
    ("r_v0 = r_zero + 6957;", "r_v0 = r_zero + 7957;", "draw041 6957 v0"),
    ("r_v1 = psp_slt(r_v1, (uint32_t)6957);", "r_v1 = psp_slt(r_v1, (uint32_t)7957);", "draw041 6957 v1"),
    ("r_v1 = psp_slt(r_s1, (uint32_t)2480);", "r_v1 = psp_slt(r_s1, (uint32_t)3480);", "draw041 2480 s1"),
    ("r_s1 = r_zero + 2479;", "r_s1 = r_zero + 3479;", "draw041 2479 s1"),
    ("r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);", "draw041 6958 s0"),
    ("r_s1 = r_zero + 6957;", "r_s1 = r_zero + 7957;", "draw041 6957 s1"),
    ("r_v0 = psp_slt(r_s1, (uint32_t)6958);", "r_v0 = psp_slt(r_s1, (uint32_t)7958);", "draw041 6958 s1"),
]:
    p = os.path.join(ROOT, "build/reloc/duel-draw/dueldraw_funcs_041.c")
    raw = open(p, "rb").read()
    crlf = b"\r\n" in raw
    txt = raw.decode("utf-8").replace("\r\n", "\n")
    if new in txt and old not in txt:
        print(f"  [already] {lbl}")
        continue
    assert old in txt, f"Missing {old} in dueldraw_funcs_041.c"
    out = txt.replace(old, new)
    if crlf: out = out.replace("\n", "\r\n")
    open(p, "wb").write(out.encode("utf-8"))
    print(f"  [patched] {lbl}")

# dueldraw_funcs_073.c: struct *(0x03457974) alloc 9932->13932, offsets 9920/9924/9928 -> 13920/13924/13928, 6958->7958
p73 = os.path.join(ROOT, "build/reloc/duel-draw/dueldraw_funcs_073.c")
raw73 = open(p73, "rb").read()
crlf73 = b"\r\n" in raw73
txt73 = raw73.decode("utf-8").replace("\r\n", "\n")
if "13932" in txt73 and "r_a0 = r_zero + 9932;" not in txt73:
    print("  [already] dueldraw_funcs_073.c")
else:
    txt73 = txt73.replace("r_a0 = r_zero + 9932;", "r_a0 = r_zero + 13932;")
    txt73 = txt73.replace("psp_write32(r_v1 + 9928, r_a0);", "psp_write32(r_v1 + 13928, r_a0);")
    txt73 = txt73.replace("r_a2 = psp_read32(r_v1 + 9920);", "r_a2 = psp_read32(r_v1 + 13920);")
    txt73 = txt73.replace("psp_write32(r_s1 + 9920,", "psp_write32(r_s1 + 13920,")
    txt73 = txt73.replace("psp_read32(r_s1 + 9920)", "psp_read32(r_s1 + 13920)")
    txt73 = txt73.replace("psp_read32(r_s3 + 9920)", "psp_read32(r_s3 + 13920)")
    txt73 = txt73.replace("psp_read32(r_s0 + 9924)", "psp_read32(r_s0 + 13924)")
    txt73 = txt73.replace("psp_write32(r_s3 + 9924,", "psp_write32(r_s3 + 13924,")
    txt73 = txt73.replace("psp_read32(r_s3 + 9928)", "psp_read32(r_s3 + 13928)")
    txt73 = txt73.replace("r_v0 = psp_slt(r_s0, (uint32_t)6958);", "r_v0 = psp_slt(r_s0, (uint32_t)7958);")
    if crlf73: txt73 = txt73.replace("\n", "\r\n")
    open(p73, "wb").write(txt73.encode("utf-8"))
    print("  [patched] dueldraw_funcs_073.c (13932/13920/13924/13928/7958)")

print("\n=== 5. modcardalbum patches (stack 8000, array 0x06610000, 6958->7958, 2480->3480) ===")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423D38  addiu      $sp, $sp, -6000 */
    r_sp = r_sp + -6000;""",
    """    /* 09423D38  addiu      $sp, $sp, -8000 */
    r_sp = r_sp + -8000;""", "cardalbum stack -8000")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423D60  addiu      $a2, $zero, 4960 */
    r_a2 = r_zero + 4960;""",
    """    /* 09423D60  addiu      $a2, $zero, 6960 */
    r_a2 = r_zero + 6960;""", "cardalbum memset 6960")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423D90  slti       $v0, $s0, 6958 */
    r_v0 = psp_slt(r_s0, (uint32_t)6958);""",
    """    /* 09423D90  slti       $v0, $s0, 7958 */
    r_v0 = psp_slt(r_s0, (uint32_t)7958);""", "cardalbum loop 7958")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423DA0  addiu      $a0, $a0, -25296 */
    r_a0 = r_a0 + -25296;
    /* 09423DA4  addu       $a1, $zero, $zero */
    r_a1 = r_zero + r_zero;
    /* 09423DA8  addiu      $a2, $zero, 4902 */
    r_a2 = r_zero + 4902;""",
    """    /* 09423DA0  relocate album_ids[] to 0x06610000 (6960 bytes) */
    r_a0 = 0x06610000u;
    /* 09423DA4  addu       $a1, $zero, $zero */
    r_a1 = r_zero + r_zero;
    /* 09423DA8  addiu      $a2, $zero, 6960 */
    r_a2 = r_zero + 6960;""", "cardalbum bss clear 0x06610000 (6960)")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423E3C  addiu      $v1, $v1, -25312 */
    r_v1 = r_v1 + -25312;
    /* 09423E40  addu       $v1, $v1, $a0 */
    r_v1 = r_v1 + r_a0;
    /* 09423E44  sh         $zero, 16($v1) */""",
    """    /* 09423E3C  relocated album_ids - 16 */
    r_v1 = 0x06610000u - 16u;
    /* 09423E40  addu       $v1, $v1, $a0 */
    r_v1 = r_v1 + r_a0;
    /* 09423E44  sh         $zero, 16($v1) */""", "cardalbum write0 -> 0x06610000")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423E54  addiu      $v1, $v1, -25312 */
    r_v1 = r_v1 + -25312;
    /* 09423E58  addu       $v1, $v1, $a0 */
    r_v1 = r_v1 + r_a0;
    /* 09423E5C  sh         $s4, 16($v1) */""",
    """    /* 09423E54  relocated album_ids - 16 */
    r_v1 = 0x06610000u - 16u;
    /* 09423E58  addu       $v1, $v1, $a0 */
    r_v1 = r_v1 + r_a0;
    /* 09423E5C  sh         $s4, 16($v1) */""", "cardalbum write id -> 0x06610000")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423E6C  slti       $v1, $s1, 2480 */
    r_v1 = psp_slt(r_s1, (uint32_t)2480);""",
    """    /* 09423E6C  slti       $v1, $s1, 3480 */
    r_v1 = psp_slt(r_s1, (uint32_t)3480);""", "cardalbum 2480->3480")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423EE0  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;
    /* 09423EE4  sll        $v1, $v1, 2 */
    r_v1 = psp_sll(r_v1, 2);
    /* 09423EE8  addu       $v0, $v0, $v1 */
    r_v0 = r_v0 + r_v1;
    /* 09423EEC  jal        0x09425BA0 */
    /* 09423EF0  addiu      $a0, $v0, 16 */""",
    """    /* 09423EE0  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;
    /* 09423EE4  sll        $v1, $v1, 2 */
    r_v1 = psp_sll(r_v1, 2);
    /* 09423EE8  addu       $v0, $v0, $v1 */
    r_v0 = r_v0 + r_v1;
    /* 09423EEC  jal        0x09425BA0 */
    /* 09423EF0  addiu      $a0, $v0, 16 */""", "cardalbum copy -> 0x06610000")
patch_exact("build/reloc/cardalbum/cardalbum_funcs_001.c",
    """    /* 09423F24  addiu      $sp, $sp, 6000 */
    r_sp = r_sp + 6000;""",
    """    /* 09423F24  addiu      $sp, $sp, 8000 */
    r_sp = r_sp + 8000;""", "cardalbum stack +8000")

for old, new, lbl in [
    ("""    /* 0942469C  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;""",
     """    /* 0942469C  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;""", "cardalbum_002 0942469C"),
    ("""    /* 09424700  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;""",
     """    /* 09424700  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;""", "cardalbum_002 09424700"),
    ("""    /* 09424760  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;""",
     """    /* 09424760  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;""", "cardalbum_002 09424760"),
    ("""    /* 094247AC  addiu      $v0, $v0, -25296 */
    r_v0 = r_v0 + -25296;""",
     """    /* 094247AC  relocated album_ids */
    r_v0 = 0x06610000u;""", "cardalbum_002 094247AC"),
    ("""    /* 094247E4  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;""",
     """    /* 094247E4  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;""", "cardalbum_002 094247E4"),
    ("""    /* 09424B0C  addiu      $v0, $v0, -25312 */
    r_v0 = r_v0 + -25312;""",
     """    /* 09424B0C  relocated album_ids - 16 */
    r_v0 = 0x06610000u - 16u;""", "cardalbum_002 09424B0C"),
    ("""    /* 09424EE8  addiu      $s0, $s0, -25312 */
    r_s0 = r_s0 + -25312;""",
     """    /* 09424EE8  relocated album_ids - 16 */
    r_s0 = 0x06610000u - 16u;""", "cardalbum_002 09424EE8"),
    ("""    /* 09425ADC  addiu      $s2, $s2, -25312 */
    r_s2 = r_s2 + -25312;""",
     """    /* 09425ADC  relocated album_ids - 16 */
    r_s2 = 0x06610000u - 16u;""", "cardalbum_002 09425ADC"),
]:
    patch_exact("build/reloc/cardalbum/cardalbum_funcs_002.c", old, new, lbl)

print("\n=== 6. EBOOT out-of-band CIP/CPM tables & preload/LRU cache ===")
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_059.c",
    """    /* 0805040C  addu       $a1, $t2, $zero */
    r_a1 = r_t2 + r_zero;
    /* 08050410  addiu      $a2, $t2, 2220 */
    r_a2 = r_t2 + 2220;""",
    """    /* 0805040C  addu       $a1, $t2, $zero */
    r_a1 = r_t2 + r_zero;
    /* 08050410  out-of-band CIP/CPM index table read (copies 2048B header to GX_CIP_TBL_BASE) */
    { extern uint32_t gx_cip_prepare_tbl_read(uint32_t); r_a2 = gx_cip_prepare_tbl_read(r_t2); }""",
    "EBOOT 0x08050410 gx_cip_prepare_tbl_read",
)
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_059.c",
    """    /* 08050488  sw         $a1, 152($a0) */
    psp_write32(r_a0 + 152, r_a1);
    /* 0805048C  sw         $zero, 156($a0) */
    psp_write32(r_a0 + 156, r_zero);""",
    """    /* 08050488  sw         $a1, 152($a0) */
    psp_write32(r_a0 + 152, r_a1);
    /* Out-of-band CIP/CPM preload & LRU cache setup */
    { extern void gx_cip_setup_cache(uint32_t); gx_cip_setup_cache(r_a0); }
    /* 0805048C  sw         $zero, 156($a0) */
    psp_write32(r_a0 + 156, r_zero);""",
    "EBOOT 0x08050488 gx_cip_setup_cache",
)
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_059.c",
    """L_08050678: PSP_MARK(0x08050678u);
    /* 08050678  lw         $v0, 200($a0) */
    r_v0 = psp_read32(r_a0 + 200);
    /* 0805067C  addu       $v0, $v0, $a0 */
    r_v0 = r_v0 + r_a0;
    /* 08050680  jr         $ra */
    { uint32_t _t = r_ra;
    /* 08050684  addiu      $v0, $v0, 172 */
    r_v0 = r_v0 + 172;
    if (psp_ra_is_jump(_t)) { psp_dispatch(_t); return; } }""",
    """L_08050678: PSP_MARK(0x08050678u);
    /* 08050678  out-of-band CIP/CPM table pointer */
    {
        extern uint32_t gx_cip_tbl_addr(uint32_t);
        uint32_t _base = (psp_read32(r_a0 + 188) > 2048u) ? gx_cip_tbl_addr(r_a0) : (r_a0 + 172u);
        r_v0 = _base + psp_read32(r_a0 + 200);
    }
    /* 08050680  jr         $ra */
    { uint32_t _t = r_ra;
    if (psp_ra_is_jump(_t)) { psp_dispatch(_t); return; } }""",
    "EBOOT 0x08050678 gx_cip_tbl_addr",
)
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_059.c",
    """L_08050358: PSP_MARK(0x08050358u);
    /* 08050358  srl        $v0, $v0, 1 */
    r_v0 = psp_srl(r_v0, 1);
    /* 0805035C  addu       $v0, $v0, $s0 */
    r_v0 = r_v0 + r_s0;
    /* 08050360  addiu      $a1, $v0, 172 */
    r_a1 = r_v0 + 172;""",
    """L_08050358: PSP_MARK(0x08050358u);
    /* 08050358  srl        $v0, $v0, 1 */
    r_v0 = psp_srl(r_v0, 1);
    /* 0805035C  out-of-band CIP/CPM table base for alt-art chain */
    {
        extern uint32_t gx_cip_tbl_addr(uint32_t);
        uint32_t _base = (psp_read32(r_s0 + 188) > 2048u) ? gx_cip_tbl_addr(r_s0) : (r_s0 + 172u);
        r_a1 = _base + r_v0;
    }""",
    "EBOOT 0x0805035C gx_cip_tbl_addr",
)
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_058.c",
    """L_0804FF98: PSP_MARK(0x0804FF98u);
    /* 0804FF98  addu       $a0, $s1, $zero */
    r_a0 = r_s1 + r_zero;
    /* 0804FF9C  addiu      $s0, $s1, 172 */
    r_s0 = r_s1 + 172;""",
    """L_0804FF98: PSP_MARK(0x0804FF98u);
    /* 0804FF98  addu       $a0, $s1, $zero */
    r_a0 = r_s1 + r_zero;
    /* 0804FF9C  out-of-band CIP/CPM table base for alt-art chain */
    { extern uint32_t gx_cip_tbl_addr(uint32_t); r_s0 = (psp_read32(r_s1 + 188) > 2048u) ? gx_cip_tbl_addr(r_s1) : (r_s1 + 172u); }""",
    "EBOOT 0x0804FF9C gx_cip_tbl_addr",
)
patch_exact(
    "build/reloc/tagforce/tagforce_funcs_058.c",
    """L_08050148: PSP_MARK(0x08050148u);
    /* 08050148  jal        0x08050678 */
    /* 0805014C  addiu      $s6, $s1, 172 */
    r_s6 = r_s1 + 172;""",
    """L_08050148: PSP_MARK(0x08050148u);
    /* 08050148  jal        0x08050678 */
    /* 0805014C  out-of-band CIP/CPM table base for alt-art chain */
    { extern uint32_t gx_cip_tbl_addr(uint32_t); r_s6 = (psp_read32(r_s1 + 188) > 2048u) ? gx_cip_tbl_addr(r_s1) : (r_s1 + 172u); }""",
    "EBOOT 0x0805014C gx_cip_tbl_addr",
)

print("All Phase C patches applied cleanly.")
