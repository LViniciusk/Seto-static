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

/* M3 GE frontend: the guest thread enqueues all payloads; the GPU thread owns
 * FBOs, textures, draws, blits, readbacks, and display-buffer presentation. */
int  seto_ge_gpu_draw_mode(void);
int  seto_ge_gpu_enqueue_target(int fbo_slot, uint32_t addr, uint32_t stride,
                                int fmt, int w, int h, int force_new);
/* Invalidate any cached GE textures or FBOs overlapped by non-GE writes. */
void seto_ge_invalidate_vram(uint32_t addr, uint32_t bytes);
/* `tex` is an optional RGBA8888 snapshot (tex_w * tex_h, owned by the GPU
 * thread on success); `tex_slot` is -1 (untextured) or 0..GE_TEX_CACHE_SLOTS-1;
 * `fbo_tex_slot` is -1 (normal texture) or 0..GE_MAX_FBOS-1 (FBO-as-texture).
 * When `tex == NULL` and `tex_slot >= 0`, the GPU thread reuses that cached slot.
 * On failure the caller keeps ownership of `tex`. */
int  seto_ge_gpu_enqueue_draw(int prim, const psp_vertex *v, int count,
                              const psp_draw_state *st, int fbo_slot,
                              uint32_t *tex, int tex_slot, int fbo_tex_slot,
                              uint64_t tex_hash);
/* Upload bottom-up RGBA8888 `(w * h)` pixels into `fbo_slot`. Ownership of
 * `rgba` transfers to the GPU thread on success. */
int  seto_ge_gpu_enqueue_upload(int fbo_slot, int w, int h, uint32_t *rgba);
/* Copy a `(w x h)` region in top-left PSP coordinates between FBO slots. */
int  seto_ge_gpu_enqueue_blit(int src_slot, int sx, int sy,
                              int dst_slot, int dx, int dy, int w, int h);
/* Synchronously read back `(w x h)` bottom-up RGBA8888 and/or D24S8 from `fbo_slot`. */
int  seto_ge_gpu_readback(int fbo_slot, int w, int h,
                          uint32_t *rgba, uint32_t *ds);
/* Publish `disp_slot` (`>= 0`) or bottom-up 480x272 RAM frame `ram_rgba`
 * (ownership transfers to GPU thread when non-NULL). */
void seto_ge_gpu_present(int disp_slot, uint32_t *ram_rgba, uint32_t capture_id);
/* M4 physical scale control (1..10) and live WM_SIZE auto-resize hook. */
void seto_gl_present_resize(int client_w, int client_h);
void seto_gl_present_invalidate(void);
int  seto_ge_gpu_set_scale(int scale);
int  seto_ge_gpu_current_scale(void);

int  seto_gl_present_has_gpu_frame(void);
int  seto_ge_gpu_read_front(uint32_t *out_top_down);
void seto_gl_present_set_overlay(const uint32_t *rgba_top_down, int x, int y,
                                 int w, int h);
void seto_ge_hd_note_tex(uint64_t fnv_hash, const psp_draw_state *st);

#endif
