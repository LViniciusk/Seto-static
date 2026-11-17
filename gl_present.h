#ifndef SETO_GL_PRESENT_H
#define SETO_GL_PRESENT_H

#include <windows.h>
#include <stdint.h>

/* Optional OpenGL presenter for the 480x272 software framebuffer.
 * Selected with SETO_PRESENT=gl; returns 0 and the caller keeps GDI on any
 * failure. All calls run on the window thread. */
int  seto_gl_present_init(HWND hwnd);
void seto_gl_present_frame(HWND hwnd, const uint32_t *rgba);
void seto_gl_present_shutdown(HWND hwnd);
int  seto_gl_present_active(void);

#endif
