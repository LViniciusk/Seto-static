/* M0: optional OpenGL 3.3 presenter for the 480x272 software framebuffer.
 *
 * The guest/GE still render into the runtime's framebuffer. This module only
 * uploads that frame as a texture and draws it with a Lanczos2 shader, so the
 * window can be scaled with GPU filtering. It is the context/swapchain base
 * for a future GE backend; the software renderer remains the default. */

#include "gl_present.h"

#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_SCREEN_W 480
#define GL_SCREEN_H 272

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static HWND g_hwnd;
static HDC g_dc;
static HGLRC g_ctx;
static int g_active;
static int g_filter;
static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint g_u_tex, g_u_texel, g_u_mode;
static PFNWGLSWAPINTERVALEXTPROC g_swap_interval;

static PFNGLCREATESHADERPROC p_glCreateShader;
static PFNGLSHADERSOURCEPROC p_glShaderSource;
static PFNGLCOMPILESHADERPROC p_glCompileShader;
static PFNGLGETSHADERIVPROC p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC p_glCreateProgram;
static PFNGLATTACHSHADERPROC p_glAttachShader;
static PFNGLLINKPROGRAMPROC p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC p_glUseProgram;
static PFNGLDELETESHADERPROC p_glDeleteShader;
static PFNGLDELETEPROGRAMPROC p_glDeleteProgram;
static PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
static PFNGLUNIFORM1IPROC p_glUniform1i;
static PFNGLUNIFORM2FPROC p_glUniform2f;
static PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
static PFNGLDELETEVERTEXARRAYSPROC p_glDeleteVertexArrays;
static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
static PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;

static const char *vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *fs_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform int u_mode;\n"
    "float lanczos2(float x) {\n"
    "  x = abs(x);\n"
    "  if (x < 1e-4) return 1.0;\n"
    "  if (x >= 2.0) return 0.0;\n"
    "  float p = 3.14159265 * x;\n"
    "  return 2.0 * sin(p) * sin(p * 0.5) / (p * p);\n"
    "}\n"
    "void main() {\n"
    "  if (u_mode < 2) { o_color = texture(u_tex, v_uv); return; }\n"
    "  vec2 uv = v_uv * (1.0 / u_texel) - 0.5;\n"
    "  vec2 base = floor(uv);\n"
    "  vec2 f = uv - base;\n"
    "  vec4 sum = vec4(0.0); float wsum = 0.0;\n"
    "  for (int j = -1; j <= 2; j++) for (int i = -1; i <= 2; i++) {\n"
    "    float wx = lanczos2(float(i) - f.x);\n"
    "    float wy = lanczos2(float(j) - f.y);\n"
    "    vec2 st = (base + vec2(float(i), float(j)) + 0.5) * u_texel;\n"
    "    sum += texture(u_tex, st) * wx * wy;\n"
    "    wsum += wx * wy;\n"
    "  }\n"
    "  o_color = sum / wsum;\n"
    "}\n";

static const float verts[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f
};

#define SETO_GL_LOAD(type, name)                                                   \
    do {                                                                      \
        p_##name = (type)wglGetProcAddress(#name);                            \
        if (!p_##name) {                                                      \
            fprintf(stderr, "gl: missing %s\n", #name);                       \
            return 0;                                                         \
        }                                                                     \
    } while (0)

static int load_gl(void) {
    SETO_GL_LOAD(PFNGLCREATESHADERPROC, glCreateShader);
    SETO_GL_LOAD(PFNGLSHADERSOURCEPROC, glShaderSource);
    SETO_GL_LOAD(PFNGLCOMPILESHADERPROC, glCompileShader);
    SETO_GL_LOAD(PFNGLGETSHADERIVPROC, glGetShaderiv);
    SETO_GL_LOAD(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
    SETO_GL_LOAD(PFNGLCREATEPROGRAMPROC, glCreateProgram);
    SETO_GL_LOAD(PFNGLATTACHSHADERPROC, glAttachShader);
    SETO_GL_LOAD(PFNGLLINKPROGRAMPROC, glLinkProgram);
    SETO_GL_LOAD(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
    SETO_GL_LOAD(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
    SETO_GL_LOAD(PFNGLUSEPROGRAMPROC, glUseProgram);
    SETO_GL_LOAD(PFNGLDELETESHADERPROC, glDeleteShader);
    SETO_GL_LOAD(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
    SETO_GL_LOAD(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    SETO_GL_LOAD(PFNGLUNIFORM1IPROC, glUniform1i);
    SETO_GL_LOAD(PFNGLUNIFORM2FPROC, glUniform2f);
    SETO_GL_LOAD(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    SETO_GL_LOAD(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    SETO_GL_LOAD(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
    SETO_GL_LOAD(PFNGLGENBUFFERSPROC, glGenBuffers);
    SETO_GL_LOAD(PFNGLBINDBUFFERPROC, glBindBuffer);
    SETO_GL_LOAD(PFNGLBUFFERDATAPROC, glBufferData);
    SETO_GL_LOAD(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
    SETO_GL_LOAD(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
    SETO_GL_LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    return 1;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = p_glCreateShader(type);
    const GLchar *s = (const GLchar *)src;
    GLint ok = 0;
    p_glShaderSource(sh, 1, &s, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[2048];
        GLsizei len = 0;
        p_glGetShaderInfoLog(sh, sizeof log, &len, log);
        fprintf(stderr, "gl: shader compile failed: %.*s\n", (int)len, log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int make_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLint ok = 0;
    if (!vs || !fs) {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        return 0;
    }
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        GLchar log[2048];
        GLsizei len = 0;
        p_glGetProgramInfoLog(g_prog, sizeof log, &len, log);
        fprintf(stderr, "gl: program link failed: %.*s\n", (int)len, log);
        p_glDeleteProgram(g_prog);
        g_prog = 0;
        return 0;
    }
    g_u_tex = p_glGetUniformLocation(g_prog, "u_tex");
    g_u_texel = p_glGetUniformLocation(g_prog, "u_texel");
    g_u_mode = p_glGetUniformLocation(g_prog, "u_mode");
    p_glUseProgram(g_prog);
    p_glUniform1i(g_u_tex, 0);
    return 1;
}

static void gl_cleanup(void) {
    if (g_ctx) {
        if (g_prog && p_glDeleteProgram) p_glDeleteProgram(g_prog);
        if (g_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_vao);
        if (g_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_vbo);
        if (g_tex) glDeleteTextures(1, &g_tex);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_ctx);
    }
    if (g_dc && g_hwnd) ReleaseDC(g_hwnd, g_dc);
    g_prog = g_vao = g_vbo = g_tex = 0;
    g_ctx = NULL;
    g_dc = NULL;
    g_active = 0;
}

int seto_gl_present_init(HWND hwnd) {
    const char *present = getenv("SETO_PRESENT");
    const char *f;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC dummy;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_ctx;

    if (!present || strcmp(present, "gl") != 0) return 0;
    f = getenv("SETO_FILTER");
    g_filter = f ? atoi(f) : 0;
    if (g_filter < 0 || g_filter > 2) g_filter = 0;

    g_hwnd = hwnd;
    g_dc = GetDC(hwnd);
    if (!g_dc) {
        fprintf(stderr, "gl: GetDC failed; using GDI\n");
        return 0;
    }

    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    {
        int pf = ChoosePixelFormat(g_dc, &pfd);
        if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) {
            fprintf(stderr, "gl: SetPixelFormat failed (%lu); using GDI\n", GetLastError());
            gl_cleanup();
            return 0;
        }
    }

    dummy = wglCreateContext(g_dc);
    if (!dummy || !wglMakeCurrent(g_dc, dummy)) {
        fprintf(stderr, "gl: wglCreateContext failed; using GDI\n");
        if (dummy) wglDeleteContext(dummy);
        gl_cleanup();
        return 0;
    }

    create_ctx = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create_ctx) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        g_ctx = create_ctx(g_dc, NULL, attribs);
    }
    if (!g_ctx) {
        fprintf(stderr, "gl: OpenGL 3.3 core unavailable; using GDI\n");
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(dummy);
        gl_cleanup();
        return 0;
    }
    wglMakeCurrent(g_dc, g_ctx);
    wglDeleteContext(dummy);

    if (!load_gl() || !make_program()) {
        gl_cleanup();
        return 0;
    }

    g_swap_interval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (g_swap_interval) g_swap_interval(1);

    p_glGenVertexArrays(1, &g_vao);
    p_glBindVertexArray(g_vao);
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            (const void *)(uintptr_t)(2 * sizeof(float)));

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GL_SCREEN_W, GL_SCREEN_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    g_active = 1;
    fprintf(stderr, "gl: OpenGL %s presenter active\n",
            (const char *)glGetString(GL_VERSION));
    return 1;
}

void seto_gl_present_frame(HWND hwnd, const uint32_t *rgba) {
    RECT rc;
    int cw, ch, dw, dh;
    GLint filter;

    if (!g_active) return;
    (void)hwnd;

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GL_SCREEN_W, GL_SCREEN_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    GetClientRect(g_hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    if ((int64_t)cw * 17 > (int64_t)ch * 30) {
        dh = ch;
        dw = (int)((int64_t)ch * 30 / 17);
    } else {
        dw = cw;
        dh = (int)((int64_t)cw * 17 / 30);
    }
    glViewport((cw - dw) / 2, (ch - dh) / 2, dw, dh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    filter = g_filter == 0 ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    p_glUseProgram(g_prog);
    p_glUniform2f(g_u_texel, 1.0f / GL_SCREEN_W, 1.0f / GL_SCREEN_H);
    p_glUniform1i(g_u_mode, g_filter);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SwapBuffers(g_dc);
}

void seto_gl_present_shutdown(HWND hwnd) {
    (void)hwnd;
    gl_cleanup();
}

int seto_gl_present_active(void) {
    return g_active;
}
