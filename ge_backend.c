/* M1.1 frontend: keep the software renderer authoritative for guest VRAM and
 * enqueue a snapshot of every supported draw for the GPU thread. */

#include "gl_present.h"
#include "ge_queue.h"

#include <psprecomp/render.h>
#include <stdio.h>

static int ge_init(int width, int height) {
    (void)width;
    (void)height;
    return 0;
}

static void ge_shutdown(void) { }

static void ge_set_target(uint32_t addr, uint32_t stride, int fmt) {
    psp_render_software.set_target(addr, stride, fmt);
    if (seto_ge_gpu_draw_mode())
        seto_ge_gpu_enqueue_target(addr, stride, fmt);
}

static int ge_state_supported(const psp_draw_state *st) {
    /* M1.1: untextured colour/clear with scissor, blend, alpha test, depth
     * and culling. Everything else waits for M2/M3 and only reaches the
     * software oracle. A clear only needs colour/depth/scissor. */
    if (st->clear) return 1;
    return !st->tex_enable && !st->stencil && !st->ctest &&
           !st->logic_enable && !st->fog_enable && !st->reg_enable &&
           st->write_mask == 0 && !st->zrange_enable;
}

static int ge_prim_supported(int prim, const psp_draw_state *st) {
    if (prim == PSP_PRIM_TRIANGLES || prim == PSP_PRIM_TRIANGLE_STRIP ||
        prim == PSP_PRIM_TRIANGLE_FAN)
        return 1;
    /* The clear primitive is a sprite pair; non-clear sprites need texturing
     * or the sprite unpacking path and stay software-only for now. */
    return st->clear && prim == PSP_PRIM_SPRITES;
}

static int ge_logged;

static void ge_draw(int prim, const psp_vertex *v, int count,
                    const psp_draw_state *st) {
    psp_render_software.draw(prim, v, count, st);
    if (!seto_ge_gpu_draw_mode()) return;
    if (!v || count <= 0 || count > GE_MAX_VERTS) return;
    if (!ge_state_supported(st) || !ge_prim_supported(prim, st)) return;
    if (seto_ge_gpu_enqueue_draw(prim, v, count, st) && !ge_logged) {
        ge_logged = 1;
        fprintf(stderr, "render: first GL draw prim=%d count=%d clear=%d\n",
                prim, count, st->clear);
    }
}

static void ge_finish(void) {
    if (psp_render_software.finish) psp_render_software.finish();
}

static void ge_present(void) {
    if (psp_render_software.present) psp_render_software.present();
    if (seto_ge_gpu_draw_mode()) seto_ge_gpu_present();
}

const psp_render_backend seto_ge_backend = {
    "gl", ge_init, ge_shutdown, ge_set_target, ge_draw, ge_finish, ge_present
};
