/* M0: optional OpenGL 3.3 presenter for the 480x272 software framebuffer.
 *
 * The guest/GE still render into the runtime's framebuffer. This module only
 * uploads that frame as a texture and draws it with a Lanczos2 shader, so the
 * window can be scaled with GPU filtering. It is the context/swapchain base
 * for a future GE backend; the software renderer remains the default. */

#include "gl_present.h"
#include "ge_queue.h"

#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include <stdint.h>
#include <stddef.h>
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
static PFNGLUNIFORM1FPROC p_glUniform1f;
static PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
static PFNGLDELETEVERTEXARRAYSPROC p_glDeleteVertexArrays;
static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
static PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
static PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
static PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;
static PFNGLUNIFORM4FPROC p_glUniform4f;
static PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
static PFNGLDELETERENDERBUFFERSPROC p_glDeleteRenderbuffers;

/* M1.0: GPU thread + SPSC queue + shared FBO. The software renderer is still
 * the display source unless SETO_GE_GPU is set; the test command only changes
 * the triangle colour. */
static HGLRC g_ge_ctx;
static HANDLE g_ge_thread;
static HANDLE g_ge_ready;
static volatile LONG g_ge_quit;
static CRITICAL_SECTION g_ge_lock;
static int g_ge_lock_ready;
static int g_ge_active;
static int g_ge_ok;
static int g_ge_draw_mode;
static GLuint g_ge_prog, g_ge_vao, g_ge_vbo, g_ge_tex, g_ge_fbo, g_ge_depth;
static GLint g_ge_color_loc;
static int g_ge_w, g_ge_h;
static ge_queue g_ge_q;
static int g_ge_queue_ready;
static uint32_t g_ge_rgb = 0xFFFFFFFFu;

static GLuint g_ge_geo_prog, g_ge_geo_vao, g_ge_geo_vbo;
static GLint g_ge_geo_size, g_ge_geo_atest, g_ge_geo_afunc, g_ge_geo_aref;

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
    SETO_GL_LOAD(PFNGLUNIFORM1FPROC, glUniform1f);
    SETO_GL_LOAD(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    SETO_GL_LOAD(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    SETO_GL_LOAD(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
    SETO_GL_LOAD(PFNGLGENBUFFERSPROC, glGenBuffers);
    SETO_GL_LOAD(PFNGLBINDBUFFERPROC, glBindBuffer);
    SETO_GL_LOAD(PFNGLBUFFERDATAPROC, glBufferData);
    SETO_GL_LOAD(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
    SETO_GL_LOAD(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
    SETO_GL_LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    SETO_GL_LOAD(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
    SETO_GL_LOAD(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    SETO_GL_LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    SETO_GL_LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    SETO_GL_LOAD(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    SETO_GL_LOAD(PFNGLUNIFORM4FPROC, glUniform4f);
    SETO_GL_LOAD(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers);
    SETO_GL_LOAD(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer);
    SETO_GL_LOAD(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
    SETO_GL_LOAD(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
    SETO_GL_LOAD(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers);
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

static const char *ge_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *ge_fs =
    "#version 330 core\n"
    "uniform vec4 u_color;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = u_color; }\n";

static const char *ge_geo_vs =
    "#version 330 core\n"
    "layout(location=0) in ivec2 a_pos;\n"
    "layout(location=1) in float a_z;\n"
    "layout(location=2) in vec4 a_color;\n"
    "uniform vec2 u_size;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "  vec2 p = vec2(a_pos) / u_size;\n"
    "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0,\n"
    "                     a_z / 65535.0 * 2.0 - 1.0, 1.0);\n"
    "  v_color = a_color;\n"
    "}\n";

static const char *ge_geo_fs =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "out vec4 o_color;\n"
    "uniform int u_atest;\n"
    "uniform int u_afunc;\n"
    "uniform float u_aref;\n"
    "void main() {\n"
    "  if (u_atest != 0) {\n"
    "    bool pass;\n"
    "    if (u_afunc == 0) pass = false;\n"
    "    else if (u_afunc == 1) pass = true;\n"
    "    else if (u_afunc == 2) pass = abs(v_color.a - u_aref) < (1.0 / 255.0);\n"
    "    else if (u_afunc == 3) pass = abs(v_color.a - u_aref) >= (1.0 / 255.0);\n"
    "    else if (u_afunc == 4) pass = v_color.a < u_aref;\n"
    "    else if (u_afunc == 5) pass = v_color.a <= u_aref;\n"
    "    else if (u_afunc == 6) pass = v_color.a > u_aref;\n"
    "    else pass = v_color.a >= u_aref;\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  o_color = v_color;\n"
    "}\n";

static const float ge_verts[] = {
    -0.6f, -0.5f,
     0.6f, -0.5f,
     0.0f,  0.6f
};

static int ge_make_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, ge_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, ge_fs);
    GLint ok = 0;
    if (!vs || !fs) {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        return 0;
    }
    g_ge_prog = p_glCreateProgram();
    p_glAttachShader(g_ge_prog, vs);
    p_glAttachShader(g_ge_prog, fs);
    p_glLinkProgram(g_ge_prog);
    p_glGetProgramiv(g_ge_prog, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        GLchar log[2048];
        GLsizei len = 0;
        p_glGetProgramInfoLog(g_ge_prog, sizeof log, &len, log);
        fprintf(stderr, "gl: GE program link failed: %.*s\n", (int)len, log);
        p_glDeleteProgram(g_ge_prog);
        g_ge_prog = 0;
        return 0;
    }
    g_ge_color_loc = p_glGetUniformLocation(g_ge_prog, "u_color");
    return 1;
}

static int ge_make_vao(void) {
    p_glGenVertexArrays(1, &g_ge_vao);
    p_glBindVertexArray(g_ge_vao);
    p_glGenBuffers(1, &g_ge_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof ge_verts, ge_verts, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);
    return 1;
}

static int ge_make_geo_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, ge_geo_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, ge_geo_fs);
    GLint ok = 0;
    if (!vs || !fs) {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        return 0;
    }
    g_ge_geo_prog = p_glCreateProgram();
    p_glAttachShader(g_ge_geo_prog, vs);
    p_glAttachShader(g_ge_geo_prog, fs);
    p_glLinkProgram(g_ge_geo_prog);
    p_glGetProgramiv(g_ge_geo_prog, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        GLchar log[2048];
        GLsizei len = 0;
        p_glGetProgramInfoLog(g_ge_geo_prog, sizeof log, &len, log);
        fprintf(stderr, "gl: GE geo program link failed: %.*s\n", (int)len, log);
        p_glDeleteProgram(g_ge_geo_prog);
        g_ge_geo_prog = 0;
        return 0;
    }
    g_ge_geo_size = p_glGetUniformLocation(g_ge_geo_prog, "u_size");
    g_ge_geo_atest = p_glGetUniformLocation(g_ge_geo_prog, "u_atest");
    g_ge_geo_afunc = p_glGetUniformLocation(g_ge_geo_prog, "u_afunc");
    g_ge_geo_aref = p_glGetUniformLocation(g_ge_geo_prog, "u_aref");
    return 1;
}

static int ge_make_geo_vao(void) {
    p_glGenVertexArrays(1, &g_ge_geo_vao);
    p_glBindVertexArray(g_ge_geo_vao);
    p_glGenBuffers(1, &g_ge_geo_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_INT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, x));
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, z));
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, rgba));
    return 1;
}

static GLenum ge_blend_factor(int f) {
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return GL_SRC_COLOR;
    case 3: return GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_DST_COLOR;
    case 5: return GL_ONE_MINUS_DST_COLOR;
    case 6: return GL_SRC_ALPHA;
    case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 8: return GL_DST_ALPHA;
    case 9: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ONE;
    }
}

static GLenum ge_depth_func(int f) {
    switch (f) {
    case 0: return GL_NEVER;
    case 1: return GL_ALWAYS;
    case 2: return GL_EQUAL;
    case 3: return GL_NOTEQUAL;
    case 4: return GL_LESS;
    case 5: return GL_LEQUAL;
    case 6: return GL_GREATER;
    default: return GL_GEQUAL;
    }
}

static void ge_apply_state(const psp_draw_state *st) {
    int x0 = st->sc_x0, y0 = st->sc_y0, x1 = st->sc_x1, y1 = st->sc_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= 480) x1 = 479;
    if (y1 >= 272) y1 = 271;
    if (x1 < x0 || y1 < y0) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, 272 - (y1 + 1), x1 - x0 + 1, y1 - y0 + 1);
    }

    if (st->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(ge_blend_factor(st->blend_src), ge_blend_factor(st->blend_dst));
    } else {
        glDisable(GL_BLEND);
    }

    if (st->cull_enable) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(st->cull_face ? GL_CW : GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }

    if (st->ztest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(ge_depth_func(st->zfunc));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    glDisable(GL_STENCIL_TEST);
}

static void ge_process_draw(const ge_cmd *c) {
    GLenum mode;
    if (!c || c->count <= 0) return;

    if (c->state.clear) {
        const uint32_t col = c->verts[0].rgba;
        const GLfloat r = (GLfloat)(col & 0xFF) / 255.0f;
        const GLfloat g = (GLfloat)((col >> 8) & 0xFF) / 255.0f;
        const GLfloat b = (GLfloat)((col >> 16) & 0xFF) / 255.0f;
        const GLfloat a = (GLfloat)((col >> 24) & 0xFF) / 255.0f;
        GLbitfield bits = GL_COLOR_BUFFER_BIT;
        ge_apply_state(&c->state);
        glClearColor(r, g, b, a);
        if (c->state.clear_depth) {
            glDepthMask(GL_TRUE);
            glClearDepth(0.0);
            bits |= GL_DEPTH_BUFFER_BIT;
        }
        glClear(bits);
        return;
    }

    switch (c->prim) {
    case PSP_PRIM_TRIANGLES:      mode = GL_TRIANGLES; break;
    case PSP_PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PSP_PRIM_TRIANGLE_FAN:   mode = GL_TRIANGLE_FAN; break;
    default: return;
    }

    ge_apply_state(&c->state);
    p_glUseProgram(g_ge_geo_prog);
    p_glUniform2f(g_ge_geo_size, 480.0f, 272.0f);
    p_glUniform1i(g_ge_geo_atest, c->state.atest ? 1 : 0);
    p_glUniform1i(g_ge_geo_afunc, c->state.atest_func);
    p_glUniform1f(g_ge_geo_aref, (GLfloat)c->state.atest_ref / 255.0f);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(c->count * sizeof(psp_vertex)),
                   c->verts, GL_STREAM_DRAW);
    p_glBindVertexArray(g_ge_geo_vao);
    glDrawArrays(mode, 0, c->count);
}

static int ge_make_target(int w, int h) {
    if (g_ge_fbo) {
        p_glDeleteFramebuffers(1, &g_ge_fbo);
        g_ge_fbo = 0;
    }
    if (g_ge_tex) {
        glDeleteTextures(1, &g_ge_tex);
        g_ge_tex = 0;
    }
    glGenTextures(1, &g_ge_tex);
    glBindTexture(GL_TEXTURE_2D, g_ge_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    p_glGenFramebuffers(1, &g_ge_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g_ge_tex, 0);
    if (g_ge_depth) {
        p_glDeleteRenderbuffers(1, &g_ge_depth);
        g_ge_depth = 0;
    }
    p_glGenRenderbuffers(1, &g_ge_depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_ge_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g_ge_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: GE FBO incomplete (%dx%d)\n", w, h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteFramebuffers(1, &g_ge_fbo);
        g_ge_fbo = 0;
        glDeleteTextures(1, &g_ge_tex);
        g_ge_tex = 0;
        p_glDeleteRenderbuffers(1, &g_ge_depth);
        g_ge_depth = 0;
        return 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ge_w = w;
    g_ge_h = h;
    return 1;
}

static void ge_free_gl(void) {
    if (g_ge_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_fbo);
    if (g_ge_depth && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &g_ge_depth);
    if (g_ge_tex) glDeleteTextures(1, &g_ge_tex);
    if (g_ge_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_prog);
    if (g_ge_geo_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_geo_prog);
    if (g_ge_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_vao);
    if (g_ge_geo_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_geo_vao);
    if (g_ge_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_vbo);
    if (g_ge_geo_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_geo_vbo);
    g_ge_fbo = g_ge_depth = g_ge_tex = g_ge_prog = g_ge_vao = g_ge_vbo = 0;
    g_ge_geo_prog = g_ge_geo_vao = g_ge_geo_vbo = 0;
    g_ge_w = g_ge_h = 0;
}

static DWORD WINAPI ge_thread(LPVOID unused) {
    RECT rc;
    int w, h;
    (void)unused;

    if (!wglMakeCurrent(g_dc, g_ge_ctx)) {
        fprintf(stderr, "gl: GE render context make-current failed\n");
        SetEvent(g_ge_ready);
        return 1;
    }

    if (g_ge_draw_mode) {
        if (!ge_make_geo_program() || !ge_make_geo_vao() || !ge_make_target(480, 272)) {
            ge_free_gl();
            wglMakeCurrent(NULL, NULL);
            SetEvent(g_ge_ready);
            return 1;
        }
    } else {
        GetClientRect(g_hwnd, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) { w = 960; h = 544; }
        if (!ge_make_program() || !ge_make_vao() || !ge_make_target(w, h)) {
            ge_free_gl();
            wglMakeCurrent(NULL, NULL);
            SetEvent(g_ge_ready);
            return 1;
        }
    }
    g_ge_ok = 1;
    SetEvent(g_ge_ready);

    while (!g_ge_quit) {
        ge_cmd cmd;
        if (g_ge_draw_mode) {
            EnterCriticalSection(&g_ge_lock);
            while (ge_queue_pop(&g_ge_q, &cmd)) {
                if (cmd.type == GE_CMD_DRAW) ge_process_draw(&cmd);
                else if (cmd.type == GE_CMD_TEST_COLOR) g_ge_rgb = cmd.color;
                /* GE_CMD_TARGET: the M1.1 display target is the fixed 480x272
                 * FBO. GE_CMD_PRESENT: the window timer presents the FBO. */
            }
            glFinish();
            LeaveCriticalSection(&g_ge_lock);
            Sleep(16);
            continue;
        }

        while (ge_queue_pop(&g_ge_q, &cmd)) {
            if (cmd.type == GE_CMD_TEST_COLOR) g_ge_rgb = cmd.color;
        }
        GetClientRect(g_hwnd, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            EnterCriticalSection(&g_ge_lock);
            if (w != g_ge_w || h != g_ge_h) ge_make_target(w, h);
            if (g_ge_fbo && g_ge_tex) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_fbo);
                glViewport(0, 0, w, h);
                glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                p_glUseProgram(g_ge_prog);
                p_glUniform4f(g_ge_color_loc,
                              (GLfloat)((g_ge_rgb >> 16) & 0xFF) / 255.0f,
                              (GLfloat)((g_ge_rgb >> 8) & 0xFF) / 255.0f,
                              (GLfloat)(g_ge_rgb & 0xFF) / 255.0f, 1.0f);
                p_glBindVertexArray(g_ge_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glFinish();
            }
            LeaveCriticalSection(&g_ge_lock);
        }
        Sleep(16);
    }

    ge_free_gl();
    wglMakeCurrent(NULL, NULL);
    return 0;
}

static void ge_stop(void) {
    if (g_ge_thread) {
        InterlockedExchange(&g_ge_quit, 1);
        WaitForSingleObject(g_ge_thread, 2000);
        CloseHandle(g_ge_thread);
        g_ge_thread = NULL;
    }
    if (g_ge_ready) {
        CloseHandle(g_ge_ready);
        g_ge_ready = NULL;
    }
    if (g_ge_lock_ready) {
        DeleteCriticalSection(&g_ge_lock);
        g_ge_lock_ready = 0;
    }
    if (g_ge_queue_ready) {
        ge_queue_destroy(&g_ge_q);
        g_ge_queue_ready = 0;
    }
    if (g_ge_ctx) {
        wglDeleteContext(g_ge_ctx);
        g_ge_ctx = NULL;
    }
    g_ge_active = 0;
    g_ge_ok = 0;
}

static int ge_start(PFNWGLCREATECONTEXTATTRIBSARBPROC create_ctx) {
    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    g_ge_ctx = create_ctx(g_dc, g_ctx, attribs);
    if (!g_ge_ctx) {
        fprintf(stderr, "gl: GE render context unavailable\n");
        return 0;
    }
    InitializeCriticalSection(&g_ge_lock);
    g_ge_lock_ready = 1;
    if (ge_queue_init(&g_ge_q, 64) != 0) {
        fprintf(stderr, "gl: GE queue init failed\n");
        ge_stop();
        return 0;
    }
    g_ge_queue_ready = 1;
    g_ge_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ge_ready) {
        fprintf(stderr, "gl: GE ready event failed\n");
        ge_stop();
        return 0;
    }
    g_ge_quit = 0;
    g_ge_thread = CreateThread(NULL, 0, ge_thread, NULL, 0, NULL);
    if (!g_ge_thread) {
        fprintf(stderr, "gl: GE thread failed\n");
        ge_stop();
        return 0;
    }
    WaitForSingleObject(g_ge_ready, 3000);
    CloseHandle(g_ge_ready);
    g_ge_ready = NULL;
    if (!g_ge_ok) {
        fprintf(stderr, "gl: GE thread init failed\n");
        ge_stop();
        return 0;
    }
    g_ge_active = 1;
    fprintf(stderr, "gl: GE GPU %s backend active (%dx%d)\n",
            g_ge_draw_mode ? "draw" : "test", g_ge_w, g_ge_h);
    return 1;
}

static void gl_cleanup(void) {
    ge_stop();
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

    {
        const char *ge = getenv("SETO_GE_GPU");
        if (ge && ge[0]) {
            g_ge_draw_mode = strcmp(ge, "draw") == 0;
            if (!ge_start(create_ctx))
                fprintf(stderr, "gl: GE backend disabled; software frame stays\n");
        }
    }
    return 1;
}

void seto_gl_present_frame(HWND hwnd, const uint32_t *rgba) {
    RECT rc;
    int cw, ch, dw, dh;
    GLint filter;

    if (!g_active) return;
    (void)hwnd;

    if (g_ge_active) {
        if (g_ge_draw_mode) {
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
            EnterCriticalSection(&g_ge_lock);
            if (g_ge_tex) {
                glBindTexture(GL_TEXTURE_2D, g_ge_tex);
                glViewport((cw - dw) / 2, (ch - dh) / 2, dw, dh);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                p_glUseProgram(g_prog);
                p_glUniform2f(g_u_texel, 1.0f / 480.0f, 1.0f / 272.0f);
                p_glUniform1i(g_u_mode, g_filter);
                p_glBindVertexArray(g_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glFinish();
            }
            LeaveCriticalSection(&g_ge_lock);
            SwapBuffers(g_dc);
            return;
        }

        ge_cmd c;
        c.type = GE_CMD_TEST_COLOR;
        c.color = (uint32_t)GetTickCount();
        ge_queue_push(&g_ge_q, c);

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
        EnterCriticalSection(&g_ge_lock);
        if (g_ge_tex) {
            glBindTexture(GL_TEXTURE_2D, g_ge_tex);
            glViewport((cw - dw) / 2, (ch - dh) / 2, dw, dh);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            p_glUseProgram(g_prog);
            p_glUniform2f(g_u_texel, 1.0f / g_ge_w, 1.0f / g_ge_h);
            p_glUniform1i(g_u_mode, 1);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
        }
        LeaveCriticalSection(&g_ge_lock);
        SwapBuffers(g_dc);
        return;
    }

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

int seto_ge_gpu_draw_mode(void) { return g_ge_draw_mode; }

int seto_ge_gpu_enqueue_target(uint32_t addr, uint32_t stride, int fmt) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return 0;
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_TARGET;
    c.addr = addr;
    c.stride = stride;
    c.fmt = (uint32_t)fmt;
    return ge_queue_push(&g_ge_q, c);
}

int seto_ge_gpu_enqueue_draw(int prim, const psp_vertex *v, int count,
                             const psp_draw_state *st) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return 0;
    if (!v || !st || count <= 0 || count > GE_MAX_VERTS) return 0;
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_DRAW;
    c.prim = (uint32_t)prim;
    c.count = (uint32_t)count;
    c.state = *st;
    memcpy(c.verts, v, (size_t)count * sizeof *v);
    return ge_queue_push(&g_ge_q, c);
}

void seto_ge_gpu_present(void) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return;
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_PRESENT;
    ge_queue_push(&g_ge_q, c);
}

void seto_gl_present_shutdown(HWND hwnd) {
    (void)hwnd;
    gl_cleanup();
}

int seto_gl_present_active(void) {
    return g_active;
}
