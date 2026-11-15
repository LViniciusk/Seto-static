/* Synthetic render workloads: timing plus a byte-exact framebuffer/depth oracle. */
#include <psprecomp/hle.h>
#include <psprecomp/render.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FB PSP_VRAM_BASE
#define ZB (FB + 0x110000u)
#define TEX 0x08840000u
#define FRAMES 40

static uint32_t hash(const void *data, size_t size) {
    const unsigned char *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t random_state = 12345;
static uint32_t next(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}

int main(int argc, char **argv) {
    if (psp_mem_init() != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    if (psp_mem_map_module(0x08000000u, 0x640000u) != 0) return 1;
    for (int i = 0; i < 8; i++)
        if (psp_mem_map_extra(0x01000000u + (uint32_t)i * 0x200000u, 0x100000u) != 0) return 1;
    uint32_t clut[256];
    for (unsigned i = 0; i < 256; i++) clut[i] = i * 0x01010101u;
    for (unsigned i = 0; i < 128u * 128u; i++) psp_write32(TEX + i * 4, i * 2654435761u);
    if (argc > 1 && strcmp(argv[1], "--checked") == 0) psp_mem_watch_write(0x03FFFFFFu);

    const char *names[] = { "opaque", "textured-triangles", "alpha-sprites", "stencil-background", "4444-triangles", "display-copy" };
    /* Recorded with the pre-optimization software renderer (b9fdea4). */
    const uint32_t expected[] = { 0xC29B687F, 0xEEDAB556, 0x75C044B1, 0x69D307BB, 0xBF7F1558, 0x6300F5C5 };
    const uint64_t expected_pixels[] = { 25070720, 25059920, 7642240, 30293120, 25059920, 0 };
    uint32_t display[480 * 272];
    const psp_render_backend *r = psp_render_current();
    for (int test = 0; test < 6; test++) {
        memset(psp_mem.vram, 0x55, PSP_VRAM_SIZE);
        psp_render_reset_pixels();
        const int fmt = test == 4 ? 2 : 3;
        r->set_target(FB, 512, fmt);
        psp_draw_state st = {0};
        st.sc_x1 = 479; st.sc_y1 = 271;
        st.tex_addr = TEX; st.tex_bw = st.tex_w = st.tex_h = 128;
        st.tex_fmt = 5; st.tex_swizzle = 1; st.tex_alpha = 1;
        st.clut = (const uint8_t *)clut; st.clut_fmt = 3; st.clut_mask = 255;
        st.zaddr = ZB; st.zbw = 512; st.zfunc = 7; st.zwrite = 1;
        st.atest_func = 6; st.atest_mask = 255;
        st.blend_src = 2; st.blend_dst = 3;
        st.stencil_func = 1; st.stencil_ref = 2; st.stencil_mask = 255;
        st.stencil_zpass = 2;
        st.gouraud = 1;
        psp_vertex v[] = {
            { -21, -13, 31000, 0xFF8899AAu, -3.25f, 1.5f, 0 },
            { 479, 30, 32000, 0xAA778899u, 125.75f, -2.25f, 0 },
            { 16, 270, 33000, 0x88776655u, 0.5f, 126.5f, 0 },
            { 501, 291, 34000, 0xFFABCDEFu, 127.75f, 127.5f, 0 }
        };
        if (test == 5) {
            psp_cpu.r[PSP_REG_A0] = FB;
            psp_cpu.r[PSP_REG_A1] = 512;
            psp_cpu.r[PSP_REG_A2] = 3;
            psp_hle_call(0x289D82FEu); /* sceDisplaySetFrameBuf */
        }
        const clock_t start = clock();
        for (int frame = 0; frame < FRAMES; frame++) {
            if (test == 5) {
                for (int j = 0; j < 20; j++) if (psp_display_read(display) != 0) return 1;
                continue;
            }
            st.clear = 1; st.clear_color = st.clear_alpha = st.clear_depth = 1;
            const psp_vertex clear[] = {
                { 0, 0, 0, 0, 0, 0, 0 }, { 480, 272, 0, 0, 0, 0, 0 }
            };
            r->draw(PSP_PRIM_SPRITES, clear, 2, &st);
            st.clear = 0;
            st.tex_enable = test == 1 || test == 2 || test == 4;
            st.ztest = test == 1 || test == 4;
            st.atest = st.blend = test == 2;
            st.stencil = test == 3;
            st.stencil_func = 1; st.stencil_ref = 2; st.stencil_zpass = 2;
            for (int layer = 0; layer < 4; layer++) {
                v[0].z = (float)(31000 + layer * 100);
                v[3].z = (float)(34000 + layer * 100);
                r->draw(test == 2 ? PSP_PRIM_SPRITES : PSP_PRIM_TRIANGLE_STRIP, v, 4, &st);
            }
            if (test == 3) {
                st.stencil_func = 2; st.stencil_ref = 0; st.stencil_zpass = 0;
                r->draw(PSP_PRIM_SPRITES, clear, 2, &st);
            }
        }
        const double ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        const uint32_t sum = test == 5 ? hash(display, sizeof display) : hash(psp_mem.vram, PSP_VRAM_SIZE);
        printf("%-22s %8.2f ms hash=%08X pixels=%llu bad=%llu\n", names[test], ms, sum,
               (unsigned long long)psp_render_pixels(), (unsigned long long)psp_mem_bad_access);
        if (sum != expected[test] || psp_render_pixels() != expected_pixels[test] || psp_mem_bad_access) return 1;
    }
    /* Mix states, formats, target switches, texture writes and framebuffer feedback. */
    memset(psp_mem.vram, 0x55, PSP_VRAM_SIZE);
    psp_render_reset_pixels();
    for (int i = 0; i < 400; i++) {
        psp_draw_state st = {0};
        const int fmt = i % 4;
        r->set_target(FB + (i % 2 ? 0x88000 : 0), 512, fmt);
        st.sc_x0 = 3; st.sc_y0 = 2; st.sc_x1 = 80; st.sc_y1 = 75;
        st.tex_enable = next() & 1; st.tex_fmt = next() % 11;
        st.tex_w = 32; st.tex_h = i % 7 ? 32 : 9;
        st.tex_bw = i % 7 ? 128 : 2; st.tex_addr = i % 9 ? TEX : FB;
        st.tex_swizzle = next() & 1; st.tex_func = next() % 5;
        st.tex_alpha = next() & 1; st.tex_double = next() & 1;
        st.tex_clamp_u = next() & 1; st.tex_clamp_v = next() & 1;
        st.clut = (const uint8_t *)clut; st.clut_fmt = next() % 4; st.clut_mask = 255;
        st.gouraud = next() & 1;
        st.clear = i % 17 == 0;
        st.clear_color = next() & 1; st.clear_alpha = next() & 1; st.clear_depth = next() & 1;
        st.zaddr = ZB; st.zbw = 512; st.ztest = next() & 1;
        st.zfunc = next() % 8; st.zwrite = next() & 1;
        st.stencil = next() & 1; st.stencil_func = next() % 8;
        st.stencil_ref = next() & 255; st.stencil_mask = next() & 255;
        st.stencil_fail = next() % 6; st.stencil_zfail = next() % 6; st.stencil_zpass = next() % 6;
        st.atest = next() & 1; st.atest_func = next() % 8; st.atest_ref = next() & 255; st.atest_mask = 255;
        st.blend = next() & 1; st.blend_op = next() % 6;
        st.blend_src = next() % 11; st.blend_dst = next() % 11;
        st.blend_sfix = next(); st.blend_dfix = next(); st.write_mask = i % 3 ? 0 : next();
        psp_vertex v[4];
        for (int j = 0; j < 4; j++) {
            v[j].x = (int)(next() % 110) - 10; v[j].y = (int)(next() % 100) - 10;
            v[j].z = (float)(next() % 70000) - 1000;
            v[j].rgba = next(); v[j].u = (float)(next() % 50) - 5.25f;
            v[j].v = (float)(next() % 50) - 5.75f; v[j].behind = 0;
        }
        const uint32_t value = next();
        const uint32_t address = TEX + (next() % 4096) * 4;
        psp_write32(address, value);
        r->draw(i % 2 ? PSP_PRIM_TRIANGLE_STRIP : PSP_PRIM_SPRITES, v, 4, &st);
    }
    printf("mixed-state oracle hash=%08X pixels=%llu bad=%llu\n", hash(psp_mem.vram, PSP_VRAM_SIZE),
           (unsigned long long)psp_render_pixels(), (unsigned long long)psp_mem_bad_access);
    if (hash(psp_mem.vram, PSP_VRAM_SIZE) != 0xF39C4FDBu ||
            psp_render_pixels() != 480118 || psp_mem_bad_access) return 1;
    psp_mem_free();
    return 0;
}
