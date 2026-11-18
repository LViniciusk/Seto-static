/* M3 frontend: GPU FBOs are authoritative in hardware mode (`SETO_GL_SHOT_DIR`
 * and `SETO_GE_SW_ORACLE=1` unset), supporting up to GE_MAX_FBOS VRAM render
 * targets, FBO-as-texture with self-texturing, per-draw software fallbacks
 * with VRAM<->FBO sync/upload, GE_TRXKICK block transfers, and display-aware
 * presentation (including RAM display buffers such as decoded .pmf frames). */

#include "gl_present.h"
#include "ge_queue.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ge_logged_draw;
static int ge_logged_tex;
static int g_target_fmt = 3;
static uint32_t g_target_addr = PSP_VRAM_BASE;
static uint32_t g_target_stride = 512u;

typedef struct {
    int valid;
    uint32_t addr;
    uint32_t end_addr;
    int bw, w, h, fmt, swz;
    int has_raw_hash;
    uint64_t raw_hash;
    uint64_t hash;
    uint32_t lru;
} ge_prod_tex_slot;

typedef struct {
    int      used;
    uint32_t addr;
    uint32_t stride;
    int      fmt;
    int      w;
    int      h;
    int      used_h;
    uint32_t zaddr;
    int      zbw;
    int      dirty;
    int      z_dirty;
    int      stencil_written;
    int      drawn_since_present;
    int      has_vram_sig;
    uint64_t last_vram_sig;
    uint32_t lru;
} ge_prod_fbo;

static ge_prod_tex_slot g_prod_tex[GE_TEX_CACHE_SLOTS];
static uint32_t g_prod_lru;

static ge_prod_fbo g_prod_fbos[GE_MAX_FBOS];
static uint8_t g_fbo_force_new[GE_MAX_FBOS];
static uint32_t g_fbo_lru;
static int g_cur_gpu_slot = -1;

static uint32_t ge_vram_addr(uint32_t addr) {
    const uint32_t raw = addr & PSP_ADDR_MASK;
    if (raw < PSP_VRAM_SIZE) return PSP_VRAM_BASE | raw;
    return raw;
}

static int ge_is_vram_target(uint32_t addr, uint32_t stride, int fmt) {
    return addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE &&
           stride > 0u && stride <= 1024u && fmt >= 0 && fmt <= 3;
}

static int ge_sw_oracle(void) {
    const char *dir = getenv("SETO_GL_SHOT_DIR");
    if (dir && dir[0]) return 1;
    const char *orac = getenv("SETO_GE_SW_ORACLE");
    return orac && orac[0] && orac[0] != '0';
}

static uint32_t ge_fmt_bpp(int fmt) {
    return fmt == 3 ? 4u : 2u;
}

static uint32_t ge_tex_byte_span(int bw, int tw, int th, int fmt) {
    const uint64_t stride = (uint64_t)(bw > 0 ? bw : tw);
    const uint64_t rows = (uint64_t)(th > 0 ? th : 1);
    uint64_t bytes = 0;
    if (fmt == PSP_TPSM_T4 || fmt == PSP_TPSM_DXT1)
        bytes = (stride * rows + 1u) / 2u;
    else if (fmt == PSP_TPSM_T8 || fmt == PSP_TPSM_DXT3 || fmt == PSP_TPSM_DXT5)
        bytes = stride * rows;
    else if (fmt == PSP_TPSM_8888 || fmt == PSP_TPSM_T32)
        bytes = stride * rows * 4u;
    else
        bytes = stride * rows * 2u;
    if (bytes > PSP_VRAM_SIZE) bytes = PSP_VRAM_SIZE;
    return (uint32_t)bytes;
}

static void ge_prod_cache_reset(void) {
    memset(g_prod_tex, 0, sizeof g_prod_tex);
    g_prod_lru = 0;
    memset(g_prod_fbos, 0, sizeof g_prod_fbos);
    memset(g_fbo_force_new, 0, sizeof g_fbo_force_new);
    g_fbo_lru = 0;
    g_cur_gpu_slot = -1;
}

static void ge_invalidate_tex_range(uint32_t addr, uint32_t bytes) {
    if (bytes == 0u) return;
    const uint64_t end = (uint64_t)addr + (uint64_t)bytes;
    for (int i = 0; i < GE_TEX_CACHE_SLOTS; i++) {
        if (!g_prod_tex[i].valid) continue;
        if ((uint64_t)g_prod_tex[i].addr < end && (uint64_t)g_prod_tex[i].end_addr > addr)
            g_prod_tex[i].valid = 0;
    }
}

void seto_ge_invalidate_vram(uint32_t addr, uint32_t bytes) {
    if (bytes == 0u) return;
    addr = ge_vram_addr(addr);
    ge_invalidate_tex_range(addr, bytes);
    if (addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE)
        return;
    const uint64_t end = (uint64_t)addr + (uint64_t)bytes;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        ge_prod_fbo *f = &g_prod_fbos[i];
        if (!f->used) continue;
        const uint64_t fend = (uint64_t)f->addr +
                              (uint64_t)f->h * (uint64_t)f->stride * ge_fmt_bpp(f->fmt);
        if ((uint64_t)f->addr < end && fend > addr) {
            f->used = 0;
            g_fbo_force_new[i] = 1;
            if (g_cur_gpu_slot == i) g_cur_gpu_slot = -1;
        }
    }
}

static int ge_max_vram_rows(uint32_t addr, uint32_t stride, int fmt) {
    const uint32_t bpp = ge_fmt_bpp(fmt);
    if (addr < PSP_VRAM_BASE || addr >= PSP_VRAM_BASE + PSP_VRAM_SIZE || stride == 0u)
        return 1;
    const uint32_t avail = (PSP_VRAM_BASE + PSP_VRAM_SIZE) - addr;
    const uint32_t row_bytes = stride * bpp;
    uint32_t rows = avail / row_bytes;
    if (rows < 1u) rows = 1u;
    if (rows > 1024u) rows = 1024u;
    return (int)rows;
}

static int ge_fbo_safe_rows(int slot) {
    const ge_prod_fbo *f = &g_prod_fbos[slot];
    const uint32_t bpp = ge_fmt_bpp(f->fmt);
    uint32_t limit = PSP_VRAM_BASE + PSP_VRAM_SIZE;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        if (i == slot || !g_prod_fbos[i].used) continue;
        if (g_prod_fbos[i].addr > f->addr && g_prod_fbos[i].addr < limit)
            limit = g_prod_fbos[i].addr;
    }
    if (f->zaddr > f->addr && f->zaddr < limit)
        limit = f->zaddr;
    const uint32_t row_bytes = f->stride * bpp;
    uint32_t rows = (limit - f->addr) / (row_bytes ? row_bytes : 4u);
    if (rows < 1u) rows = 1u;
    if (rows > (uint32_t)f->h) rows = (uint32_t)f->h;
    return (int)rows;
}

static int ge_sync_fbo_to_vram(int slot) {
    if (slot < 0 || slot >= GE_MAX_FBOS) return 0;
    ge_prod_fbo *f = &g_prod_fbos[slot];
    if (!f->used || !f->dirty) return 0;
    if (!seto_ge_gpu_draw_mode() || !seto_gl_present_active() || ge_sw_oracle())
        return 0;

    int w = f->w;
    if (w > (int)f->stride) w = (int)f->stride;
    int h = f->used_h > 0 ? f->used_h : 1;
    const int safe_h = ge_fbo_safe_rows(slot);
    if (h > safe_h) h = safe_h;
    if (w <= 0 || h <= 0) return 0;

    const size_t npx = (size_t)w * (size_t)h;
    uint32_t *rgba = (uint32_t *)malloc(npx * sizeof(uint32_t));
    if (!rgba) return -1;
    const int need_ds = f->z_dirty || (f->stencil_written && f->fmt != 0);
    uint32_t *ds = need_ds ? (uint32_t *)malloc(npx * sizeof(uint32_t)) : NULL;
    if (need_ds && !ds) {
        free(rgba);
        return -1;
    }

    if (seto_ge_gpu_readback(slot, w, h, rgba, ds) != 0) {
        free(ds);
        free(rgba);
        return -1;
    }

    const uint32_t bpp = ge_fmt_bpp(f->fmt);
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = rgba + (size_t)(h - 1 - y) * (size_t)w;
        const uint32_t *drow = ds ? ds + (size_t)(h - 1 - y) * (size_t)w : NULL;
        const uint32_t raddr = f->addr + (uint32_t)y * f->stride * bpp;
        void *vp = psp_mem_unwatched_ptr(raddr, (uint32_t)w * bpp);
        if (f->fmt == 3) {
            if (vp && !(f->stencil_written && drow)) {
                memcpy(vp, srow, (size_t)w * 4u);
            } else if (vp) {
                uint32_t *dst32 = (uint32_t *)vp;
                for (int x = 0; x < w; x++)
                    dst32[x] = (srow[x] & 0x00FFFFFFu) | ((drow[x] & 0xFFu) << 24);
            } else {
                for (int x = 0; x < w; x++) {
                    uint32_t c = srow[x];
                    if (f->stencil_written && drow)
                        c = (c & 0x00FFFFFFu) | ((drow[x] & 0xFFu) << 24);
                    psp_write32(raddr + (uint32_t)x * 4u, c);
                }
            }
        } else {
            uint16_t *dst16 = (uint16_t *)vp;
            for (int x = 0; x < w; x++) {
                uint32_t c = srow[x];
                if (f->stencil_written && drow && f->fmt != 0)
                    c = (c & 0x00FFFFFFu) | ((drow[x] & 0xFFu) << 24);
                const uint16_t p = (uint16_t)psp_color_pack(c, f->fmt);
                if (dst16) dst16[x] = p;
                else psp_write16(raddr + (uint32_t)x * 2u, p);
            }
        }

        if (f->z_dirty && drow && f->zaddr && f->zbw > 0) {
            const uint32_t zr = f->zaddr + (uint32_t)y * (uint32_t)f->zbw * 2u;
            if (zr >= PSP_VRAM_BASE &&
                (uint64_t)zr + (uint64_t)w * 2u <= PSP_VRAM_BASE + PSP_VRAM_SIZE) {
                uint16_t *zp = (uint16_t *)psp_mem_unwatched_ptr(zr, (uint32_t)w * 2u);
                for (int x = 0; x < w; x++) {
                    const uint32_t d24 = drow[x] >> 8;
                    const uint16_t z16 = (uint16_t)(((16777215ULL - d24) * 65535ULL +
                                                     8388607ULL) / 16777215ULL);
                    if (zp) zp[x] = z16;
                    else psp_write16(zr + (uint32_t)x * 2u, z16);
                }
            }
        }
    }

    f->dirty = 0;
    f->z_dirty = 0;
    free(ds);
    free(rgba);
    return 0;
}

static void ge_sync_overlapping_fbos(uint32_t addr, uint32_t bytes) {
    if (bytes == 0u || ge_sw_oracle()) return;
    const uint64_t end = (uint64_t)addr + (uint64_t)bytes;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        const ge_prod_fbo *f = &g_prod_fbos[i];
        if (!f->used || !f->dirty) continue;
        const int h = f->used_h > 0 ? f->used_h : f->h;
        const uint64_t fend = (uint64_t)f->addr +
                              (uint64_t)h * (uint64_t)f->stride * ge_fmt_bpp(f->fmt);
        if ((uint64_t)f->addr < end && fend > addr)
            ge_sync_fbo_to_vram(i);
    }
}

static int ge_upload_vram_to_fbo(int slot, int h) {
    if (slot < 0 || slot >= GE_MAX_FBOS) return 0;
    ge_prod_fbo *f = &g_prod_fbos[slot];
    if (!f->used) return 0;
    if (h <= 0) h = 1;
    const int max_r = ge_max_vram_rows(f->addr, f->stride, f->fmt);
    if (h > f->h) h = f->h;
    if (h > max_r) h = max_r;
    const int w = f->w;
    if (w <= 0 || h <= 0) return 0;

    uint32_t *rgba = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!rgba) return 0;

    const uint32_t bpp = ge_fmt_bpp(f->fmt);
    for (int y = 0; y < h; y++) {
        uint32_t *drow = rgba + (size_t)(h - 1 - y) * (size_t)w;
        const uint32_t raddr = f->addr + (uint32_t)y * f->stride * bpp;
        const void *vp = psp_mem_unwatched_ptr(raddr, (uint32_t)w * bpp);
        if (f->fmt == 3) {
            if (vp) memcpy(drow, vp, (size_t)w * 4u);
            else {
                for (int x = 0; x < w; x++)
                    drow[x] = psp_read32(raddr + (uint32_t)x * 4u);
            }
        } else {
            const uint16_t *src16 = (const uint16_t *)vp;
            for (int x = 0; x < w; x++) {
                const uint16_t p = src16 ? src16[x]
                                         : psp_read16(raddr + (uint32_t)x * 2u);
                drow[x] = psp_color_expand(p, f->fmt);
            }
        }
    }

    if (!seto_ge_gpu_enqueue_upload(slot, w, h, rgba)) {
        free(rgba);
        return 0;
    }
    if (h > f->used_h) f->used_h = h;
    ge_invalidate_tex_range(f->addr, (uint32_t)h * f->stride * bpp);
    return 1;
}

static int ge_vram_has_nonzero(uint32_t addr, int w, int h, uint32_t stride, int fmt) {
    const uint32_t bpp = ge_fmt_bpp(fmt);
    const int max_r = ge_max_vram_rows(addr, stride, fmt);
    if (h > max_r) h = max_r;
    for (int y = 0; y < h; y++) {
        const uint32_t raddr = addr + (uint32_t)y * stride * bpp;
        const void *vp = psp_mem_unwatched_ptr(raddr, (uint32_t)w * bpp);
        if (vp) {
            const uint32_t nbytes = (uint32_t)w * bpp;
            const uint8_t *p = (const uint8_t *)vp;
            for (uint32_t i = 0; i < nbytes; i++)
                if (p[i]) return 1;
        } else if (bpp == 4u) {
            for (int x = 0; x < w; x++)
                if (psp_read32(raddr + (uint32_t)x * 4u)) return 1;
        } else {
            for (int x = 0; x < w; x++)
                if (psp_read16(raddr + (uint32_t)x * 2u)) return 1;
        }
    }
    return 0;
}

static int ge_find_fbo_exact(uint32_t addr) {
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        if (g_prod_fbos[i].used && g_prod_fbos[i].addr == addr)
            return i;
    }
    return -1;
}

static int ge_fbo_ensure(uint32_t addr, uint32_t stride, int fmt, int min_h) {
    if (!ge_is_vram_target(addr, stride, fmt)) return -1;
    const int max_r = ge_max_vram_rows(addr, stride, fmt);
    const int want_w = (int)stride;
    int init_h = min_h > 272 ? min_h : 272;
    if (init_h > max_r) init_h = max_r;

    int free_idx = -1;
    int victim = 0;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        ge_prod_fbo *f = &g_prod_fbos[i];
        if (!f->used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (f->addr == addr) {
            if (f->stride == stride && f->fmt == fmt) {
                int new_h = f->h;
                if (min_h > new_h) {
                    new_h = min_h;
                    if (new_h > max_r) new_h = max_r;
                }
                if (new_h > f->h || g_cur_gpu_slot != i) {
                    if (!seto_ge_gpu_enqueue_target(i, f->addr, f->stride,
                                                    f->fmt, f->w, new_h, 0))
                        return -1;
                    f->h = new_h;
                    g_cur_gpu_slot = i;
                }
                f->lru = ++g_fbo_lru;
                return i;
            }
            free_idx = i;
            break;
        }
        if (g_prod_fbos[victim].used && f->lru < g_prod_fbos[victim].lru)
            victim = i;
    }

    int slot = free_idx >= 0 ? free_idx : victim;
    ge_prod_fbo *f = &g_prod_fbos[slot];
    if (f->used && free_idx < 0)
        ge_sync_fbo_to_vram(slot);

    memset(f, 0, sizeof *f);
    f->used = 1;
    f->addr = addr;
    f->stride = stride;
    f->fmt = fmt;
    f->w = want_w;
    f->h = init_h;
    f->lru = ++g_fbo_lru;

    const int force_new = g_fbo_force_new[slot];
    g_fbo_force_new[slot] = 0;
    if (!seto_ge_gpu_enqueue_target(slot, addr, stride, fmt, want_w, init_h, force_new)) {
        f->used = 0;
        return -1;
    }
    g_cur_gpu_slot = slot;

    if (!ge_sw_oracle() && ge_vram_has_nonzero(addr, want_w, init_h, stride, fmt)) {
        ge_upload_vram_to_fbo(slot, init_h);
        f->dirty = 0;
    }
    return slot;
}

static int ge_find_tex_fbo(const psp_draw_state *st) {
    if (!st || !st->tex_enable || st->clear) return -1;
    if (st->tex_swizzle || st->tex_fmt < 0 || st->tex_fmt > 3) return -1;
    const uint32_t taddr = ge_vram_addr(st->tex_addr);
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        const ge_prod_fbo *f = &g_prod_fbos[i];
        if (!f->used || f->addr != taddr) continue;
        if (st->tex_fmt == f->fmt && (uint32_t)st->tex_bw == f->stride &&
            st->tex_w > 0 && st->tex_h > 0 &&
            st->tex_w <= f->w && st->tex_h <= f->h)
            return i;
    }
    return -1;
}

static void ge_trace_text(int prim, const psp_vertex *v, int count, const psp_draw_state *st,
                          int tw, int th, const uint32_t *px) {
    static DWORD t0;
    static int n;
    if (!getenv("SETO_GE_TRACE_TEXT")) return;
    if (!t0) t0 = GetTickCount();
    if (GetTickCount() - t0 < 25000 || n >= 40) return;
    n++;
    fprintf(stderr, "text: #%d prim=%d n=%d %dx%d fmt=%d swz=%d func=%d ta=%d "
            "at=%d/%d/%d bl=%d/%d/%d sc=%d,%d-%d,%d reg=%d:%d,%d-%d,%d "
            "v0=%d,%d u%g v%g %08X v1=%d,%d u%g v%g %08X\n", n, prim, count, tw, th,
            st->tex_fmt, st->tex_swizzle, st->tex_func, st->tex_alpha,
            st->atest, st->atest_func, st->atest_ref, st->blend, st->blend_src,
            st->blend_dst, st->sc_x0, st->sc_y0, st->sc_x1, st->sc_y1,
            st->reg_enable, st->reg_x0, st->reg_y0, st->reg_x1, st->reg_y1,
            v[0].x, v[0].y, v[0].u, v[0].v, v[0].rgba,
            v[count > 1 ? 1 : 0].x, v[count > 1 ? 1 : 0].y,
            v[count > 1 ? 1 : 0].u, v[count > 1 ? 1 : 0].v,
            v[count > 1 ? 1 : 0].rgba);
    if (n == 1) {
        FILE *f = fopen("build/tex-text.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", tw, th);
            for (int i = 0; i < tw * th; i++) {
                fputc((int)(px[i] & 0xFF), f);
                fputc((int)((px[i] >> 8) & 0xFF), f);
                fputc((int)((px[i] >> 16) & 0xFF), f);
            }
            fclose(f);
        }
    }
}

static int ge_init(int width, int height) {
    (void)width;
    (void)height;
    ge_prod_cache_reset();
    return 0;
}

static void ge_shutdown(void) {
    ge_prod_cache_reset();
}

static void ge_set_target(uint32_t addr, uint32_t stride, int fmt) {
    addr = ge_vram_addr(addr);
    g_target_addr = addr;
    g_target_stride = stride;
    g_target_fmt = fmt;
    psp_render_software.set_target(addr, stride, fmt);
}

static int ge_rgb_mask_supported(uint32_t write_mask) {
    const uint32_t rm = write_mask & 0xFFu;
    const uint32_t gm = (write_mask >> 8) & 0xFFu;
    const uint32_t bm = (write_mask >> 16) & 0xFFu;
    return (rm == 0u || rm == 0xFFu) &&
           (gm == 0u || gm == 0xFFu) &&
           (bm == 0u || bm == 0xFFu);
}

static int ge_state_supported(const psp_draw_state *st) {
    if (!ge_rgb_mask_supported(st->write_mask)) return 0;
    if (st->clear) return 1;
    if (st->logic_enable) return 0;
    if (st->stencil && g_target_fmt != 3 &&
        (st->stencil_fail >= 4 || st->stencil_zfail >= 4 || st->stencil_zpass >= 4))
        return 0;
    if (st->blend) {
        if (st->blend_op >= 5) return 0;
        if (st->blend_op <= 2) {
            if ((st->blend_src >= 4 && st->blend_src <= 5) ||
                (st->blend_src >= 8 && st->blend_src <= 9) ||
                (st->blend_dst >= 4 && st->blend_dst <= 5) ||
                (st->blend_dst >= 8 && st->blend_dst <= 9))
                return 0;
        }
    }
    return 1;
}

static void ge_note_reject(const char *why, int prim, int count,
                           const psp_draw_state *st) {
    typedef struct {
        const char *why;
        int prim, bop, bsrc, bdst;
        uint32_t addr, stride, mask;
        int logic;
    } reject_sig;
    static reject_sig seen[16];
    static int nseen;
    const reject_sig sig = {
        why, prim,
        st->blend ? st->blend_op : -1,
        st->blend ? st->blend_src : -1,
        st->blend ? st->blend_dst : -1,
        g_target_addr, g_target_stride, st->write_mask, st->logic_enable
    };
    for (int i = 0; i < nseen; i++) {
        if (seen[i].why == sig.why && seen[i].prim == sig.prim &&
            seen[i].addr == sig.addr && seen[i].stride == sig.stride &&
            seen[i].logic == sig.logic && seen[i].bop == sig.bop &&
            seen[i].bsrc == sig.bsrc && seen[i].bdst == sig.bdst &&
            seen[i].mask == sig.mask)
            return;
    }
    if (nseen >= 16) return;
    seen[nseen++] = sig;
    fprintf(stderr, "render: GL reject %s prim=%d count=%d target=%08X/%u/%d "
            "tex=%dx%d fmt=%d blend=%d:%d/%d/%d sf=%06X df=%06X "
            "stencil=%d ctest=%d logic=%d mask=%08X\n",
            why, prim, count, g_target_addr, g_target_stride, g_target_fmt,
            st->tex_w, st->tex_h, st->tex_fmt,
            st->blend, st->blend_op, st->blend_src, st->blend_dst,
            st->blend_sfix & 0xFFFFFFu, st->blend_dfix & 0xFFFFFFu,
            st->stencil, st->ctest, st->logic_enable, st->write_mask);
}

static int ge_prim_supported(int prim, const psp_draw_state *st) {
    (void)st;
    return prim >= PSP_PRIM_POINTS && prim <= PSP_PRIM_SPRITES;
}

static int ge_sprites_to_tris(const psp_vertex *v, int count, psp_vertex *out) {
    int n = 0;
    if ((count & 1) || count <= 0) return 0;
    for (int i = 0; i + 1 < count; i += 2) {
        const psp_vertex *a = &v[i], *b = &v[i + 1];
        if (a->x == b->x || a->y == b->y) continue;
        const int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
        const int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
        const float sdx = (float)(b->x - a->x), sdy = (float)(b->y - a->y);
        const float tdu = b->u - a->u, tdv = b->v - a->v;
        const float du = tdu / sdx, dv = tdv / sdy;
        const float adx = sdx < 0 ? -sdx : sdx, ady = sdy < 0 ? -sdy : sdy;
        const float adu = tdu < 0 ? -tdu : tdu, adv = tdv < 0 ? -tdv : tdv;
        const float e1 = adu - ady, e2 = adv - adx;
        const int transposed = adx != ady && adu != adx &&
                               e1 > -0.01f && e1 < 0.01f &&
                               e2 > -0.01f && e2 < 0.01f;
        psp_vertex tl = *b, tr = *b, bl = *b, br = *b;
        tl.x = x0; tl.y = y0;
        tr.x = x1; tr.y = y0;
        bl.x = x0; bl.y = y1;
        br.x = x1; br.y = y1;
        tl.fx = (float)x0; tl.fy = (float)y0; tl.clip_w = 1.0f;
        tr.fx = (float)x1; tr.fy = (float)y0; tr.clip_w = 1.0f;
        bl.fx = (float)x0; bl.fy = (float)y1; bl.clip_w = 1.0f;
        br.fx = (float)x1; br.fy = (float)y1; br.clip_w = 1.0f;
        if (transposed) {
            const float duy = tdu / sdy, dvx = tdv / sdx;
            tl.u = a->u + (float)(y0 - a->y) * duy;
            tl.v = a->v + (float)(x0 - a->x) * dvx;
            tr.u = tl.u;
            tr.v = a->v + (float)(x1 - a->x) * dvx;
            bl.u = a->u + (float)(y1 - a->y) * duy;
            bl.v = tl.v;
            br.u = bl.u;
            br.v = tr.v;
        } else {
            tl.u = a->u + (float)(x0 - a->x) * du;
            tl.v = a->v + (float)(y0 - a->y) * dv;
            tr.u = a->u + (float)(x1 - a->x) * du; tr.v = tl.v;
            bl.u = tl.u; bl.v = a->v + (float)(y1 - a->y) * dv;
            br.u = tr.u; br.v = bl.v;
        }
        out[n++] = tl; out[n++] = bl; out[n++] = tr;
        out[n++] = tr; out[n++] = bl; out[n++] = br;
    }
    return n;
}

static uint64_t ge_tex_hash(const uint32_t *px, int w, int h) {
    uint64_t hval = 14695981039346656037ULL;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        hval ^= (uint64_t)px[i];
        hval *= 1099511628211ULL;
    }
    return hval ^ (hval >> 33);
}

static uint32_t *ge_tex_copy(const uint32_t *px, int w, int h) {
    const size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    uint32_t *tex = (uint32_t *)malloc(bytes);
    if (tex) memcpy(tex, px, bytes);
    return tex;
}

static void ge_log_draw(int prim, int n, const psp_draw_state *st, int tw, int th) {
    if (ge_logged_draw >= 12) return;
    fprintf(stderr, "render: GL draw prim=%d n=%d clear=%d tex=%dx%d func=%d "
            "blend=%d atest=%d\n", prim, n, st->clear, tw, th, st->tex_func,
            st->blend, st->atest);
    ge_logged_draw++;
}

static uint64_t ge_fnv_bytes(uint64_t h, const uint8_t *p, size_t n) {
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ULL;
    }
    for (; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t ge_fnv_u32(uint64_t h, uint32_t v) {
    h ^= (uint64_t)v;
    return h * 1099511628211ULL;
}

static int ge_compute_raw_tex_hash(const psp_draw_state *st, uint64_t *out_hash) {
    static const int TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
    if (!st || !out_hash || st->tex_w <= 0 || st->tex_h <= 0 || st->tex_bw <= 0 ||
        st->tex_fmt < 0 || st->tex_fmt > PSP_TPSM_DXT5)
        return 0;
    if (st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32 && !st->clut)
        return 0;
    if (st->tex_fmt >= PSP_TPSM_DXT1 && st->tex_bw < 4)
        return 0;

    uint64_t size;
    if (st->tex_fmt >= PSP_TPSM_DXT1) {
        size = ((uint64_t)((st->tex_h - 1) / 4) * (uint32_t)(st->tex_bw / 4) +
                (uint32_t)((st->tex_w - 1) / 4) + 1u) *
               (st->tex_fmt == PSP_TPSM_DXT1 ? 8u : 16u);
    } else {
        const uint32_t bpp = (uint32_t)TEX_BPP[st->tex_fmt];
        const uint64_t row = (uint64_t)(uint32_t)st->tex_bw * bpp / 8u;
        const uint64_t x = (uint64_t)(uint32_t)(st->tex_w - 1) * bpp / 8u;
        const uint32_t y = (uint32_t)(st->tex_h - 1);
        if (st->tex_swizzle) {
            const uint64_t blocks = row >= 16u ? row / 16u : 1u;
            uint64_t last = (y / 8u * blocks) * 128u + (y & 7u) * 16u;
            if (y >= 8u) {
                const uint64_t prev = ((y / 8u - 1u) * blocks) * 128u + 112u;
                if (prev > last) last = prev;
            }
            size = last + x / 16u * 128u + (x & 15u);
        } else {
            size = y * row + x;
        }
        size += bpp < 8u ? 1u : bpp / 8u;
    }
    if (size == 0u || size > (uint64_t)PSP_VRAM_SIZE * 2u) return 0;

    const uint8_t *raw = (const uint8_t *)psp_mem_unwatched_ptr(st->tex_addr, (uint32_t)size);
    if (!raw) return 0;

    uint64_t h = 14695981039346656037ULL;
    h = ge_fnv_u32(h, (uint32_t)st->tex_bw);
    h = ge_fnv_u32(h, (uint32_t)st->tex_w);
    h = ge_fnv_u32(h, (uint32_t)st->tex_h);
    h = ge_fnv_u32(h, (uint32_t)st->tex_fmt);
    h = ge_fnv_u32(h, (uint32_t)st->tex_swizzle);
    h = ge_fnv_bytes(h, raw, (size_t)size);
    if (st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32) {
        h = ge_fnv_u32(h, (uint32_t)st->clut_fmt);
        h = ge_fnv_u32(h, (uint32_t)st->clut_shift);
        h = ge_fnv_u32(h, (uint32_t)st->clut_mask);
        h = ge_fnv_u32(h, (uint32_t)st->clut_start);
        h = ge_fnv_bytes(h, st->clut, 1024u);
    }
    *out_hash = h ^ (h >> 33);
    return 1;
}

static int ge_emit_chunk(int prim, const psp_vertex *verts, int nverts,
                         const psp_draw_state *st, int fbo_slot,
                         const uint32_t *tex_px, int tw, int th,
                         uint64_t hash, int fbo_tex_slot,
                         int pre_hit_slot, int has_raw_hash, uint64_t raw_hash) {
    if (nverts <= 0) return 1;
    if (!seto_gl_present_active()) {
        ge_prod_cache_reset();
        return 0;
    }
    if (fbo_tex_slot >= 0) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, fbo_slot,
                                      NULL, -1, fbo_tex_slot, 0u)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        ge_log_draw(prim, nverts, st, st->tex_w, st->tex_h);
        return 1;
    }
    if (pre_hit_slot >= 0) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, fbo_slot,
                                      NULL, pre_hit_slot, -1, hash)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        g_prod_tex[pre_hit_slot].lru = ++g_prod_lru;
        ge_log_draw(prim, nverts, st, tw, th);
        return 1;
    }
    if (!tex_px) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, fbo_slot,
                                      NULL, -1, -1, 0u)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        ge_log_draw(prim, nverts, st, 0, 0);
        return 1;
    }

    const uint32_t taddr = ge_vram_addr(st->tex_addr);
    int hit = -1;
    int victim = 0;
    for (int i = 0; i < GE_TEX_CACHE_SLOTS; i++) {
        const ge_prod_tex_slot *s = &g_prod_tex[i];
        if (s->valid && s->addr == taddr && s->bw == st->tex_bw &&
            s->w == tw && s->h == th && s->fmt == st->tex_fmt &&
            s->swz == st->tex_swizzle && s->hash == hash) {
            hit = i;
            break;
        }
        if (!g_prod_tex[victim].valid) continue;
        if (!s->valid || s->lru < g_prod_tex[victim].lru) victim = i;
    }
    if (hit >= 0) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, fbo_slot,
                                      NULL, hit, -1, hash)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        g_prod_tex[hit].lru = ++g_prod_lru;
        if (has_raw_hash) {
            g_prod_tex[hit].has_raw_hash = 1;
            g_prod_tex[hit].raw_hash = raw_hash;
        }
        ge_log_draw(prim, nverts, st, tw, th);
        return 1;
    }
    uint32_t *copy = ge_tex_copy(tex_px, tw, th);
    if (!copy) return 0;
    seto_ge_hd_note_tex(hash, st);
    if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, fbo_slot,
                                  copy, victim, -1, hash)) {
        free(copy);
        g_prod_tex[victim].valid = 0;
        ge_note_reject("queue", prim, nverts, st);
        return 0;
    }
    g_prod_tex[victim].valid = 1;
    g_prod_tex[victim].addr = taddr;
    g_prod_tex[victim].end_addr =
        taddr + ge_tex_byte_span(st->tex_bw, tw, th, st->tex_fmt);
    g_prod_tex[victim].bw = st->tex_bw;
    g_prod_tex[victim].w = tw;
    g_prod_tex[victim].h = th;
    g_prod_tex[victim].fmt = st->tex_fmt;
    g_prod_tex[victim].swz = st->tex_swizzle;
    g_prod_tex[victim].has_raw_hash = has_raw_hash;
    g_prod_tex[victim].raw_hash = raw_hash;
    g_prod_tex[victim].hash = hash;
    g_prod_tex[victim].lru = ++g_prod_lru;
    ge_log_draw(prim, nverts, st, tw, th);
    return 1;
}

typedef struct {
    psp_vertex verts[GE_MAX_VERTS];
    int n;
    const psp_draw_state *st;
    int fbo_slot;
    const uint32_t *tex_px;
    int tw, th;
    uint64_t hash;
    int fbo_tex_slot;
    int pre_hit_slot;
    int has_raw_hash;
    uint64_t raw_hash;
    int failed;
} ge_tri_batch;

static void ge_batch_flush(ge_tri_batch *b) {
    if (b->failed || b->n <= 0) { b->n = 0; return; }
    if (!ge_emit_chunk(PSP_PRIM_TRIANGLES, b->verts, b->n, b->st, b->fbo_slot,
                       b->tex_px, b->tw, b->th, b->hash, b->fbo_tex_slot,
                       b->pre_hit_slot, b->has_raw_hash, b->raw_hash)) {
        b->failed = 1;
    }
    b->n = 0;
}

static void ge_batch_push_tri(ge_tri_batch *b, const psp_vertex *a,
                              const psp_vertex *bv, const psp_vertex *c) {
    if (b->failed) return;
    if (a->behind || bv->behind || c->behind) return;
    if (!b->st->clear) {
        const int64_t area = (int64_t)(bv->x - a->x) * (int64_t)(c->y - a->y) -
                             (int64_t)(bv->y - a->y) * (int64_t)(c->x - a->x);
        if (area == 0) {
            if (b->st->through || (a->clip_w <= 0.0f && bv->clip_w <= 0.0f))
                return;
            const float farea = (bv->fx - a->fx) * (c->fy - a->fy) -
                                (bv->fy - a->fy) * (c->fx - a->fx);
            if (farea > -1e-6f && farea < 1e-6f) return;
        }
    }
    psp_vertex va = *a, vb = *bv, vc = *c;
    if (!b->st->gouraud && !b->st->clear) {
        va.rgba = vb.rgba = vc.rgba;
        va.spec = vb.spec = vc.spec;
    }
    if (b->n + 3 > 510) ge_batch_flush(b);
    if (b->failed) return;
    b->verts[b->n++] = va;
    b->verts[b->n++] = vb;
    b->verts[b->n++] = vc;
}

static void ge_sw_fallback_draw(int prim, const psp_vertex *v, int count,
                                const psp_draw_state *st, int slot, int draw_h) {
    const int oracle = ge_sw_oracle();
    if (!oracle) {
        if (slot >= 0) ge_sync_fbo_to_vram(slot);
        else ge_sync_overlapping_fbos(g_target_addr,
                                      (uint32_t)draw_h * g_target_stride * ge_fmt_bpp(g_target_fmt));
        if (st && st->tex_enable && !st->clear) {
            ge_sync_overlapping_fbos(ge_vram_addr(st->tex_addr),
                                     ge_tex_byte_span(st->tex_bw, st->tex_w, st->tex_h, st->tex_fmt));
        }
    }
    psp_render_software.draw(prim, v, count, st);
    if (slot >= 0) {
        ge_prod_fbo *f = &g_prod_fbos[slot];
        int up_h = f->used_h > draw_h ? f->used_h : draw_h;
        const int safe_h = ge_fbo_safe_rows(slot);
        if (up_h > safe_h) up_h = safe_h;
        if (ge_upload_vram_to_fbo(slot, up_h))
            f->dirty = 0;
        f->drawn_since_present = 1;
    } else {
        ge_invalidate_tex_range(g_target_addr,
                                (uint32_t)draw_h * g_target_stride * ge_fmt_bpp(g_target_fmt));
    }
}

static void ge_draw(int prim, const psp_vertex *v, int count,
                    const psp_draw_state *st) {
    const int oracle = ge_sw_oracle();
    if (!seto_ge_gpu_draw_mode() || !seto_gl_present_active()) {
        psp_render_software.draw(prim, v, count, st);
        return;
    }
    if (!v || count <= 0 || !st) return;

    int draw_h = st->sc_y1 >= 0 ? st->sc_y1 + 1 : 272;
    if (st->reg_enable && st->reg_y1 >= 0 && st->reg_y1 + 1 < draw_h)
        draw_h = st->reg_y1 + 1;
    if (draw_h < 1) draw_h = 1;
    if (draw_h > 1024) draw_h = 1024;

    if (!ge_is_vram_target(g_target_addr, g_target_stride, g_target_fmt)) {
        ge_note_reject("offscreen", prim, count, st);
        ge_sw_fallback_draw(prim, v, count, st, -1, draw_h);
        return;
    }

    int slot = ge_fbo_ensure(g_target_addr, g_target_stride, g_target_fmt, draw_h);
    if (slot < 0) {
        psp_render_software.draw(prim, v, count, st);
        return;
    }

    if (!ge_state_supported(st)) {
        ge_note_reject("state", prim, count, st);
        ge_sw_fallback_draw(prim, v, count, st, slot, draw_h);
        return;
    }
    if (!ge_prim_supported(prim, st)) {
        ge_note_reject("prim", prim, count, st);
        ge_sw_fallback_draw(prim, v, count, st, slot, draw_h);
        return;
    }

    const uint32_t *tex_px = NULL;
    int tw = 0, th = 0;
    uint64_t tex_hash = 0;
    int fbo_tex_slot = -1;
    int pre_hit_slot = -1;
    int has_raw_hash = 0;
    uint64_t raw_hash = 0;
    if (st->tex_enable && !st->clear) {
        fbo_tex_slot = ge_find_tex_fbo(st);
        if (fbo_tex_slot < 0) {
            if (!oracle) {
                ge_sync_overlapping_fbos(
                    ge_vram_addr(st->tex_addr),
                    ge_tex_byte_span(st->tex_bw, st->tex_w, st->tex_h, st->tex_fmt));
            }
            const char *trace_txt = getenv("SETO_TRACE_TEXT");
            has_raw_hash = ge_compute_raw_tex_hash(st, &raw_hash);
            if (has_raw_hash && (!trace_txt || !trace_txt[0])) {
                const uint32_t taddr = ge_vram_addr(st->tex_addr);
                for (int i = 0; i < GE_TEX_CACHE_SLOTS; i++) {
                    const ge_prod_tex_slot *s = &g_prod_tex[i];
                    if (s->valid && s->has_raw_hash && s->addr == taddr &&
                        s->bw == st->tex_bw && s->w == st->tex_w &&
                        s->h == st->tex_h && s->fmt == st->tex_fmt &&
                        s->swz == st->tex_swizzle && s->raw_hash == raw_hash) {
                        pre_hit_slot = i;
                        tw = s->w;
                        th = s->h;
                        tex_hash = s->hash;
                        break;
                    }
                }
            }
            if (pre_hit_slot < 0) {
                if (!psp_render_texture_rgba(st, &tex_px, &tw, &th)) {
                    ge_note_reject("decode", prim, count, st);
                    ge_sw_fallback_draw(prim, v, count, st, slot, draw_h);
                    return;
                }
                tex_hash = ge_tex_hash(tex_px, tw, th);
                if (!ge_logged_tex) {
                    ge_logged_tex = 1;
                    fprintf(stderr, "render: first GL texture %dx%d fmt=%d\n", tw, th,
                            st->tex_fmt);
                }
                if (tw <= 256 && th <= 256)
                    ge_trace_text(prim, v, count, st, tw, th, tex_px);
            }
        }
    }

    if (oracle)
        psp_render_software.draw(prim, v, count, st);

    int emitted_ok = 1;
    if (prim == PSP_PRIM_SPRITES) {
        const int max_pairs = GE_MAX_VERTS / 6;
        psp_vertex sprites[GE_MAX_VERTS];
        psp_draw_state sp_st = *st;
        sp_st.cull_enable = 0;
        for (int off = 0; off + 1 < count; off += max_pairs * 2) {
            int pairs = (count - off) / 2;
            if (pairs > max_pairs) pairs = max_pairs;
            const int n = ge_sprites_to_tris(v + off, pairs * 2, sprites);
            if (!n) continue;
            if (!ge_emit_chunk(PSP_PRIM_TRIANGLES, sprites, n, &sp_st, slot,
                               tex_px, tw, th, tex_hash, fbo_tex_slot,
                               pre_hit_slot, has_raw_hash, raw_hash)) {
                emitted_ok = 0;
                break;
            }
            if (tex_px && pre_hit_slot < 0) pre_hit_slot = -1; /* slot updated after 1st chunk */
        }
    } else if (prim == PSP_PRIM_POINTS) {
        psp_vertex pts[GE_MAX_VERTS];
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (v[i].behind) continue;
            pts[n++] = v[i];
            if (n == GE_MAX_VERTS) {
                if (!ge_emit_chunk(PSP_PRIM_POINTS, pts, n, st, slot,
                                   tex_px, tw, th, tex_hash, fbo_tex_slot,
                                   pre_hit_slot, has_raw_hash, raw_hash)) {
                    emitted_ok = 0;
                    break;
                }
                n = 0;
            }
        }
        if (emitted_ok && n > 0) {
            if (!ge_emit_chunk(PSP_PRIM_POINTS, pts, n, st, slot,
                               tex_px, tw, th, tex_hash, fbo_tex_slot,
                               pre_hit_slot, has_raw_hash, raw_hash))
                emitted_ok = 0;
        }
    } else if (prim == PSP_PRIM_LINES || prim == PSP_PRIM_LINE_STRIP) {
        psp_vertex lines[GE_MAX_VERTS];
        int n = 0;
        const int step = (prim == PSP_PRIM_LINES) ? 2 : 1;
        for (int i = 0; i + 1 < count; i += step) {
            const psp_vertex *a = &v[i], *bv = &v[i + 1];
            if (a->behind || bv->behind) continue;
            psp_vertex va = *a, vb = *bv;
            if (!st->gouraud && !st->clear) {
                va.rgba = vb.rgba;
                va.spec = vb.spec;
            }
            if (n + 2 > GE_MAX_VERTS) {
                if (!ge_emit_chunk(PSP_PRIM_LINES, lines, n, st, slot,
                                   tex_px, tw, th, tex_hash, fbo_tex_slot,
                                   pre_hit_slot, has_raw_hash, raw_hash)) {
                    emitted_ok = 0;
                    break;
                }
                n = 0;
            }
            lines[n++] = va;
            lines[n++] = vb;
        }
        if (emitted_ok && n > 0) {
            if (!ge_emit_chunk(PSP_PRIM_LINES, lines, n, st, slot,
                               tex_px, tw, th, tex_hash, fbo_tex_slot,
                               pre_hit_slot, has_raw_hash, raw_hash))
                emitted_ok = 0;
        }
    } else {
        ge_tri_batch b;
        b.n = 0;
        b.st = st;
        b.fbo_slot = slot;
        b.tex_px = tex_px;
        b.tw = tw;
        b.th = th;
        b.hash = tex_hash;
        b.fbo_tex_slot = fbo_tex_slot;
        b.pre_hit_slot = pre_hit_slot;
        b.has_raw_hash = has_raw_hash;
        b.raw_hash = raw_hash;
        b.failed = 0;
        if (prim == PSP_PRIM_TRIANGLES) {
            for (int i = 0; i + 2 < count; i += 3)
                ge_batch_push_tri(&b, &v[i], &v[i + 1], &v[i + 2]);
        } else if (prim == PSP_PRIM_TRIANGLE_STRIP) {
            for (int i = 0; i + 2 < count; i++) {
                if (i & 1) ge_batch_push_tri(&b, &v[i + 1], &v[i], &v[i + 2]);
                else       ge_batch_push_tri(&b, &v[i], &v[i + 1], &v[i + 2]);
            }
        } else if (prim == PSP_PRIM_TRIANGLE_FAN) {
            for (int i = 1; i + 1 < count; i++)
                ge_batch_push_tri(&b, &v[0], &v[i], &v[i + 1]);
        }
        ge_batch_flush(&b);
        if (b.failed) emitted_ok = 0;
    }

    if (!emitted_ok) {
        if (!oracle) ge_sw_fallback_draw(prim, v, count, st, slot, draw_h);
        return;
    }

    ge_prod_fbo *f = &g_prod_fbos[slot];
    const int safe_h = ge_fbo_safe_rows(slot);
    int uh = draw_h;
    if (uh > safe_h) uh = safe_h;
    if (uh > f->used_h) f->used_h = uh;
    f->dirty = 1;
    f->drawn_since_present = 1;
    f->lru = ++g_fbo_lru;

    if ((st->clear && st->clear_depth) || (!st->clear && st->ztest && st->zwrite)) {
        const uint32_t za = ge_vram_addr(st->zaddr);
        if (za >= PSP_VRAM_BASE && za < PSP_VRAM_BASE + PSP_VRAM_SIZE && st->zbw > 0) {
            f->zaddr = za;
            f->zbw = st->zbw;
            f->z_dirty = 1;
        }
    }
    if (!st->clear && st->stencil)
        f->stencil_written = 1;

    ge_invalidate_tex_range(f->addr,
                            (uint32_t)f->used_h * f->stride * ge_fmt_bpp(f->fmt));
}

static void ge_finish(void) {
    if (psp_render_software.finish) psp_render_software.finish();
}

static int ge_transfer(uint32_t src, int src_bw, int sx, int sy,
                       uint32_t dst, int dst_bw, int dx, int dy,
                       int w, int h, int bpp) {
    if (!seto_ge_gpu_draw_mode() || !seto_gl_present_active())
        return 0;

    src = ge_vram_addr(src);
    dst = ge_vram_addr(dst);
    const int src_slot = ge_find_fbo_exact(src);
    const int dst_slot = ge_find_fbo_exact(dst);
    const int oracle = ge_sw_oracle();

    if (src_slot >= 0 && dst_slot >= 0) {
        ge_prod_fbo *sf = &g_prod_fbos[src_slot];
        ge_prod_fbo *df = &g_prod_fbos[dst_slot];
        if ((int)sf->stride == src_bw && (int)df->stride == dst_bw &&
            sf->fmt == df->fmt && (int)ge_fmt_bpp(sf->fmt) == bpp &&
            sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0 &&
            sx + w <= sf->w && sy + h <= sf->h &&
            dx + w <= df->w && dy + h <= df->h) {
            if (oracle) {
                psp_render_software_transfer(src, src_bw, sx, sy,
                                             dst, dst_bw, dx, dy, w, h, bpp);
            }
            if (seto_ge_gpu_enqueue_blit(src_slot, sx, sy, dst_slot, dx, dy, w, h)) {
                if (dy + h > df->used_h) {
                    const int safe = ge_fbo_safe_rows(dst_slot);
                    df->used_h = (dy + h) < safe ? (dy + h) : safe;
                }
                df->dirty = 1;
                df->drawn_since_present = 1;
                ge_invalidate_tex_range(df->addr,
                                        (uint32_t)df->used_h * df->stride * ge_fmt_bpp(df->fmt));
                return 1;
            }
        }
    }

    const uint32_t s_end_row = (uint32_t)(sy + h);
    const uint32_t d_end_row = (uint32_t)(dy + h);
    if (!oracle) {
        if (src_slot >= 0) ge_sync_fbo_to_vram(src_slot);
        else ge_sync_overlapping_fbos(src, s_end_row * (uint32_t)src_bw * (uint32_t)bpp);

        if (dst_slot >= 0) {
            const ge_prod_fbo *df = &g_prod_fbos[dst_slot];
            const int full_overwrite = (dx == 0 && dy == 0 && w >= df->w && h >= df->used_h);
            if (!full_overwrite) ge_sync_fbo_to_vram(dst_slot);
        } else {
            ge_sync_overlapping_fbos(dst, d_end_row * (uint32_t)dst_bw * (uint32_t)bpp);
        }
    }

    psp_render_software_transfer(src, src_bw, sx, sy, dst, dst_bw, dx, dy, w, h, bpp);

    if (dst_slot >= 0) {
        ge_prod_fbo *df = &g_prod_fbos[dst_slot];
        int up_h = (int)d_end_row > df->used_h ? (int)d_end_row : df->used_h;
        const int safe = ge_fbo_safe_rows(dst_slot);
        if (up_h > safe) up_h = safe;
        if (ge_upload_vram_to_fbo(dst_slot, up_h))
            df->dirty = 0;
        df->drawn_since_present = 1;
    } else {
        ge_invalidate_tex_range(dst, d_end_row * (uint32_t)dst_bw * (uint32_t)bpp);
    }
    return 1;
}

static void ge_sync(void) {
    if (!seto_ge_gpu_draw_mode() || !seto_gl_present_active() || ge_sw_oracle())
        return;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        if (g_prod_fbos[i].used && g_prod_fbos[i].dirty)
            ge_sync_fbo_to_vram(i);
    }
}

static __thread int g_in_raw_disp_read;

static uint32_t *ge_capture_ram_frame_bottom_up(void) {
    uint32_t top_down[480 * 272];
    g_in_raw_disp_read = 1;
    const int ok = (psp_display_read(top_down) == 0);
    g_in_raw_disp_read = 0;
    if (!ok) return NULL;
    uint32_t *out = (uint32_t *)malloc(sizeof top_down);
    if (!out) return NULL;
    for (int y = 0; y < 272; y++)
        memcpy(out + (size_t)(271 - y) * 480u, top_down + (size_t)y * 480u, 480u * 4u);
    return out;
}

static uint64_t ge_vram_sample_sig(uint32_t addr, uint32_t stride, int fmt) {
    if (!ge_is_vram_target(addr, stride, fmt)) return 0u;
    const uint32_t bpp = ge_fmt_bpp(fmt);
    const int max_r = ge_max_vram_rows(addr, stride, fmt);
    const int rows = max_r < 272 ? max_r : 272;
    const int cols = (int)stride < 480 ? (int)stride : 480;
    uint64_t h = 14695981039346656037ULL;
    for (int sy = 4; sy < rows; sy += 17) {
        const uint32_t raddr = addr + (uint32_t)sy * stride * bpp;
        const void *vp = psp_mem_unwatched_ptr(raddr, (uint32_t)cols * bpp);
        for (int sx = 8; sx < cols; sx += 31) {
            uint32_t v;
            if (vp) {
                v = bpp == 4u ? ((const uint32_t *)vp)[sx]
                              : (uint32_t)((const uint16_t *)vp)[sx];
            } else {
                v = bpp == 4u ? psp_read32(raddr + (uint32_t)sx * 4u)
                              : (uint32_t)psp_read16(raddr + (uint32_t)sx * 2u);
            }
            h ^= (uint64_t)v;
            h *= 1099511628211ULL;
        }
    }
    return h ^ (h >> 32);
}

static void ge_present(void) {
    static uint32_t g_present_seq;
    if (psp_render_software.present) psp_render_software.present();
    if (!seto_ge_gpu_draw_mode() || !seto_gl_present_active()) return;

    uint32_t disp_addr = 0, disp_stride = 0;
    int disp_fmt = 3;
    psp_display_get_framebuf(&disp_addr, &disp_stride, &disp_fmt);
    disp_addr = ge_vram_addr(disp_addr);

    int disp_slot = ge_find_fbo_exact(disp_addr);
    const int oracle = ge_sw_oracle();
    if (disp_slot >= 0) {
        ge_prod_fbo *df = &g_prod_fbos[disp_slot];
        if (!df->drawn_since_present && df->has_vram_sig) {
            const uint64_t cur_sig = ge_vram_sample_sig(disp_addr, disp_stride, disp_fmt);
            if (cur_sig != df->last_vram_sig) {
                df->used = 0;
                g_fbo_force_new[disp_slot] = 1;
                if (g_cur_gpu_slot == disp_slot) g_cur_gpu_slot = -1;
                disp_slot = -1;
            }
        }
    }
    if (disp_slot >= 0) {
        ge_prod_fbo *df = &g_prod_fbos[disp_slot];
        if (!oracle && df->dirty && getenv("SETO_GE_EAGER_SYNC"))
            ge_sync_fbo_to_vram(disp_slot);
        df->drawn_since_present = 0;
        df->last_vram_sig = ge_vram_sample_sig(disp_addr, disp_stride, disp_fmt);
        df->has_vram_sig = 1;
    }

    const uint32_t seq = ++g_present_seq;
    uint32_t cap = 0;
    const char *dir = getenv("SETO_GL_SHOT_DIR");
    const char *hdir = getenv("SETO_GL_HIRES_SHOT_DIR");
    if ((dir && dir[0]) || (hdir && hdir[0])) {
        const char *ev = getenv("SETO_GL_SHOT_EVERY");
        int every = ev ? atoi(ev) : 30;
        if (every <= 0) every = 30;
        if ((seq % (uint32_t)every) == 0u) {
            if (dir && dir[0]) {
                char path[260];
                snprintf(path, sizeof path, "%s/sw-%05u.ppm", dir, seq);
                psp_display_capture(path);
            }
            cap = seq;
        }
    }

    uint32_t *ram_frame = NULL;
    if (disp_slot < 0)
        ram_frame = ge_capture_ram_frame_bottom_up();
    seto_ge_gpu_present(disp_slot, ram_frame, cap);
}

static int ge_read_display(uint32_t *out) {
    if (!out || g_in_raw_disp_read || ge_sw_oracle() ||
        !seto_ge_gpu_draw_mode() || !seto_gl_present_active() ||
        !seto_gl_present_has_gpu_frame())
        return -1;
    uint32_t disp_addr = 0, disp_stride = 0;
    int disp_fmt = 3;
    psp_display_get_framebuf(&disp_addr, &disp_stride, &disp_fmt);
    disp_addr = ge_vram_addr(disp_addr);
    if (!ge_is_vram_target(disp_addr, disp_stride, disp_fmt))
        return -1;
    return seto_ge_gpu_read_front(out);
}

const psp_render_backend seto_ge_backend = {
    "gl", ge_init, ge_shutdown, ge_set_target, ge_draw, ge_finish, ge_present,
    ge_transfer, ge_sync, ge_read_display
};
