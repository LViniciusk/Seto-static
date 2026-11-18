/* M2 frontend: keep the software renderer authoritative for guest VRAM and
 * enqueue a snapshot of every supported draw. Textures are decoded to
 * RGBA8888 by the runtime's cache/decoder so both backends share one oracle,
 * with a lockstep producer-side cache to avoid redundant GPU uploads. */

#include "gl_present.h"
#include "ge_queue.h"

#include <psprecomp/hle.h>
#include <psprecomp/render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ge_logged_draw;
static int ge_logged_tex;
static int g_target_fmt = 3;
static uint32_t g_target_addr;
static uint32_t g_target_stride = 512u;

typedef struct {
    int valid;
    uint32_t addr;
    int w, h, fmt, swz;
    uint64_t hash;
    uint32_t lru;
} ge_prod_tex_slot;

static ge_prod_tex_slot g_prod_tex[GE_TEX_CACHE_SLOTS];
static uint32_t g_prod_lru;

static void ge_prod_cache_reset(void) {
    memset(g_prod_tex, 0, sizeof g_prod_tex);
    g_prod_lru = 0;
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
    g_target_addr = addr;
    g_target_stride = stride;
    g_target_fmt = fmt;
    psp_render_software.set_target(addr, stride, fmt);
    if (seto_ge_gpu_draw_mode())
        seto_ge_gpu_enqueue_target(addr, stride, fmt);
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
    /* M2: clear, 2D/3D textured draws, stencil, fog, specular lmode, ctest,
     * alpha test with mask, zrange/depth, blend ops/SFIX/DFIX/2x-alpha, byte masks.
     * Logic ops, partial channel bitmasks, GU_ABS blend, dst-alpha blend (4/5/8/9
     * when blend_op <= 2), and 16-bit INCR/DECR stencil stay software. */
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

/* A GE sprite is a pair of corners; the software samples it as an axis-aligned
 * rectangle with u along x and v along y, using the second corner's colour and
 * depth for every pixel. Expand each pair to two triangles with the matching
 * corner UVs. A 90-degree transposed blit is skipped (software-only for now). */
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
        if (transposed) {
            /* The software's 90-degree blit: u runs along y, v along x. */
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

static int ge_emit_chunk(int prim, const psp_vertex *verts, int nverts,
                         const psp_draw_state *st, const uint32_t *tex_px,
                         int tw, int th, int have_hash, uint64_t hash) {
    if (nverts <= 0) return 1;
    if (!seto_gl_present_active()) {
        ge_prod_cache_reset();
        return 0;
    }
    if (!tex_px) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, NULL, -1)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        ge_log_draw(prim, nverts, st, 0, 0);
        return 1;
    }
    (void)have_hash;
    int hit = -1;
    int victim = 0;
    for (int i = 0; i < GE_TEX_CACHE_SLOTS; i++) {
        const ge_prod_tex_slot *s = &g_prod_tex[i];
        if (s->valid && s->addr == st->tex_addr && s->w == tw && s->h == th &&
            s->fmt == st->tex_fmt && s->swz == st->tex_swizzle && s->hash == hash) {
            hit = i;
            break;
        }
        if (!g_prod_tex[victim].valid) continue;
        if (!s->valid || s->lru < g_prod_tex[victim].lru) victim = i;
    }
    if (hit >= 0) {
        if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, NULL, hit)) {
            ge_note_reject("queue", prim, nverts, st);
            return 0;
        }
        g_prod_tex[hit].lru = ++g_prod_lru;
        ge_log_draw(prim, nverts, st, tw, th);
        return 1;
    }
    uint32_t *copy = ge_tex_copy(tex_px, tw, th);
    if (!copy) return 0;
    if (!seto_ge_gpu_enqueue_draw(prim, verts, nverts, st, copy, victim)) {
        free(copy);
        g_prod_tex[victim].valid = 0;
        ge_note_reject("queue", prim, nverts, st);
        return 0;
    }
    g_prod_tex[victim].valid = 1;
    g_prod_tex[victim].addr = st->tex_addr;
    g_prod_tex[victim].w = tw;
    g_prod_tex[victim].h = th;
    g_prod_tex[victim].fmt = st->tex_fmt;
    g_prod_tex[victim].swz = st->tex_swizzle;
    g_prod_tex[victim].hash = hash;
    g_prod_tex[victim].lru = ++g_prod_lru;
    ge_log_draw(prim, nverts, st, tw, th);
    return 1;
}

typedef struct {
    psp_vertex verts[GE_MAX_VERTS];
    int n;
    const psp_draw_state *st;
    const uint32_t *tex_px;
    int tw, th;
    uint64_t hash;
    int failed;
} ge_tri_batch;

static void ge_batch_flush(ge_tri_batch *b) {
    if (b->failed || b->n <= 0) { b->n = 0; return; }
    if (!ge_emit_chunk(PSP_PRIM_TRIANGLES, b->verts, b->n, b->st,
                       b->tex_px, b->tw, b->th, b->tex_px != NULL, b->hash)) {
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
        if (area == 0) return;
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

static void ge_draw(int prim, const psp_vertex *v, int count,
                    const psp_draw_state *st) {
    psp_render_software.draw(prim, v, count, st);
    if (!seto_ge_gpu_draw_mode()) return;
    if (g_target_addr != 0u &&
        (g_target_stride != 512u ||
         (g_target_addr != 0x04000000u && g_target_addr != 0x04088000u &&
          g_target_addr != 0x04044000u))) {
        ge_note_reject("offscreen", prim, count, st);
        return;
    }
    if (!v || count <= 0) return;
    if (!ge_state_supported(st)) { ge_note_reject("state", prim, count, st); return; }
    if (!ge_prim_supported(prim, st)) { ge_note_reject("prim", prim, count, st); return; }

    const uint32_t *tex_px = NULL;
    int tw = 0, th = 0;
    uint64_t tex_hash = 0;
    if (st->tex_enable && !st->clear) {
        if (!psp_render_texture_rgba(st, &tex_px, &tw, &th)) {
            ge_note_reject("decode", prim, count, st);
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

    if (prim == PSP_PRIM_SPRITES) {
        /* 85 sprites fill 510 verts (<= GE_MAX_VERTS) with two triangles each.
         * Hardware sprites never backface-cull (unlike triangles). */
        const int max_pairs = GE_MAX_VERTS / 6;
        psp_vertex sprites[GE_MAX_VERTS];
        psp_draw_state sp_st = *st;
        sp_st.cull_enable = 0;
        for (int off = 0; off + 1 < count; off += max_pairs * 2) {
            int pairs = (count - off) / 2;
            if (pairs > max_pairs) pairs = max_pairs;
            const int n = ge_sprites_to_tris(v + off, pairs * 2, sprites);
            if (!n) continue;
            if (!ge_emit_chunk(PSP_PRIM_TRIANGLES, sprites, n, &sp_st,
                               tex_px, tw, th, tex_px != NULL, tex_hash))
                return;
        }
        return;
    }

    if (prim == PSP_PRIM_POINTS) {
        psp_vertex pts[GE_MAX_VERTS];
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (v[i].behind) continue;
            pts[n++] = v[i];
            if (n == GE_MAX_VERTS) {
                if (!ge_emit_chunk(PSP_PRIM_POINTS, pts, n, st,
                                   tex_px, tw, th, tex_px != NULL, tex_hash))
                    return;
                n = 0;
            }
        }
        if (n > 0)
            ge_emit_chunk(PSP_PRIM_POINTS, pts, n, st, tex_px, tw, th, tex_px != NULL, tex_hash);
        return;
    }

    if (prim == PSP_PRIM_LINES || prim == PSP_PRIM_LINE_STRIP) {
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
                if (!ge_emit_chunk(PSP_PRIM_LINES, lines, n, st,
                                   tex_px, tw, th, tex_px != NULL, tex_hash))
                    return;
                n = 0;
            }
            lines[n++] = va;
            lines[n++] = vb;
        }
        if (n > 0)
            ge_emit_chunk(PSP_PRIM_LINES, lines, n, st, tex_px, tw, th, tex_px != NULL, tex_hash);
        return;
    }

    ge_tri_batch b;
    b.n = 0;
    b.st = st;
    b.tex_px = tex_px;
    b.tw = tw;
    b.th = th;
    b.hash = tex_hash;
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
}

static void ge_finish(void) {
    if (psp_render_software.finish) psp_render_software.finish();
}

static void ge_present(void) {
    static uint32_t g_present_seq;
    if (psp_render_software.present) psp_render_software.present();
    if (!seto_ge_gpu_draw_mode()) return;

    const uint32_t seq = ++g_present_seq;
    uint32_t cap = 0;
    const char *dir = getenv("SETO_GL_SHOT_DIR");
    if (dir && dir[0]) {
        const char *ev = getenv("SETO_GL_SHOT_EVERY");
        int every = ev ? atoi(ev) : 30;
        if (every <= 0) every = 30;
        if ((seq % (uint32_t)every) == 0u) {
            char path[260];
            snprintf(path, sizeof path, "%s/sw-%05u.ppm", dir, seq);
            psp_display_capture(path);
            cap = seq;
        }
    }
    seto_ge_gpu_present(cap);
}

const psp_render_backend seto_ge_backend = {
    "gl", ge_init, ge_shutdown, ge_set_target, ge_draw, ge_finish, ge_present
};
