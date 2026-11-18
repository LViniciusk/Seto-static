#ifndef SETO_GL_PRESENT_H
#define SETO_GL_PRESENT_H

#include <windows.h>
#include <stdint.h>

#include <psprecomp/render.h>

/* Optional OpenGL presenter for the 480x272 software framebuffer.
 * Selected with SETO_PRESENT=gl; returns 0 and the caller keeps GDI on any
 * failure. All calls run on the window thread. */
int  seto_gl_present_init(HWND hwnd);
void seto_gl_present_frame(HWND hwnd, const uint32_t *rgba);
void seto_gl_present_shutdown(HWND hwnd);
int  seto_gl_present_active(void);

/* M1.1 GE frontend: the guest thread enqueues all payloads; the GPU thread
 * consumes them. Software rendering still runs for guest VRAM. */
int  seto_ge_gpu_draw_mode(void);
int  seto_ge_gpu_enqueue_target(uint32_t addr, uint32_t stride, int fmt);
/* `tex` is an optional RGBA8888 snapshot (tex_w * tex_h, owned by the GPU
 * thread on success); `tex_slot` is -1 (untextured) or 0..GE_TEX_CACHE_SLOTS-1.
 * When `tex == NULL` and `tex_slot >= 0`, the GPU thread reuses that cached slot.
 * On failure the caller keeps ownership of `tex`. */
int  seto_ge_gpu_enqueue_draw(int prim, const psp_vertex *v, int count,
                              const psp_draw_state *st, uint32_t *tex,
                              int tex_slot);
void seto_ge_gpu_present(uint32_t capture_id);

#endif
