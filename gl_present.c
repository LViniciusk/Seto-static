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
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001
#endif
#ifndef GL_ONE_MINUS_CONSTANT_COLOR
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_MIN
#define GL_MIN 0x8007
#endif
#ifndef GL_MAX
#define GL_MAX 0x8008
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif

static HWND g_hwnd;
static HDC g_dc;
static HGLRC g_ctx;
static int g_active;
static int g_filter;
static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint g_u_tex, g_u_texel, g_u_mode, g_u_flip;
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
static PFNGLUNIFORM3IPROC p_glUniform3i;
static PFNGLUNIFORM2FPROC p_glUniform2f;
static PFNGLUNIFORM3FPROC p_glUniform3f;
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
static PFNGLBLENDEQUATIONPROC p_glBlendEquation;
static PFNGLBLENDFUNCSEPARATEPROC p_glBlendFuncSeparate;
static PFNGLBLENDCOLORPROC p_glBlendColor;

/* M1-M2: GPU thread + SPSC queue + shared FBO. */
static HGLRC g_ge_ctx;
static HANDLE g_ge_thread;
static HANDLE g_ge_ready;
static HANDLE g_ge_wake;
static volatile LONG g_ge_quit;
static CRITICAL_SECTION g_ge_lock;
static int g_ge_lock_ready;
static int g_ge_active;
static int g_ge_ok;
static int g_ge_draw_mode;
static GLuint g_ge_prog, g_ge_vao, g_ge_vbo, g_ge_tex, g_ge_front_tex, g_ge_fbo, g_ge_depth;
static GLuint g_ge_tex_cache[GE_TEX_CACHE_SLOTS];
static GLint g_ge_color_loc;
static int g_ge_w, g_ge_h;
static int g_ge_have_front;
static int g_ge_passthrough_sw;
static ge_queue g_ge_q;
static int g_ge_queue_ready;
static uint32_t g_ge_rgb = 0xFFFFFFFFu;

static GLuint g_ge_geo_prog, g_ge_geo_vao, g_ge_geo_vbo;
static GLint g_ge_geo_size, g_ge_geo_half_px, g_ge_geo_clear;
static GLint g_ge_geo_atest, g_ge_geo_afunc, g_ge_geo_aref, g_ge_geo_amask;
static GLint g_ge_geo_ctest, g_ge_geo_cfunc, g_ge_geo_cref, g_ge_geo_cmask;
static GLint g_ge_geo_tex_on, g_ge_geo_tex_func, g_ge_geo_tex_alpha;
static GLint g_ge_geo_tex_double, g_ge_geo_tex_env, g_ge_geo_tex_size;
static GLint g_ge_geo_tex_sampler;
static GLint g_ge_geo_zrange, g_ge_geo_zmin, g_ge_geo_zmax;
static GLint g_ge_geo_fog_on, g_ge_geo_fog_color, g_ge_geo_lmode;
static GLint g_ge_geo_zclamp_vert;
static GLint g_ge_geo_src_premul, g_ge_geo_sfix_rgb, g_ge_geo_alpha_2x;

static const char *vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform int u_flip;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_uv.x, u_flip != 0 ? 1.0 - a_uv.y : a_uv.y);\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

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
    "  if (u_mode < 2) { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); return; }\n"
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
    "  o_color = vec4((sum / wsum).rgb, 1.0);\n"
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
            HMODULE glmod = GetModuleHandleA("opengl32.dll");                 \
            if (glmod) p_##name = (type)GetProcAddress(glmod, #name);         \
        }                                                                     \
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
    SETO_GL_LOAD(PFNGLUNIFORM3IPROC, glUniform3i);
    SETO_GL_LOAD(PFNGLUNIFORM2FPROC, glUniform2f);
    SETO_GL_LOAD(PFNGLUNIFORM3FPROC, glUniform3f);
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
    SETO_GL_LOAD(PFNGLBLENDEQUATIONPROC, glBlendEquation);
    SETO_GL_LOAD(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate);
    SETO_GL_LOAD(PFNGLBLENDCOLORPROC, glBlendColor);
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
    g_u_flip = p_glGetUniformLocation(g_prog, "u_flip");
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
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in float a_z;\n"
    "layout(location=2) in vec4 a_color;\n"
    "layout(location=3) in vec2 a_uv;\n"
    "layout(location=4) in vec4 a_spec;\n"
    "layout(location=5) in float a_fog;\n"
    "uniform vec2 u_size;\n"
    "uniform vec2 u_tex_size;\n"
    "uniform float u_half_px;\n"
    "uniform int u_zclamp_vert;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "out float v_z;\n"
    "out vec3 v_spec;\n"
    "out float v_fog;\n"
    "void main() {\n"
    "  vec2 p = (a_pos + vec2(u_half_px)) / u_size;\n"
    "  float z = u_zclamp_vert != 0 ? clamp(floor(a_z + 0.5), 0.0, 65535.0) : a_z;\n"
    "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0,\n"
    "                     1.0 - z / 65535.0 * 2.0, 1.0);\n"
    "  v_color = a_color;\n"
    "  v_uv = a_uv / u_tex_size;\n"
    "  v_z = a_z;\n"
    "  v_spec = a_spec.rgb;\n"
    "  v_fog = a_fog;\n"
    "}\n";

static const char *ge_geo_fs =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "in float v_z;\n"
    "in vec3 v_spec;\n"
    "in float v_fog;\n"
    "out vec4 o_color;\n"
    "uniform int u_clear;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_tex_on;\n"
    "uniform int u_tex_func;\n"
    "uniform int u_tex_alpha;\n"
    "uniform int u_tex_double;\n"
    "uniform vec4 u_tex_env;\n"
    "uniform int u_zrange;\n"
    "uniform int u_zmin;\n"
    "uniform int u_zmax;\n"
    "uniform int u_lmode;\n"
    "uniform int u_fog_on;\n"
    "uniform vec3 u_fog_color;\n"
    "uniform int u_ctest;\n"
    "uniform int u_cfunc;\n"
    "uniform ivec3 u_cref;\n"
    "uniform ivec3 u_cmask;\n"
    "uniform int u_atest;\n"
    "uniform int u_afunc;\n"
    "uniform int u_aref;\n"
    "uniform int u_amask;\n"
    "uniform int u_src_premul;\n"
    "uniform vec3 u_sfix_rgb;\n"
    "uniform int u_alpha_2x;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_clear != 0) {\n"
    "    o_color = c;\n"
    "    return;\n"
    "  }\n"
    "  if (u_zrange != 0) {\n"
    "    int iz = clamp(int(v_z + 0.5), 0, 65535);\n"
    "    if (iz < u_zmin || iz > u_zmax) discard;\n"
    "  }\n"
    "  if (u_tex_on != 0) {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    "    vec3 rgb;\n"
    "    float a;\n"
    "    if (u_tex_func == 0) {\n"
    "      rgb = t.rgb * c.rgb;\n"
    "      a = u_tex_alpha != 0 ? t.a * c.a : c.a;\n"
    "    } else if (u_tex_func == 1) {\n"
    "      rgb = u_tex_alpha != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb;\n"
    "      a = c.a;\n"
    "    } else if (u_tex_func == 2) {\n"
    "      rgb = mix(c.rgb, u_tex_env.rgb, t.rgb);\n"
    "      a = u_tex_alpha != 0 ? t.a * c.a : c.a;\n"
    "    } else if (u_tex_func == 3) {\n"
    "      rgb = t.rgb;\n"
    "      a = u_tex_alpha != 0 ? t.a : c.a;\n"
    "    } else {\n"
    "      rgb = t.rgb + c.rgb;\n"
    "      a = u_tex_alpha != 0 ? t.a * c.a : c.a;\n"
    "    }\n"
    "    if (u_tex_double != 0) rgb *= 2.0;\n"
    "    c = vec4(clamp(rgb, 0.0, 1.0), a);\n"
    "  }\n"
    "  if (u_lmode != 0) {\n"
    "    float sscale = (u_tex_on != 0 && u_tex_double != 0) ? 2.0 : 1.0;\n"
    "    c.rgb = clamp(c.rgb + v_spec * sscale, 0.0, 1.0);\n"
    "  }\n"
    "  if (u_fog_on != 0) {\n"
    "    c.rgb = mix(u_fog_color, c.rgb, clamp(v_fog, 0.0, 1.0));\n"
    "  }\n"
    "  if (u_ctest != 0) {\n"
    "    ivec3 ic = ivec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5) & u_cmask;\n"
    "    ivec3 ir = u_cref & u_cmask;\n"
    "    bool cpass;\n"
    "    if (u_cfunc == 0) cpass = false;\n"
    "    else if (u_cfunc == 1) cpass = true;\n"
    "    else if (u_cfunc == 2) cpass = all(equal(ic, ir));\n"
    "    else cpass = any(notEqual(ic, ir));\n"
    "    if (!cpass) discard;\n"
    "  }\n"
    "  if (u_atest != 0) {\n"
    "    int ia = int(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5) & u_amask;\n"
    "    int ir = u_aref & u_amask;\n"
    "    bool pass;\n"
    "    if (u_afunc == 0) pass = false;\n"
    "    else if (u_afunc == 1) pass = true;\n"
    "    else if (u_afunc == 2) pass = (ia == ir);\n"
    "    else if (u_afunc == 3) pass = (ia != ir);\n"
    "    else if (u_afunc == 4) pass = (ia < ir);\n"
    "    else if (u_afunc == 5) pass = (ia <= ir);\n"
    "    else if (u_afunc == 6) pass = (ia > ir);\n"
    "    else pass = (ia >= ir);\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  if (u_src_premul == 1) c.rgb *= clamp(c.a, 0.0, 1.0);\n"
    "  else if (u_src_premul == 2) c.rgb *= (1.0 - clamp(c.a, 0.0, 1.0));\n"
    "  else if (u_src_premul == 3) c.rgb *= clamp(c.a * 2.0, 0.0, 1.0);\n"
    "  else if (u_src_premul == 4) c.rgb *= (1.0 - clamp(c.a * 2.0, 0.0, 1.0));\n"
    "  else if (u_src_premul == 5) c.rgb *= u_sfix_rgb;\n"
    "  if (u_alpha_2x != 0) c.a = clamp(c.a * 2.0, 0.0, 1.0);\n"
    "  o_color = c;\n"
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
    g_ge_geo_half_px = p_glGetUniformLocation(g_ge_geo_prog, "u_half_px");
    g_ge_geo_clear = p_glGetUniformLocation(g_ge_geo_prog, "u_clear");
    g_ge_geo_atest = p_glGetUniformLocation(g_ge_geo_prog, "u_atest");
    g_ge_geo_afunc = p_glGetUniformLocation(g_ge_geo_prog, "u_afunc");
    g_ge_geo_aref = p_glGetUniformLocation(g_ge_geo_prog, "u_aref");
    g_ge_geo_amask = p_glGetUniformLocation(g_ge_geo_prog, "u_amask");
    g_ge_geo_ctest = p_glGetUniformLocation(g_ge_geo_prog, "u_ctest");
    g_ge_geo_cfunc = p_glGetUniformLocation(g_ge_geo_prog, "u_cfunc");
    g_ge_geo_cref = p_glGetUniformLocation(g_ge_geo_prog, "u_cref");
    g_ge_geo_cmask = p_glGetUniformLocation(g_ge_geo_prog, "u_cmask");
    g_ge_geo_tex_on = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_on");
    g_ge_geo_tex_func = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_func");
    g_ge_geo_tex_alpha = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_alpha");
    g_ge_geo_tex_double = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_double");
    g_ge_geo_tex_env = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_env");
    g_ge_geo_tex_size = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_size");
    g_ge_geo_tex_sampler = p_glGetUniformLocation(g_ge_geo_prog, "u_tex");
    g_ge_geo_zrange = p_glGetUniformLocation(g_ge_geo_prog, "u_zrange");
    g_ge_geo_zmin = p_glGetUniformLocation(g_ge_geo_prog, "u_zmin");
    g_ge_geo_zmax = p_glGetUniformLocation(g_ge_geo_prog, "u_zmax");
    g_ge_geo_fog_on = p_glGetUniformLocation(g_ge_geo_prog, "u_fog_on");
    g_ge_geo_fog_color = p_glGetUniformLocation(g_ge_geo_prog, "u_fog_color");
    g_ge_geo_lmode = p_glGetUniformLocation(g_ge_geo_prog, "u_lmode");
    g_ge_geo_zclamp_vert = p_glGetUniformLocation(g_ge_geo_prog, "u_zclamp_vert");
    g_ge_geo_src_premul = p_glGetUniformLocation(g_ge_geo_prog, "u_src_premul");
    g_ge_geo_sfix_rgb = p_glGetUniformLocation(g_ge_geo_prog, "u_sfix_rgb");
    g_ge_geo_alpha_2x = p_glGetUniformLocation(g_ge_geo_prog, "u_alpha_2x");
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
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, u));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, spec));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, fog));
    return 1;
}

/* PSP blend factor codes (render.c:393-408):
 * 0/1 = other_color / 1-other_color, 2/3 = src_alpha / 1-src_alpha,
 * 4/5 = dst_alpha / 1-dst_alpha, 10+ = SFIX / DFIX. */
static GLenum ge_blend_factor(int code, int is_src, uint32_t fix, int inv_const) {
    switch (code) {
    case 0: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: return GL_SRC_ALPHA;
    case 3: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: return GL_DST_ALPHA;
    case 5: return GL_ONE_MINUS_DST_ALPHA;
    default: {
        const uint32_t rgb = fix & 0x00FFFFFFu;
        if (rgb == 0u) return GL_ZERO;
        if (rgb == 0x00FFFFFFu) return GL_ONE;
        return inv_const ? GL_ONE_MINUS_CONSTANT_COLOR : GL_CONSTANT_COLOR;
    }
    }
}

static GLenum ge_blend_op(int op) {
    switch (op) {
    case 1:  return GL_FUNC_SUBTRACT;
    case 2:  return GL_FUNC_REVERSE_SUBTRACT;
    case 3:  return GL_MIN;
    case 4:  return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum ge_stencil_func(int f) {
    switch (f) {
    case 0:  return GL_NEVER;
    case 1:  return GL_ALWAYS;
    case 2:  return GL_EQUAL;
    case 3:  return GL_NOTEQUAL;
    case 4:  return GL_LESS;
    case 5:  return GL_LEQUAL;
    case 6:  return GL_GREATER;
    default: return GL_GEQUAL;
    }
}

static GLenum ge_stencil_op(int op) {
    switch (op) {
    case 1:  return GL_ZERO;
    case 2:  return GL_REPLACE;
    case 3:  return GL_INVERT;
    case 4:  return GL_INCR;
    case 5:  return GL_DECR;
    default: return GL_KEEP;
    }
}

/* GE depth is 0 = far, 65535 = near; the shader maps it to window depth
 * 1 - z/65535, so the GE's comparisons flip. */
static GLenum ge_depth_func(int f) {
    switch (f) {
    case 0: return GL_NEVER;
    case 1: return GL_ALWAYS;
    case 2: return GL_EQUAL;
    case 3: return GL_NOTEQUAL;
    case 4: return GL_GREATER;
    case 5: return GL_GEQUAL;
    case 6: return GL_LESS;
    default: return GL_LEQUAL;
    }
}

static void ge_plan_blend(const psp_draw_state *st, GLenum *sfactor, GLenum *dfactor,
                          int *use_const, uint32_t *const_rgb,
                          int *src_premul, int *alpha_2x) {
    *sfactor = GL_ONE;
    *dfactor = GL_ZERO;
    *use_const = 0;
    *const_rgb = 0u;
    *src_premul = 0;
    *alpha_2x = 0;
    if (!st->blend || st->clear) return;
    if (st->blend_op >= 3) {
        *sfactor = GL_ONE;
        *dfactor = GL_ONE;
        return;
    }
    const uint32_t sf = st->blend_sfix & 0x00FFFFFFu;
    const uint32_t df = st->blend_dfix & 0x00FFFFFFu;
    const int s_const = st->blend_src >= 10 && sf != 0u && sf != 0x00FFFFFFu;
    const int d_const = st->blend_dst >= 10 && df != 0u && df != 0x00FFFFFFu;
    const int s_2x = (st->blend_src == 6 || st->blend_src == 7);
    const int d_2x = (st->blend_dst == 6 || st->blend_dst == 7);
    const int s_1x = (st->blend_src == 2 || st->blend_src == 3);
    const int d_1x = (st->blend_dst == 2 || st->blend_dst == 3);

    if (s_const && d_const && sf != df && ((sf ^ df) & 0x00FFFFFFu) != 0x00FFFFFFu) {
        *src_premul = 5;
        *sfactor = GL_ONE;
        *use_const = 1;
        *const_rgb = df;
        *dfactor = GL_CONSTANT_COLOR;
        return;
    }
    if ((s_2x && d_1x) || (s_1x && d_2x)) {
        *src_premul = (st->blend_src == 2) ? 1 :
                      (st->blend_src == 3) ? 2 :
                      (st->blend_src == 6) ? 3 : 4;
        *sfactor = GL_ONE;
        *alpha_2x = d_2x ? 1 : 0;
        *dfactor = (st->blend_dst == 2 || st->blend_dst == 6)
                   ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
        return;
    }
    *alpha_2x = (s_2x || d_2x) ? 1 : 0;
    if (s_const || d_const) {
        *use_const = 1;
        *const_rgb = s_const ? sf : df;
    }
    const int inv_df = (s_const && d_const && df != sf) ? 1 : 0;
    const int sc = (st->blend_src == 6) ? 2 : (st->blend_src == 7) ? 3 : st->blend_src;
    const int dc = (st->blend_dst == 6) ? 2 : (st->blend_dst == 7) ? 3 : st->blend_dst;
    *sfactor = ge_blend_factor(sc, 1, sf, 0);
    *dfactor = ge_blend_factor(dc, 0, df, inv_df);
}

static void ge_apply_state(const psp_draw_state *st, uint32_t clear_rgba) {
    glEnable(GL_DEPTH_CLAMP);
    int x0 = st->sc_x0, y0 = st->sc_y0, x1 = st->sc_x1, y1 = st->sc_y1;
    if (st->reg_enable) {
        if (st->reg_x0 > x0) x0 = st->reg_x0;
        if (st->reg_y0 > y0) y0 = st->reg_y0;
        if (st->reg_x1 < x1) x1 = st->reg_x1;
        if (st->reg_y1 < y1) y1 = st->reg_y1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= 480) x1 = 479;
    if (y1 >= 272) y1 = 271;
    glEnable(GL_SCISSOR_TEST);
    if (x1 < x0 || y1 < y0) {
        glScissor(0, 0, 0, 0);
    } else {
        glScissor(x0, 272 - (y1 + 1), x1 - x0 + 1, y1 - y0 + 1);
    }

    const GLboolean rm = (st->write_mask & 0x000000FFu) == 0u ? GL_TRUE : GL_FALSE;
    const GLboolean gm = (st->write_mask & 0x0000FF00u) == 0u ? GL_TRUE : GL_FALSE;
    const GLboolean bm = (st->write_mask & 0x00FF0000u) == 0u ? GL_TRUE : GL_FALSE;
    const GLuint smask = (~st->write_mask >> 24) & 0xFFu;

    if (st->clear) {
        /* CLEAR writes vertex colour/alpha/depth directly. Enable depth/stencil
         * tests with ALWAYS so GL executes the buffer writes. */
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        if (st->clear_depth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glDepthMask(GL_TRUE);
        } else {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
        glColorMask(st->clear_color ? rm : GL_FALSE,
                    st->clear_color ? gm : GL_FALSE,
                    st->clear_color ? bm : GL_FALSE,
                    (st->clear_alpha && (st->write_mask & 0xFF000000u) == 0u) ? GL_TRUE : GL_FALSE);
        if (st->clear_alpha && smask) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, (GLint)((clear_rgba >> 24) & 0xFFu), 0xFFu);
            glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
            glStencilMask(smask);
        } else {
            glDisable(GL_STENCIL_TEST);
            glStencilMask(0u);
        }
        return;
    }

    glColorMask(rm, gm, bm, GL_FALSE);
    if (st->blend) {
        GLenum sfactor, dfactor;
        int use_const, src_premul, alpha_2x;
        uint32_t const_rgb;
        ge_plan_blend(st, &sfactor, &dfactor, &use_const, &const_rgb,
                      &src_premul, &alpha_2x);
        glEnable(GL_BLEND);
        p_glBlendEquation(ge_blend_op(st->blend_op));
        if (use_const) {
            p_glBlendColor((GLfloat)(const_rgb & 0xFFu) / 255.0f,
                           (GLfloat)((const_rgb >> 8) & 0xFFu) / 255.0f,
                           (GLfloat)((const_rgb >> 16) & 0xFFu) / 255.0f, 1.0f);
        }
        p_glBlendFuncSeparate(sfactor, dfactor, GL_ZERO, GL_ONE);
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
        glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    if (st->stencil) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(ge_stencil_func(st->stencil_func),
                      st->stencil_ref & 0xFF,
                      (GLuint)(st->stencil_mask & 0xFF));
        glStencilOp(ge_stencil_op(st->stencil_fail),
                    ge_stencil_op(st->stencil_zfail),
                    ge_stencil_op(st->stencil_zpass));
        glStencilMask(smask);
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0u);
    }
}

static void ge_process_draw(const ge_cmd *c) {
    GLenum mode;
    GLfloat half_px = 0.0f;
    if (!c || c->count <= 0) {
        if (c && c->tex) free(c->tex);
        return;
    }

    switch (c->prim) {
    case PSP_PRIM_POINTS:         mode = GL_POINTS;         half_px = 0.5f; break;
    case PSP_PRIM_LINES:          mode = GL_LINES;          half_px = 0.5f; break;
    case PSP_PRIM_LINE_STRIP:     mode = GL_LINE_STRIP;     half_px = 0.5f; break;
    case PSP_PRIM_TRIANGLES:      mode = GL_TRIANGLES;      break;
    case PSP_PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PSP_PRIM_TRIANGLE_FAN:   mode = GL_TRIANGLE_FAN;   break;
    default: free(c->tex); return;
    }

    ge_apply_state(&c->state, c->verts[0].rgba);
    if (getenv("SETO_GE_DEBUG_DRAW")) {
        static int nlogged;
        if (nlogged < 6) {
            nlogged++;
            fprintf(stderr, "gpu: draw prim=%u n=%u clear=%d cc=%d ca=%d cd=%d "
                    "sc=%d,%d-%d,%d reg=%d/%d v0=%d,%d z=%.0f col=%08X slot=%d err=%u\n",
                    c->prim, c->count, c->state.clear, c->state.clear_color,
                    c->state.clear_alpha, c->state.clear_depth,
                    c->state.sc_x0, c->state.sc_y0, c->state.sc_x1, c->state.sc_y1,
                    c->state.reg_enable, c->state.reg_enable ? c->state.reg_x1 : 0,
                    c->verts[0].x, c->verts[0].y, c->verts[0].z,
                    c->verts[0].rgba, c->tex_slot, glGetError());
        }
    }
    p_glUseProgram(g_ge_geo_prog);
    p_glUniform2f(g_ge_geo_size, 480.0f, 272.0f);
    p_glUniform1f(g_ge_geo_half_px, half_px);
    p_glUniform1i(g_ge_geo_clear, c->state.clear ? 1 : 0);

    GLuint tex = 0;
    if (c->tex_slot >= 0 && c->tex_slot < GE_TEX_CACHE_SLOTS &&
        c->state.tex_enable && !c->state.clear &&
        c->state.tex_w > 0 && c->state.tex_h > 0) {
        if (!g_ge_tex_cache[c->tex_slot]) glGenTextures(1, &g_ge_tex_cache[c->tex_slot]);
        tex = g_ge_tex_cache[c->tex_slot];
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        (c->state.tex_filter & 0x101) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        (c->state.tex_filter & 0x101) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        c->state.tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        c->state.tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        if (c->tex) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->state.tex_w, c->state.tex_h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, c->tex);
        }
    }
    free(c->tex);

    p_glUniform2f(g_ge_geo_tex_size, tex ? (GLfloat)c->state.tex_w : 1.0f,
                  tex ? (GLfloat)c->state.tex_h : 1.0f);
    p_glUniform1i(g_ge_geo_tex_on, tex ? 1 : 0);
    p_glUniform1i(g_ge_geo_tex_sampler, 0);
    if (tex) {
        p_glUniform1i(g_ge_geo_tex_func, c->state.tex_func);
        p_glUniform1i(g_ge_geo_tex_alpha, c->state.tex_alpha ? 1 : 0);
        p_glUniform1i(g_ge_geo_tex_double, c->state.tex_double ? 1 : 0);
        const uint32_t e = c->state.tex_env;
        p_glUniform4f(g_ge_geo_tex_env, (GLfloat)(e & 0xFFu) / 255.0f,
                      (GLfloat)((e >> 8) & 0xFFu) / 255.0f,
                      (GLfloat)((e >> 16) & 0xFFu) / 255.0f, 1.0f);
    }

    p_glUniform1i(g_ge_geo_atest, c->state.atest ? 1 : 0);
    p_glUniform1i(g_ge_geo_afunc, c->state.atest_func);
    p_glUniform1i(g_ge_geo_aref, c->state.atest_ref & 0xFF);
    p_glUniform1i(g_ge_geo_amask, c->state.atest_mask & 0xFF);

    p_glUniform1i(g_ge_geo_ctest, c->state.ctest ? 1 : 0);
    p_glUniform1i(g_ge_geo_cfunc, c->state.ctest_func);
    {
        const uint32_t cr = c->state.ctest_ref;
        const uint32_t cm = c->state.ctest_mask;
        p_glUniform3i(g_ge_geo_cref, (GLint)(cr & 0xFFu),
                      (GLint)((cr >> 8) & 0xFFu),
                      (GLint)((cr >> 16) & 0xFFu));
        p_glUniform3i(g_ge_geo_cmask, (GLint)(cm & 0xFFu),
                      (GLint)((cm >> 8) & 0xFFu),
                      (GLint)((cm >> 16) & 0xFFu));
    }
    p_glUniform1i(g_ge_geo_lmode, c->state.lmode ? 1 : 0);
    {
        const int fog_on = (c->state.fog_enable && !c->state.through) ? 1 : 0;
        const uint32_t fc = c->state.fog_color;
        p_glUniform1i(g_ge_geo_fog_on, fog_on);
        p_glUniform3f(g_ge_geo_fog_color, (GLfloat)(fc & 0xFFu) / 255.0f,
                      (GLfloat)((fc >> 8) & 0xFFu) / 255.0f,
                      (GLfloat)((fc >> 16) & 0xFFu) / 255.0f);
    }
    p_glUniform1i(g_ge_geo_zrange,
                  (c->state.zrange_enable && !c->state.through) ? 1 : 0);
    p_glUniform1i(g_ge_geo_zmin, c->state.zmin);
    p_glUniform1i(g_ge_geo_zmax, c->state.zmax);
    p_glUniform1i(g_ge_geo_zclamp_vert,
                  (c->state.clear || c->state.through || !c->state.ztest) ? 1 : 0);
    {
        GLenum sfactor, dfactor;
        int use_const, src_premul, alpha_2x;
        uint32_t const_rgb;
        const uint32_t sf = c->state.blend_sfix;
        ge_plan_blend(&c->state, &sfactor, &dfactor, &use_const, &const_rgb,
                      &src_premul, &alpha_2x);
        p_glUniform1i(g_ge_geo_src_premul, src_premul);
        p_glUniform3f(g_ge_geo_sfix_rgb, (GLfloat)(sf & 0xFFu) / 255.0f,
                      (GLfloat)((sf >> 8) & 0xFFu) / 255.0f,
                      (GLfloat)((sf >> 16) & 0xFFu) / 255.0f);
        p_glUniform1i(g_ge_geo_alpha_2x, alpha_2x);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(c->count * sizeof(psp_vertex)),
                   c->verts, GL_STREAM_DRAW);
    p_glBindVertexArray(g_ge_geo_vao);
    glDrawArrays(mode, 0, c->count);
    if (getenv("SETO_GE_DEBUG_DRAW") && c->count > 0) {
        static int npost;
        if (npost < 4) {
            unsigned char px[4] = {0, 0, 0, 0};
            npost++;
            glReadPixels(240, 136, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "gpu: post draw prim=%u n=%u tex=%dx%d center=%02X%02X%02X%02X\n",
                    c->prim, c->count, c->state.tex_w, c->state.tex_h,
                    px[0], px[1], px[2], px[3]);
        }
    }
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
    if (g_ge_front_tex) {
        glDeleteTextures(1, &g_ge_front_tex);
        g_ge_front_tex = 0;
    }
    glGenTextures(1, &g_ge_tex);
    glBindTexture(GL_TEXTURE_2D, g_ge_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    if (g_ge_draw_mode) {
        glGenTextures(1, &g_ge_front_tex);
        glBindTexture(GL_TEXTURE_2D, g_ge_front_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

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
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, g_ge_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: GE FBO incomplete (%dx%d)\n", w, h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteFramebuffers(1, &g_ge_fbo);
        g_ge_fbo = 0;
        glDeleteTextures(1, &g_ge_tex);
        g_ge_tex = 0;
        if (g_ge_front_tex) {
            glDeleteTextures(1, &g_ge_front_tex);
            g_ge_front_tex = 0;
        }
        p_glDeleteRenderbuffers(1, &g_ge_depth);
        g_ge_depth = 0;
        return 0;
    }
    /* Guest depth starts zeroed, which is the GE's far plane. */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glViewport(0, 0, w, h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ge_w = w;
    g_ge_h = h;
    return 1;
}

static void ge_free_gl(void) {
    if (g_ge_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_fbo);
    if (g_ge_depth && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &g_ge_depth);
    if (g_ge_tex) glDeleteTextures(1, &g_ge_tex);
    if (g_ge_front_tex) glDeleteTextures(1, &g_ge_front_tex);
    glDeleteTextures(GE_TEX_CACHE_SLOTS, g_ge_tex_cache);
    memset(g_ge_tex_cache, 0, sizeof g_ge_tex_cache);
    if (g_ge_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_prog);
    if (g_ge_geo_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_geo_prog);
    if (g_ge_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_vao);
    if (g_ge_geo_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_geo_vao);
    if (g_ge_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_vbo);
    if (g_ge_geo_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_geo_vbo);
    g_ge_fbo = g_ge_depth = g_ge_tex = g_ge_front_tex = g_ge_prog = g_ge_vao = g_ge_vbo = 0;
    g_ge_geo_prog = g_ge_geo_vao = g_ge_geo_vbo = 0;
    g_ge_w = g_ge_h = 0;
    g_ge_have_front = 0;
    g_ge_passthrough_sw = 0;
}

static void ge_capture_fbo_ppm(uint32_t capture_id) {
    static uint8_t px[480 * 272 * 4];
    static uint8_t rgb[480 * 272 * 3];
    const char *dir = getenv("SETO_GL_SHOT_DIR");
    if (!dir || !dir[0] || !capture_id) return;
    glReadPixels(0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < 272; y++) {
        const uint8_t *src = px + (271 - y) * 480 * 4;
        uint8_t *dst = rgb + y * 480 * 3;
        for (int x = 0; x < 480; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    char path[260];
    snprintf(path, sizeof path, "%s/gl-%05u.ppm", dir, capture_id);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n480 272\n255\n");
        fwrite(rgb, 1, sizeof rgb, f);
        fclose(f);
    }
}

static void ge_publish_front(void) {
    if (!g_ge_fbo || !g_ge_front_tex) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_fbo);
    EnterCriticalSection(&g_ge_lock);
    glBindTexture(GL_TEXTURE_2D, g_ge_front_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 480, 272);
    glFinish();
    g_ge_have_front = 1;
    g_ge_passthrough_sw = 0;
    LeaveCriticalSection(&g_ge_lock);
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

    DWORD last_pub = GetTickCount();
    int draws_since_present = 0;
    while (!g_ge_quit) {
        ge_cmd cmd;
        if (g_ge_draw_mode) {
            if (g_ge_fbo) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_fbo);
                glViewport(0, 0, 480, 272);
            }
            int popped = 0;
            while (ge_queue_pop(&g_ge_q, &cmd)) {
                if (popped < 4 && getenv("SETO_GE_DEBUG_FBO")) {
                    fprintf(stderr, "gl: pop type=%u prim=%u count=%u v0=%d,%d "
                            "col=%08X slot=%d tex=%p\n", cmd.type, cmd.prim, cmd.count,
                            cmd.verts[0].x, cmd.verts[0].y, cmd.verts[0].rgba,
                            cmd.tex_slot, (void *)cmd.tex);
                }
                popped++;
                if (cmd.type == GE_CMD_DRAW) {
                    ge_process_draw(&cmd);
                    draws_since_present++;
                } else if (cmd.type == GE_CMD_PRESENT) {
                    if (cmd.color) ge_capture_fbo_ppm(cmd.color);
                    if (draws_since_present > 0) {
                        ge_publish_front();
                        draws_since_present = 0;
                        last_pub = GetTickCount();
                    } else {
                        EnterCriticalSection(&g_ge_lock);
                        g_ge_passthrough_sw = 1;
                        LeaveCriticalSection(&g_ge_lock);
                    }
                } else if (cmd.type == GE_CMD_TEST_COLOR) {
                    g_ge_rgb = cmd.color;
                }
            }
            if (draws_since_present > 0 && GetTickCount() - last_pub >= 100u) {
                ge_publish_front();
                draws_since_present = 0;
                last_pub = GetTickCount();
            }
            if (getenv("SETO_GE_DEBUG_FBO")) {
                static int fbo_log;
                static unsigned char px[480 * 272 * 4];
                if ((fbo_log++ % 60) == 0) {
                    int nz = 0, maxv = 0;
                    glReadPixels(0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    for (int i = 0; i < 480 * 272 * 4; i++) {
                        if (px[i]) { nz++; if (px[i] > maxv) maxv = px[i]; }
                    }
                    fprintf(stderr, "gl: FBO=%u tex=%u status=%u popped=%d err=%u "
                            "nonzero=%d max=%02X center=%02X%02X%02X%02X\n",
                            g_ge_fbo, g_ge_tex,
                            p_glCheckFramebufferStatus(GL_FRAMEBUFFER), popped,
                            glGetError(), nz, maxv,
                            px[(136 * 480 + 240) * 4 + 0],
                            px[(136 * 480 + 240) * 4 + 1],
                            px[(136 * 480 + 240) * 4 + 2],
                            px[(136 * 480 + 240) * 4 + 3]);
                }
            }
            if (g_ge_wake) WaitForSingleObject(g_ge_wake, 4);
            else Sleep(4);
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
        if (g_ge_wake) SetEvent(g_ge_wake);
        WaitForSingleObject(g_ge_thread, 2000);
        CloseHandle(g_ge_thread);
        g_ge_thread = NULL;
    }
    if (g_ge_wake) {
        CloseHandle(g_ge_wake);
        g_ge_wake = NULL;
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
    if (ge_queue_init(&g_ge_q, 512) != 0) {
        fprintf(stderr, "gl: GE queue init failed\n");
        ge_stop();
        return 0;
    }
    g_ge_queue_ready = 1;
    g_ge_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_ge_wake) {
        fprintf(stderr, "gl: GE wake event failed\n");
        ge_stop();
        return 0;
    }
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
            int use_ge = 0;
            EnterCriticalSection(&g_ge_lock);
            use_ge = (g_ge_front_tex && g_ge_have_front && !g_ge_passthrough_sw);
            if (use_ge) {
                GetClientRect(g_hwnd, &rc);
                cw = rc.right - rc.left;
                ch = rc.bottom - rc.top;
                if (cw > 0 && ch > 0) {
                    if ((int64_t)cw * 17 > (int64_t)ch * 30) {
                        dh = ch;
                        dw = (int)((int64_t)ch * 30 / 17);
                    } else {
                        dw = cw;
                        dh = (int)((int64_t)cw * 17 / 30);
                    }
                    glBindTexture(GL_TEXTURE_2D, g_ge_front_tex);
                    filter = g_filter == 0 ? GL_NEAREST : GL_LINEAR;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    glViewport((cw - dw) / 2, (ch - dh) / 2, dw, dh);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    p_glUseProgram(g_prog);
                    p_glUniform2f(g_u_texel, 1.0f / 480.0f, 1.0f / 272.0f);
                    p_glUniform1i(g_u_mode, g_filter);
                    p_glUniform1i(g_u_flip, 1);
                    p_glBindVertexArray(g_vao);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glFinish();
                }
            }
            LeaveCriticalSection(&g_ge_lock);
            if (use_ge) {
                if (cw > 0 && ch > 0) SwapBuffers(g_dc);
                return;
            }
            /* Passthrough software frame (before first GE frame or during movies). */
            goto present_sw_frame;
        }

        ge_cmd c;
        c.type = GE_CMD_TEST_COLOR;
        c.color = (uint32_t)GetTickCount();
        if (ge_queue_push(&g_ge_q, c) && g_ge_wake) SetEvent(g_ge_wake);

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
            p_glUniform1i(g_u_flip, 1);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
        }
        LeaveCriticalSection(&g_ge_lock);
        SwapBuffers(g_dc);
        return;
    }

    present_sw_frame:
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
    p_glUniform1i(g_u_flip, 0);
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
    c.tex_slot = -1;
    for (int i = 0; i < 100; i++) {
        if (ge_queue_push(&g_ge_q, c)) {
            if (g_ge_wake) SetEvent(g_ge_wake);
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

int seto_ge_gpu_enqueue_draw(int prim, const psp_vertex *v, int count,
                             const psp_draw_state *st, uint32_t *tex,
                             int tex_slot) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return 0;
    if (!v || !st || count <= 0 || count > GE_MAX_VERTS) return 0;
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_DRAW;
    c.prim = (uint32_t)prim;
    c.count = (uint32_t)count;
    c.tex = tex;
    c.tex_slot = tex_slot;
    c.state = *st;
    memcpy(c.verts, v, (size_t)count * sizeof *v);
    /* Backpressure: the GPU thread is the only consumer, and dropping a draw
     * loses UI text at the end of a frame. Wait up to 250 ms for space. */
    for (int i = 0; i < 250; i++) {
        if (ge_queue_push(&g_ge_q, c)) {
            if (g_ge_wake) SetEvent(g_ge_wake);
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

void seto_ge_gpu_present(uint32_t capture_id) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return;
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_PRESENT;
    c.color = capture_id;
    c.tex_slot = -1;
    for (int i = 0; i < 250; i++) {
        if (ge_queue_push(&g_ge_q, c)) {
            if (g_ge_wake) SetEvent(g_ge_wake);
            return;
        }
        Sleep(1);
    }
}

void seto_gl_present_shutdown(HWND hwnd) {
    (void)hwnd;
    gl_cleanup();
}

int seto_gl_present_active(void) {
    return g_active;
}
