/* M0: optional OpenGL 3.3 presenter for the 480x272 software framebuffer.
 *
 * The guest/GE still render into the runtime's framebuffer. This module only
 * uploads that frame as a texture and draws it with a Lanczos2 shader, so the
 * window can be scaled with GPU filtering. It is the context/swapchain base
 * for a future GE backend; the software renderer remains the default. */

#include "gl_present.h"
#include "ge_queue.h"
#include "perf_mon.h"

#include <psprecomp/mem.h>
#include <windows.h>
#include <gdiplus.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include <ctype.h>
#include <stdatomic.h>
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
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif
#ifndef GL_MAX_RENDERBUFFER_SIZE
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFULL
#endif

static HWND g_hwnd;
static HDC g_dc;
static HGLRC g_ctx;
static int g_active;
static int g_filter;
static int g_post_filter;
static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint g_u_tex, g_u_texel, g_u_mode, g_u_flip, g_u_post, g_u_overlay;
static PFNWGLSWAPINTERVALEXTPROC g_swap_interval;

static CRITICAL_SECTION g_ovl_lock;
static int g_ovl_lock_ready;
static GLuint g_ovl_tex;
static int g_ovl_tex_w, g_ovl_tex_h;
static uint32_t *g_ovl_px;
static size_t g_ovl_cap;
static int g_ovl_x, g_ovl_y, g_ovl_w, g_ovl_h, g_ovl_dirty;

static ULONG_PTR g_gdiplus_token;
static int g_gdiplus_ready;

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
static PFNGLUNIFORM2IPROC p_glUniform2i;
static PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
static PFNGLBUFFERSUBDATAPROC p_glBufferSubData;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC p_glRenderbufferStorageMultisample;
static PFNGLFENCESYNCPROC p_glFenceSync;
static PFNGLWAITSYNCPROC p_glWaitSync;
static PFNGLDELETESYNCPROC p_glDeleteSync;

/* M1-M5: GPU thread + SPSC queue + multi-target scaled MSAA FBO manager. */
typedef struct {
    int      used;
    uint32_t addr;
    uint32_t stride;
    int      fmt;
    int      w;
    int      h;
    int      scale;
    int      phys_w;
    int      phys_h;
    int      samples;
    int      msaa_dirty;
    int      msaa_ds_dirty;
    int      ss_newer;
    int      ss_ds_newer;
    int      stencil_alpha_dirty;
    GLuint   fbo;
    GLuint   color_tex;
    GLuint   depth_rb;
    GLuint   msaa_fbo;
    GLuint   msaa_color_rb;
    GLuint   msaa_depth_rb;
} ge_gpu_fbo;

static HGLRC g_ge_ctx;
static HANDLE g_ge_thread;
static HANDLE g_ge_ready;
static HANDLE g_ge_wake;
static HANDLE g_ge_rb_done;
static volatile LONG g_ge_quit;
static CRITICAL_SECTION g_ge_lock;
static CRITICAL_SECTION g_ge_rb_lock;
static int g_ge_lock_ready;
static int g_ge_rb_lock_ready;
static int g_ge_active;
static int g_ge_ok;
static int g_ge_draw_mode;
static int g_ge_scale = 1;
static int g_ge_msaa;
static _Atomic int g_ge_pending_scale;
static _Atomic int g_ge_gpu_frame_ready;
static _Atomic uint32_t g_ge_front_seq;
static _Atomic int g_present_force_redraw;
static _Atomic int g_ovl_pending_redraw;
static uint32_t g_last_pres_seq;
static int g_last_pres_cw, g_last_pres_ch;
static _Atomic int g_ge_front_rb_req;
static int g_ge_front_rb_ok;
static uint32_t g_ge_front_rb_buf[GL_SCREEN_W * GL_SCREEN_H];
static int g_ge_auto_scale;
static int g_ge_max_dim = 4096;
static int g_ge_smart_2d = 1;
static GLuint g_ge_prog, g_ge_vao, g_ge_vbo, g_ge_tex, g_ge_front_tex, g_ge_front_fbo;
static GLuint g_ge_fbo, g_ge_depth;
static GLuint g_ge_stage_fbo, g_ge_stage_tex, g_ge_stage_depth;
static int g_ge_stage_w, g_ge_stage_h;
static int g_ge_front_w = GL_SCREEN_W, g_ge_front_h = GL_SCREEN_H;
static GLuint g_ge_self_tex, g_ge_blit_fbo;
static int g_ge_self_w, g_ge_self_h;
static GLsync g_ge_front_sync, g_present_read_sync;
static ge_gpu_fbo g_ge_fbos[GE_MAX_FBOS];
static int g_ge_cur_fbo_slot = -1;
static int g_ge_cur_is_msaa;
typedef struct {
    GLint filt;
    GLint wrap_s;
    GLint wrap_t;
    uint8_t valid;
} ge_tex_param_cache;

typedef struct {
    GLuint texture;
    ge_tex_param_cache params;
    uint64_t used_at;
    unsigned refs;
    int retain;
    char path[640];
} ge_hd_texture;

static GLuint g_ge_tex_cache[GE_TEX_CACHE_SLOTS];
static ge_tex_param_cache g_ge_tex_params[GE_TEX_CACHE_SLOTS];
// Keep reusable HD images around when the guest reuses its texture slots.
static ge_hd_texture g_ge_hd_cache[GE_TEX_CACHE_SLOTS + 64];

static const char *ge_cached_env(const char **slot, int *init, const char *name) {
    if (!*init) {
        *slot = getenv(name);
        *init = 1;
    }
    return *slot;
}
static const char *ge_hd_tex_dir_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_HD_TEX_DIR");
}
static const char *ge_dump_tex_dir_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_DUMP_TEX_DIR");
}
static const char *ge_trace_hd_tex_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_TRACE_HD_TEX");
}
static const char *ge_debug_draw_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_GE_DEBUG_DRAW");
}
static const char *ge_debug_fbo_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_GE_DEBUG_FBO");
}
static const char *ge_shot_dir_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_GL_SHOT_DIR");
}
static const char *ge_hires_shot_dir_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_GL_HIRES_SHOT_DIR");
}
static const char *ge_present_shot_dir_env(void) {
    static const char *v; static int init;
    return ge_cached_env(&v, &init, "SETO_GL_PRESENT_SHOT_DIR");
}
seto_perf_counters g_seto_perf;
int g_seto_perf_enabled = 0;
static char g_gl_gpu_info[256] = "unknown";
static ge_hd_texture *g_ge_hd_slots[GE_TEX_CACHE_SLOTS];
static uint8_t g_ge_hd_pending_slots[GE_TEX_CACHE_SLOTS];
static int32_t g_ge_hd_pending_path_off[GE_TEX_CACHE_SLOTS];
static uint64_t g_ge_hd_clock;
static uint64_t g_dumped_hashes[2048];
static int g_dumped_count;
static uint64_t g_hd_miss_hashes[512];
static int g_hd_miss_cursor;
static int g_hd_logged;
static GLint g_ge_color_loc;
static int g_ge_w, g_ge_h;
static int g_ge_have_front;
static int g_ge_passthrough_sw;
static ge_queue g_ge_q;
static int g_ge_queue_ready;
static uint32_t g_ge_rgb = 0xFFFFFFFFu;

static GLuint g_ge_geo_prog, g_ge_geo_vao, g_ge_geo_vbo;
static GLint g_ge_geo_size, g_ge_geo_half_px, g_ge_geo_clear;
static GLint g_ge_geo_use_subpixel, g_ge_geo_smart_2d, g_ge_geo_render_scale, g_ge_geo_fbo_scale;
static GLint g_ge_geo_atest, g_ge_geo_afunc, g_ge_geo_aref, g_ge_geo_amask;
static GLint g_ge_geo_ctest, g_ge_geo_cfunc, g_ge_geo_cref, g_ge_geo_cmask;
static GLint g_ge_geo_tex_on, g_ge_geo_tex_func, g_ge_geo_tex_alpha;
static GLint g_ge_geo_tex_double, g_ge_geo_tex_env, g_ge_geo_tex_size;
static GLint g_ge_geo_tex_sampler, g_ge_geo_tex_is_fbo, g_ge_geo_fbo_tex_size;
static GLint g_ge_geo_tex_clamp, g_ge_geo_tex_filter;
static GLint g_ge_geo_zrange, g_ge_geo_zmin, g_ge_geo_zmax;
static GLint g_ge_geo_fog_on, g_ge_geo_fog_color, g_ge_geo_lmode;
static GLint g_ge_geo_zclamp_vert;
static GLint g_ge_geo_src_premul, g_ge_geo_sfix_rgb, g_ge_geo_alpha_2x;

#define GE_VBO_RING_VERTS 65536u
static uint32_t g_ge_geo_vbo_head;

typedef struct {
    int valid;
    uint8_t sub_init;
    int size_w, size_h;
    GLfloat half_px;
    int clear, use_subpixel;
    int tex_on, tex_w, tex_h;
    int tex_is_fbo, fbo_tw, fbo_th, fbo_ts;
    int clamp_u, clamp_v, tex_filter, smart_2d, render_scale;
    int tex_func, tex_alpha, tex_double;
    uint32_t tex_env;
    int atest, afunc, aref, amask;
    int ctest, cfunc;
    uint32_t cref, cmask;
    int lmode, fog_on;
    uint32_t fog_color;
    int zrange, zmin, zmax, zclamp_vert;
    int src_premul, alpha_2x;
    uint32_t sfix;
} ge_geo_ucache;

static ge_geo_ucache g_geo_uc;

typedef struct {
    int states_valid;
    int vp_valid;
    int vp_w, vp_h;
    GLfloat point_size;
    int multisample;
    int depth_clamp;
    int scissor_on;
    int sc_x, sc_y, sc_w, sc_h;
    uint8_t color_mask;
    int blend_on;
    int blend_op;
    GLenum blend_src, blend_dst;
    int blend_const_valid;
    uint32_t blend_const_rgb;
    int cull_on, cull_front;
    int depth_on, depth_func, depth_mask;
    int stencil_on, stencil_func, stencil_ref;
    GLuint stencil_rmask, stencil_wmask;
    int stencil_fail, stencil_zfail, stencil_zpass;
    GLuint bound_tex;
} ge_gl_state_cache;

static ge_gl_state_cache g_gl_sc;

static void ge_gl_state_invalidate(void) {
    g_gl_sc.states_valid = 0;
    g_gl_sc.vp_valid = 0;
    g_gl_sc.bound_tex = 0;
}

static const char *vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform int u_flip;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_suv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_uv.x, u_flip != 0 ? 1.0 - a_uv.y : a_uv.y);\n"
    "  v_suv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *fs_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec2 v_suv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform int u_mode;\n"
    "uniform int u_post;\n"
    "uniform int u_overlay;\n"
    "float lanczos2(float x) {\n"
    "  x = abs(x);\n"
    "  if (x < 1e-4) return 1.0;\n"
    "  if (x >= 2.0) return 0.0;\n"
    "  float p = 3.14159265 * x;\n"
    "  return 2.0 * sin(p) * sin(p * 0.5) / (p * p);\n"
    "}\n"
    "void main() {\n"
    "  if (u_overlay != 0) {\n"
    "    o_color = texture(u_tex, v_uv);\n"
    "    return;\n"
    "  }\n"
    "  vec3 col;\n"
    "  if (u_mode < 2) {\n"
    "    col = texture(u_tex, v_uv).rgb;\n"
    "  } else {\n"
    "    vec2 uv = v_uv * (1.0 / u_texel) - 0.5;\n"
    "    vec2 base = floor(uv);\n"
    "    vec2 f = uv - base;\n"
    "    float wx[4];\n"
    "    float wy[4];\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "      wx[k] = lanczos2(float(k - 1) - f.x);\n"
    "      wy[k] = lanczos2(float(k - 1) - f.y);\n"
    "    }\n"
    "    vec4 sum = vec4(0.0); float wsum = 0.0;\n"
    "    for (int j = 0; j < 4; j++) {\n"
    "      float wyj = wy[j];\n"
    "      float sy = (base.y + float(j - 1) + 0.5) * u_texel.y;\n"
    "      for (int i = 0; i < 4; i++) {\n"
    "        float w = wx[i] * wyj;\n"
    "        vec2 st = vec2((base.x + float(i - 1) + 0.5) * u_texel.x, sy);\n"
    "        sum += texture(u_tex, st) * w;\n"
    "        wsum += w;\n"
    "      }\n"
    "    }\n"
    "    col = (sum / wsum).rgb;\n"
    "  }\n"
    "  if (u_post == 1) {\n"
    "    vec3 n = texture(u_tex, v_uv + vec2(0.0, u_texel.y)).rgb;\n"
    "    vec3 s = texture(u_tex, v_uv - vec2(0.0, u_texel.y)).rgb;\n"
    "    vec3 e = texture(u_tex, v_uv + vec2(u_texel.x, 0.0)).rgb;\n"
    "    vec3 w = texture(u_tex, v_uv - vec2(u_texel.x, 0.0)).rgb;\n"
    "    vec3 mn = min(col, min(min(n, s), min(e, w)));\n"
    "    vec3 mx = max(col, max(max(n, s), max(e, w)));\n"
    "    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);\n"
    "    float wt = -0.18 * dot(amp, vec3(0.299, 0.587, 0.114));\n"
    "    col = clamp((col + wt * (n + s + e + w)) / max(1.0 + 4.0 * wt, 0.25), 0.0, 1.0);\n"
    "  } else if (u_post == 2) {\n"
    "    vec2 lc = v_suv * vec2(480.0, 272.0);\n"
    "    vec2 f2 = fract(lc);\n"
    "    float rowm = 0.86 + 0.14 * sin(f2.y * 3.14159265);\n"
    "    float sub = fract(lc.x * 3.0);\n"
    "    vec3 mask = vec3(0.92);\n"
    "    if (sub < 0.333) mask.r = 1.06;\n"
    "    else if (sub < 0.666) mask.g = 1.06;\n"
    "    else mask.b = 1.06;\n"
    "    col = clamp(col * mask * rowm, 0.0, 1.0);\n"
    "  } else if (u_post == 3) {\n"
    "    float scan = 0.82 + 0.18 * cos(v_suv.y * 272.0 * 6.2831853);\n"
    "    vec2 d = v_suv - vec2(0.5);\n"
    "    float vig = clamp(1.05 - dot(d, d) * 0.45, 0.70, 1.0);\n"
    "    col = clamp(col * scan * vig, 0.0, 1.0);\n"
    "  }\n"
    "  o_color = vec4(col, 1.0);\n"
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

#define SETO_GL_LOAD_OPT(type, name)                                               \
    do {                                                                      \
        p_##name = (type)wglGetProcAddress(#name);                            \
        if (!p_##name) {                                                      \
            HMODULE glmod = GetModuleHandleA("opengl32.dll");                 \
            if (glmod) p_##name = (type)GetProcAddress(glmod, #name);         \
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
    SETO_GL_LOAD(PFNGLUNIFORM2IPROC, glUniform2i);
    SETO_GL_LOAD(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer);
    SETO_GL_LOAD_OPT(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
    SETO_GL_LOAD_OPT(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample);
    SETO_GL_LOAD_OPT(PFNGLFENCESYNCPROC, glFenceSync);
    SETO_GL_LOAD_OPT(PFNGLWAITSYNCPROC, glWaitSync);
    SETO_GL_LOAD_OPT(PFNGLDELETESYNCPROC, glDeleteSync);
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
    g_u_post = p_glGetUniformLocation(g_prog, "u_post");
    g_u_overlay = p_glGetUniformLocation(g_prog, "u_overlay");
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
    "layout(location=6) in vec2 a_fpos;\n"
    "layout(location=7) in float a_clip_w;\n"
    "uniform vec2 u_size;\n"
    "uniform vec2 u_tex_size;\n"
    "uniform float u_half_px;\n"
    "uniform int u_zclamp_vert;\n"
    "uniform int u_use_subpixel;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "out float v_z;\n"
    "out vec3 v_spec;\n"
    "out float v_fog;\n"
    "void main() {\n"
    "  vec2 pos_px = ((u_use_subpixel != 0) ? a_fpos : a_pos) + vec2(u_half_px);\n"
    "  vec2 p = pos_px / u_size;\n"
    "  float z = u_zclamp_vert != 0 ? clamp(floor(a_z + 0.5), 0.0, 65535.0) : a_z;\n"
    "  float ndc_z = 1.0 - z / 65535.0 * 2.0;\n"
    "  float w = (u_use_subpixel != 0 && a_clip_w > 0.0) ? a_clip_w : 1.0;\n"
    "  gl_Position = vec4((p.x * 2.0 - 1.0) * w, (1.0 - p.y * 2.0) * w,\n"
    "                     ndc_z * w, w);\n"
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
    "uniform vec2 u_tex_size;\n"
    "uniform int u_tex_is_fbo;\n"
    "uniform ivec2 u_fbo_tex_size;\n"
    "uniform int u_fbo_tex_scale;\n"
    "uniform ivec2 u_tex_clamp;\n"
    "uniform int u_tex_filter;\n"
    "uniform int u_smart_2d;\n"
    "uniform float u_render_scale;\n"
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
    "int wrap_tex_i(int c, int sz, int do_clamp) {\n"
    "  if (do_clamp != 0) return clamp(c, 0, sz - 1);\n"
    "  int r = c % sz;\n"
    "  if (r < 0) r += sz;\n"
    "  return clamp(r, 0, sz - 1);\n"
    "}\n"
    "vec4 fetch_fbo_texel(int ix, int iy, int tw, int th) {\n"
    "  int wx = wrap_tex_i(ix, tw, u_tex_clamp.x);\n"
    "  int wy = wrap_tex_i(iy, th, u_tex_clamp.y);\n"
    "  int fx = clamp(wx, 0, u_fbo_tex_size.x - 1);\n"
    "  int fy = clamp(u_fbo_tex_size.y - 1 - wy, 0, u_fbo_tex_size.y - 1);\n"
    "  return texelFetch(u_tex, ivec2(fx, fy), 0);\n"
    "}\n"
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
    "    vec4 t;\n"
    "    if (u_tex_is_fbo != 0) {\n"
    "      int ts = max(1, u_fbo_tex_scale);\n"
    "      int tw = max(1, int(u_tex_size.x + 0.5)) * ts;\n"
    "      int th = max(1, int(u_tex_size.y + 0.5)) * ts;\n"
    "      vec2 puv = v_uv * u_tex_size * float(ts);\n"
    "      if (u_tex_filter == 0) {\n"
    "        t = fetch_fbo_texel(int(floor(puv.x)), int(floor(puv.y)), tw, th);\n"
    "      } else {\n"
    "        vec2 tc = puv - vec2(0.5);\n"
    "        int x0 = int(floor(tc.x)), y0 = int(floor(tc.y));\n"
    "        vec2 f = tc - vec2(float(x0), float(y0));\n"
    "        vec4 t00 = fetch_fbo_texel(x0,     y0,     tw, th);\n"
    "        vec4 t10 = fetch_fbo_texel(x0 + 1, y0,     tw, th);\n"
    "        vec4 t01 = fetch_fbo_texel(x0,     y0 + 1, tw, th);\n"
    "        vec4 t11 = fetch_fbo_texel(x0 + 1, y0 + 1, tw, th);\n"
    "        t = mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);\n"
    "      }\n"
    "    } else if (u_smart_2d != 0) {\n"
    "      vec2 p = v_uv * u_tex_size;\n"
    "      vec2 ip = floor(p - 0.5) + 0.5;\n"
    "      vec2 fp = clamp((p - ip - 0.5) * u_render_scale + 0.5, 0.0, 1.0);\n"
    "      t = texture(u_tex, (ip + fp) / u_tex_size);\n"
    "    } else {\n"
    "      t = texture(u_tex, v_uv);\n"
    "    }\n"
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
    g_ge_geo_tex_is_fbo = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_is_fbo");
    g_ge_geo_fbo_tex_size = p_glGetUniformLocation(g_ge_geo_prog, "u_fbo_tex_size");
    g_ge_geo_fbo_scale = p_glGetUniformLocation(g_ge_geo_prog, "u_fbo_tex_scale");
    g_ge_geo_tex_clamp = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_clamp");
    g_ge_geo_tex_filter = p_glGetUniformLocation(g_ge_geo_prog, "u_tex_filter");
    g_ge_geo_use_subpixel = p_glGetUniformLocation(g_ge_geo_prog, "u_use_subpixel");
    g_ge_geo_smart_2d = p_glGetUniformLocation(g_ge_geo_prog, "u_smart_2d");
    g_ge_geo_render_scale = p_glGetUniformLocation(g_ge_geo_prog, "u_render_scale");
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
    p_glUseProgram(g_ge_geo_prog);
    p_glUniform1i(g_ge_geo_tex_sampler, 0);
    memset(&g_geo_uc, 0, sizeof g_geo_uc);
    return 1;
}

static int ge_make_geo_vao(void) {
    p_glGenVertexArrays(1, &g_ge_geo_vao);
    p_glBindVertexArray(g_ge_geo_vao);
    p_glGenBuffers(1, &g_ge_geo_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(GE_VBO_RING_VERTS * sizeof(psp_vertex)),
                   NULL, GL_STREAM_DRAW);
    g_ge_geo_vbo_head = 0u;
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
    p_glEnableVertexAttribArray(6);
    p_glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, fx));
    p_glEnableVertexAttribArray(7);
    p_glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(psp_vertex),
                            (const void *)(uintptr_t)offsetof(psp_vertex, clip_w));
    return 1;
}

static GLint ge_upload_geo_verts(const psp_vertex *data, uint32_t count) {
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    if (!p_glBufferSubData || count > GE_VBO_RING_VERTS) {
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * sizeof(psp_vertex)),
                       data, GL_STREAM_DRAW);
        g_ge_geo_vbo_head = GE_VBO_RING_VERTS;
        return 0;
    }
    if (g_ge_geo_vbo_head + count > GE_VBO_RING_VERTS) {
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(GE_VBO_RING_VERTS * sizeof(psp_vertex)),
                       NULL, GL_STREAM_DRAW);
        g_ge_geo_vbo_head = 0u;
    }
    const GLint base = (GLint)g_ge_geo_vbo_head;
    p_glBufferSubData(GL_ARRAY_BUFFER,
                      (GLintptr)((size_t)g_ge_geo_vbo_head * sizeof(psp_vertex)),
                      (GLsizeiptr)((size_t)count * sizeof(psp_vertex)), data);
    g_ge_geo_vbo_head += count;
    return base;
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

static void ge_apply_state(const psp_draw_state *st, uint32_t clear_rgba,
                           int fb_w, int fb_h, int scale) {
    const int full = !g_gl_sc.states_valid;
    if (full) {
        glEnable(GL_DEPTH_CLAMP);
        g_gl_sc.depth_clamp = 1;
        glEnable(GL_SCISSOR_TEST);
        g_gl_sc.scissor_on = 1;
        g_gl_sc.blend_const_valid = 0;
        g_gl_sc.states_valid = 1;
    } else {
        if (!g_gl_sc.depth_clamp) {
            glEnable(GL_DEPTH_CLAMP);
            g_gl_sc.depth_clamp = 1;
        }
        if (!g_gl_sc.scissor_on) {
            glEnable(GL_SCISSOR_TEST);
            g_gl_sc.scissor_on = 1;
        }
    }

    int x0 = st->sc_x0, y0 = st->sc_y0, x1 = st->sc_x1, y1 = st->sc_y1;
    if (st->reg_enable) {
        if (st->reg_x0 > x0) x0 = st->reg_x0;
        if (st->reg_y0 > y0) y0 = st->reg_y0;
        if (st->reg_x1 < x1) x1 = st->reg_x1;
        if (st->reg_y1 < y1) y1 = st->reg_y1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fb_w) x1 = fb_w - 1;
    if (y1 >= fb_h) y1 = fb_h - 1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    if (x1 >= x0 && y1 >= y0) {
        sx = x0 * scale;
        sy = (fb_h - (y1 + 1)) * scale;
        sw = (x1 - x0 + 1) * scale;
        sh = (y1 - y0 + 1) * scale;
    }
    if (full || g_gl_sc.sc_x != sx || g_gl_sc.sc_y != sy ||
        g_gl_sc.sc_w != sw || g_gl_sc.sc_h != sh) {
        glScissor(sx, sy, sw, sh);
        g_gl_sc.sc_x = sx;
        g_gl_sc.sc_y = sy;
        g_gl_sc.sc_w = sw;
        g_gl_sc.sc_h = sh;
    }

    const GLboolean rm = (st->write_mask & 0x000000FFu) == 0u ? GL_TRUE : GL_FALSE;
    const GLboolean gm = (st->write_mask & 0x0000FF00u) == 0u ? GL_TRUE : GL_FALSE;
    const GLboolean bm = (st->write_mask & 0x00FF0000u) == 0u ? GL_TRUE : GL_FALSE;
    const GLuint smask = (~st->write_mask >> 24) & 0xFFu;

    if (st->clear) {
        /* CLEAR writes vertex colour/alpha/depth directly. Enable depth/stencil
         * tests with ALWAYS so GL executes the buffer writes. */
        if (full || g_gl_sc.blend_on) {
            glDisable(GL_BLEND);
            g_gl_sc.blend_on = 0;
        }
        if (full || g_gl_sc.cull_on) {
            glDisable(GL_CULL_FACE);
            g_gl_sc.cull_on = 0;
        }
        const int cd = st->clear_depth ? 1 : 0;
        if (cd) {
            if (full || !g_gl_sc.depth_on) {
                glEnable(GL_DEPTH_TEST);
                g_gl_sc.depth_on = 1;
            }
            if (full || g_gl_sc.depth_func != 1) {
                glDepthFunc(GL_ALWAYS);
                g_gl_sc.depth_func = 1;
            }
            if (full || !g_gl_sc.depth_mask) {
                glDepthMask(GL_TRUE);
                g_gl_sc.depth_mask = 1;
            }
        } else {
            if (full || g_gl_sc.depth_on) {
                glDisable(GL_DEPTH_TEST);
                g_gl_sc.depth_on = 0;
            }
            if (full || g_gl_sc.depth_mask) {
                glDepthMask(GL_FALSE);
                g_gl_sc.depth_mask = 0;
            }
        }
        const GLboolean cr = st->clear_color ? rm : GL_FALSE;
        const GLboolean cg = st->clear_color ? gm : GL_FALSE;
        const GLboolean cb = st->clear_color ? bm : GL_FALSE;
        const GLboolean ca = (st->clear_alpha && (st->write_mask & 0xFF000000u) == 0u) ? GL_TRUE : GL_FALSE;
        const uint8_t cm = (uint8_t)((cr ? 1u : 0u) | (cg ? 2u : 0u) | (cb ? 4u : 0u) | (ca ? 8u : 0u));
        if (full || g_gl_sc.color_mask != cm) {
            glColorMask(cr, cg, cb, ca);
            g_gl_sc.color_mask = cm;
        }
        if (st->clear_alpha && smask) {
            const int sref = (int)((clear_rgba >> 24) & 0xFFu);
            if (full || !g_gl_sc.stencil_on) {
                glEnable(GL_STENCIL_TEST);
                g_gl_sc.stencil_on = 1;
            }
            if (full || g_gl_sc.stencil_func != 1 || g_gl_sc.stencil_ref != sref || g_gl_sc.stencil_rmask != 0xFFu) {
                glStencilFunc(GL_ALWAYS, (GLint)sref, 0xFFu);
                g_gl_sc.stencil_func = 1;
                g_gl_sc.stencil_ref = sref;
                g_gl_sc.stencil_rmask = 0xFFu;
            }
            if (full || g_gl_sc.stencil_fail != 2 || g_gl_sc.stencil_zfail != 2 || g_gl_sc.stencil_zpass != 2) {
                glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
                g_gl_sc.stencil_fail = g_gl_sc.stencil_zfail = g_gl_sc.stencil_zpass = 2;
            }
            if (full || g_gl_sc.stencil_wmask != smask) {
                glStencilMask(smask);
                g_gl_sc.stencil_wmask = smask;
            }
        } else {
            if (full || g_gl_sc.stencil_on) {
                glDisable(GL_STENCIL_TEST);
                g_gl_sc.stencil_on = 0;
            }
            if (full || g_gl_sc.stencil_wmask != 0u) {
                glStencilMask(0u);
                g_gl_sc.stencil_wmask = 0u;
            }
        }
        return;
    }

    {
        const uint8_t cm = (uint8_t)((rm ? 1u : 0u) | (gm ? 2u : 0u) | (bm ? 4u : 0u));
        if (full || g_gl_sc.color_mask != cm) {
            glColorMask(rm, gm, bm, GL_FALSE);
            g_gl_sc.color_mask = cm;
        }
    }
    if (st->blend) {
        GLenum sfactor, dfactor;
        int use_const, src_premul, alpha_2x;
        uint32_t const_rgb;
        ge_plan_blend(st, &sfactor, &dfactor, &use_const, &const_rgb,
                      &src_premul, &alpha_2x);
        const int bfirst = full || !g_gl_sc.blend_on;
        if (bfirst) {
            glEnable(GL_BLEND);
            g_gl_sc.blend_on = 1;
        }
        if (bfirst || g_gl_sc.blend_op != st->blend_op) {
            p_glBlendEquation(ge_blend_op(st->blend_op));
            g_gl_sc.blend_op = st->blend_op;
        }
        if (use_const && (!g_gl_sc.blend_const_valid || g_gl_sc.blend_const_rgb != const_rgb)) {
            p_glBlendColor((GLfloat)(const_rgb & 0xFFu) / 255.0f,
                           (GLfloat)((const_rgb >> 8) & 0xFFu) / 255.0f,
                           (GLfloat)((const_rgb >> 16) & 0xFFu) / 255.0f, 1.0f);
            g_gl_sc.blend_const_rgb = const_rgb;
            g_gl_sc.blend_const_valid = 1;
        }
        if (bfirst || g_gl_sc.blend_src != sfactor || g_gl_sc.blend_dst != dfactor) {
            p_glBlendFuncSeparate(sfactor, dfactor, GL_ZERO, GL_ONE);
            g_gl_sc.blend_src = sfactor;
            g_gl_sc.blend_dst = dfactor;
        }
    } else if (full || g_gl_sc.blend_on) {
        glDisable(GL_BLEND);
        g_gl_sc.blend_on = 0;
    }

    if (st->cull_enable) {
        const int cfirst = full || !g_gl_sc.cull_on;
        if (cfirst) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            g_gl_sc.cull_on = 1;
        }
        const int cf = st->cull_face ? 1 : 0;
        if (cfirst || g_gl_sc.cull_front != cf) {
            glFrontFace(cf ? GL_CW : GL_CCW);
            g_gl_sc.cull_front = cf;
        }
    } else if (full || g_gl_sc.cull_on) {
        glDisable(GL_CULL_FACE);
        g_gl_sc.cull_on = 0;
    }

    if (st->ztest) {
        const int zfirst = full || !g_gl_sc.depth_on;
        if (zfirst) {
            glEnable(GL_DEPTH_TEST);
            g_gl_sc.depth_on = 1;
        }
        if (zfirst || g_gl_sc.depth_func != st->zfunc) {
            glDepthFunc(ge_depth_func(st->zfunc));
            g_gl_sc.depth_func = st->zfunc;
        }
        const int zw = st->zwrite ? 1 : 0;
        if (full || g_gl_sc.depth_mask != zw) {
            glDepthMask(zw ? GL_TRUE : GL_FALSE);
            g_gl_sc.depth_mask = zw;
        }
    } else {
        if (full || g_gl_sc.depth_on) {
            glDisable(GL_DEPTH_TEST);
            g_gl_sc.depth_on = 0;
        }
        if (full || g_gl_sc.depth_mask) {
            glDepthMask(GL_FALSE);
            g_gl_sc.depth_mask = 0;
        }
    }

    if (st->stencil) {
        const int sfirst = full || !g_gl_sc.stencil_on;
        const int sref = st->stencil_ref & 0xFF;
        const GLuint srm = (GLuint)(st->stencil_mask & 0xFF);
        if (sfirst) {
            glEnable(GL_STENCIL_TEST);
            g_gl_sc.stencil_on = 1;
        }
        if (sfirst || g_gl_sc.stencil_func != st->stencil_func ||
            g_gl_sc.stencil_ref != sref || g_gl_sc.stencil_rmask != srm) {
            glStencilFunc(ge_stencil_func(st->stencil_func), sref, srm);
            g_gl_sc.stencil_func = st->stencil_func;
            g_gl_sc.stencil_ref = sref;
            g_gl_sc.stencil_rmask = srm;
        }
        if (sfirst || g_gl_sc.stencil_fail != st->stencil_fail ||
            g_gl_sc.stencil_zfail != st->stencil_zfail ||
            g_gl_sc.stencil_zpass != st->stencil_zpass) {
            glStencilOp(ge_stencil_op(st->stencil_fail),
                        ge_stencil_op(st->stencil_zfail),
                        ge_stencil_op(st->stencil_zpass));
            g_gl_sc.stencil_fail = st->stencil_fail;
            g_gl_sc.stencil_zfail = st->stencil_zfail;
            g_gl_sc.stencil_zpass = st->stencil_zpass;
        }
        if (full || g_gl_sc.stencil_wmask != smask) {
            glStencilMask(smask);
            g_gl_sc.stencil_wmask = smask;
        }
    } else {
        if (full || g_gl_sc.stencil_on) {
            glDisable(GL_STENCIL_TEST);
            g_gl_sc.stencil_on = 0;
        }
        if (full || g_gl_sc.stencil_wmask != 0u) {
            glStencilMask(0u);
            g_gl_sc.stencil_wmask = 0u;
        }
    }
}

static void ge_alloc_msaa_for_fbo(ge_gpu_fbo *f);

static void ge_resolve_fbo_ex(ge_gpu_fbo *f, int need_ds) {
    if (!f || !f->used || f->samples < 2 || !f->msaa_fbo)
        return;
    const int do_col = f->msaa_dirty;
    const int do_ds = need_ds && f->msaa_ds_dirty;
    if (!do_col && !do_ds)
        return;
    ge_gl_state_invalidate();
    glDisable(GL_SCISSOR_TEST);
    if (do_col) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        f->msaa_dirty = 0;
        f->ss_newer = 0;
    }
    if (do_ds) {
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        f->msaa_ds_dirty = 0;
        f->ss_ds_newer = 0;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, f->msaa_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
    if (do_col) {
        p_glBlitFramebuffer(0, 0, f->phys_w, f->phys_h,
                            0, 0, f->phys_w, f->phys_h,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    if (do_ds) {
        p_glBlitFramebuffer(0, 0, f->phys_w, f->phys_h,
                            0, 0, f->phys_w, f->phys_h,
                            GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static void ge_sync_ss_to_msaa(ge_gpu_fbo *f) {
    if (!f || !f->used || f->samples < 2 || !f->msaa_fbo)
        return;
    const int do_col = f->ss_newer;
    const int do_ds = f->ss_ds_newer;
    if (!do_col && !do_ds)
        return;
    ge_gl_state_invalidate();
    glDisable(GL_SCISSOR_TEST);
    if (do_col) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        f->ss_newer = 0;
        f->msaa_dirty = 0;
    }
    if (do_ds) {
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        f->ss_ds_newer = 0;
        f->msaa_ds_dirty = 0;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->msaa_fbo);
    if (do_col) {
        p_glBlitFramebuffer(0, 0, f->phys_w, f->phys_h,
                            0, 0, f->phys_w, f->phys_h,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    if (do_ds) {
        p_glBlitFramebuffer(0, 0, f->phys_w, f->phys_h,
                            0, 0, f->phys_w, f->phys_h,
                            GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static void ge_bind_target_for_draw(int slot, int use_msaa, int need_ss_ds) {
    if (slot < 0 || slot >= GE_MAX_FBOS || !g_ge_fbos[slot].used) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        g_ge_cur_fbo_slot = -1;
        g_ge_cur_is_msaa = 0;
        return;
    }
    ge_gpu_fbo *f = &g_ge_fbos[slot];
    const int want_msaa = (use_msaa && f->samples >= 2 && f->msaa_fbo != 0);
    if (want_msaa) {
        if (f->ss_newer || f->ss_ds_newer)
            ge_sync_ss_to_msaa(f);
    } else if (f->samples >= 2 && f->msaa_fbo &&
               (f->msaa_dirty || (need_ss_ds && f->msaa_ds_dirty))) {
        ge_resolve_fbo_ex(f, need_ss_ds);
    }
    if (g_ge_cur_fbo_slot != slot || g_ge_cur_is_msaa != want_msaa) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, want_msaa ? f->msaa_fbo : f->fbo);
        g_ge_cur_fbo_slot = slot;
        g_ge_cur_is_msaa = want_msaa;
    }
}

static int ge_is_full_clear(const ge_cmd *c, const ge_gpu_fbo *f) {
    if (!c || !f || !c->state.clear || c->prim != PSP_PRIM_TRIANGLES ||
        c->count < 6u || (c->count % 3u) != 0u)
        return 0;
    const uint32_t col0 = c->verts[0].rgba;
    const float z0 = c->verts[0].z;
    int bx0 = c->verts[0].x, by0 = c->verts[0].y;
    int bx1 = bx0, by1 = by0;
    int64_t area2 = 0;
    for (uint32_t i = 0; i < c->count; i += 3u) {
        for (int k = 0; k < 3; k++) {
            const psp_vertex *vp = &c->verts[i + (uint32_t)k];
            if (vp->rgba != col0 || vp->z != z0) return 0;
            if (vp->x < bx0) bx0 = vp->x;
            if (vp->y < by0) by0 = vp->y;
            if (vp->x > bx1) bx1 = vp->x;
            if (vp->y > by1) by1 = vp->y;
        }
        const psp_vertex *a = &c->verts[i], *b = &c->verts[i + 1u], *d = &c->verts[i + 2u];
        int64_t a2 = (int64_t)(b->x - a->x) * (int64_t)(d->y - a->y) -
                     (int64_t)(b->y - a->y) * (int64_t)(d->x - a->x);
        if (a2 < 0) a2 = -a2;
        area2 += a2;
    }
    const int req_w = f->w < 480 ? f->w : 480;
    if (bx0 > 0 || by0 > 0 || bx1 < req_w || by1 < f->h) return 0;
    int x0 = c->state.sc_x0, y0 = c->state.sc_y0, x1 = c->state.sc_x1, y1 = c->state.sc_y1;
    if (c->state.reg_enable) {
        if (c->state.reg_x0 > x0) x0 = c->state.reg_x0;
        if (c->state.reg_y0 > y0) y0 = c->state.reg_y0;
        if (c->state.reg_x1 < x1) x1 = c->state.reg_x1;
        if (c->state.reg_y1 < y1) y1 = c->state.reg_y1;
    }
    if (x0 > 0 || y0 > 0 || x1 < req_w - 1 || y1 < f->h - 1) return 0;
    return area2 >= 2LL * (int64_t)(bx1 - bx0) * (int64_t)(by1 - by0);
}

static int ge_gdiplus_start_once(void) {
    if (g_gdiplus_ready) return 1;
    GdiplusStartupInput in = {0};
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gdiplus_token, &in, NULL) != Ok) return 0;
    g_gdiplus_ready = 1;
    return 1;
}

static void ge_dump_tex_png(uint64_t hash, const uint32_t *rgba, int w, int h) {
    const char *dir = ge_dump_tex_dir_env();
    if (!dir || !dir[0] || !hash || !rgba || w <= 0 || h <= 0) return;
    for (int i = 0; i < g_dumped_count; i++) {
        if (g_dumped_hashes[i] == hash) return;
    }
    CreateDirectoryA(dir, NULL);
    char path[320];
    snprintf(path, sizeof path, "%s/%016llx.png", dir, (unsigned long long)hash);
    WCHAR wpath[320];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 320) <= 0) return;
    if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
        if (g_dumped_count < 2048) g_dumped_hashes[g_dumped_count++] = hash;
        return;
    }
    if (!ge_gdiplus_start_once()) return;
    const size_t n = (size_t)w * (size_t)h;
    uint32_t *bgra = (uint32_t *)malloc(n * 4u);
    if (!bgra) return;
    for (size_t i = 0; i < n; i++) {
        const uint32_t c = rgba[i];
        bgra[i] = (c & 0xFF00FF00u) | ((c & 0x000000FFu) << 16) | ((c & 0x00FF0000u) >> 16);
    }
    static const CLSID CLSID_PNG = {
        0x557CF406u, 0x1A04u, 0x11D3u, { 0x9Au, 0x73u, 0x00u, 0x00u, 0xF8u, 0x1Eu, 0xF3u, 0x2Eu }
    };
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB, (BYTE *)bgra, &bmp) == Ok) {
        GdipSaveImageToFile((GpImage *)bmp, wpath, &CLSID_PNG, NULL);
        GdipDisposeImage((GpImage *)bmp);
    }
    free(bgra);
    if (g_dumped_count < 2048) g_dumped_hashes[g_dumped_count++] = hash;
}

static int ge_load_ppm_rgba(const char *path, uint32_t **out_px, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char hdr[4] = {0};
    if (fscanf(f, "%3s", hdr) != 1 || strcmp(hdr, "P6") != 0) { fclose(f); return 0; }
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (c > ' ') { ungetc(c, f); break; }
    }
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 || w <= 0 || h <= 0 ||
        w > g_ge_max_dim || h > g_ge_max_dim || maxv != 255) {
        fclose(f);
        return 0;
    }
    fgetc(f);
    const size_t n = (size_t)w * (size_t)h;
    uint8_t *rgb = (uint8_t *)malloc(n * 3u);
    uint32_t *rgba = (uint32_t *)malloc(n * 4u);
    if (!rgb || !rgba || fread(rgb, 1, n * 3u, f) != n * 3u) {
        free(rgb);
        free(rgba);
        fclose(f);
        return 0;
    }
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        rgba[i] = (uint32_t)rgb[i * 3u] | ((uint32_t)rgb[i * 3u + 1u] << 8) |
                  ((uint32_t)rgb[i * 3u + 2u] << 16) | 0xFF000000u;
    }
    free(rgb);
    *out_px = rgba;
    *out_w = w;
    *out_h = h;
    return 1;
}

static int ge_load_png_rgba(const char *path, uint32_t **out_px, int *out_w, int *out_h) {
    WCHAR wpath[520];
    if (!path || !path[0] || !out_px || !out_w || !out_h) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 520) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 520) <= 0)
        return 0;
    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES || !ge_gdiplus_start_once())
        return 0;
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromFile(wpath, &bmp) != Ok || !bmp) return 0;
    UINT uw = 0, uh = 0;
    if (GdipGetImageWidth((GpImage *)bmp, &uw) == Ok &&
        GdipGetImageHeight((GpImage *)bmp, &uh) == Ok &&
        uw > 0 && uh > 0 && (int)uw <= g_ge_max_dim && (int)uh <= g_ge_max_dim) {
        GpRect rc = { 0, 0, (INT)uw, (INT)uh };
        BitmapData bd = {0};
        if (GdipBitmapLockBits(bmp, &rc, ImageLockModeRead,
                               PixelFormat32bppARGB, &bd) == Ok) {
            uint32_t *px = (uint32_t *)malloc((size_t)uw * (size_t)uh * 4u);
            if (px) {
                for (UINT y = 0; y < uh; y++) {
                    const uint32_t *srow = (const uint32_t *)((const uint8_t *)bd.Scan0 +
                                           (ptrdiff_t)y * (ptrdiff_t)bd.Stride);
                    uint32_t *drow = px + (size_t)y * (size_t)uw;
                    for (UINT x = 0; x < uw; x++) {
                        const uint32_t c = srow[x];
                        drow[x] = (c & 0xFF00FF00u) | ((c & 0x000000FFu) << 16) |
                                  ((c & 0x00FF0000u) >> 16);
                    }
                }
                GdipBitmapUnlockBits(bmp, &bd);
                GdipDisposeImage((GpImage *)bmp);
                *out_px = px;
                *out_w = (int)uw;
                *out_h = (int)uh;
                return 1;
            }
            GdipBitmapUnlockBits(bmp, &bd);
        }
    }
    GdipDisposeImage((GpImage *)bmp);
    return 0;
}

/* ---- PPSSPP textures.ini / 24-hex hash pack support --------------------- */

enum {
    GE_HD_HASH_QUICK = 0,
    GE_HD_HASH_XXH32 = 1,
    GE_HD_HASH_XXH64 = 2
};

typedef struct {
    uint64_t cachekey;
    uint32_t data_hash;
    int32_t  path_off; /* -1 = unused, -2 = explicitly ignored, >= 0 = pool offset */
} ge_hd_ini_slot;

typedef struct {
    uint32_t addr;
    int      from_w, from_h;
    int      to_w, to_h;
} ge_hd_hashrange;

typedef struct {
    int   w, h;
    float factor;
} ge_hd_reducerange;

typedef struct {
    uint64_t fnv_hash;
    int32_t  path_off; /* -1 = miss, >= 0 = pool offset */
    uint8_t  valid;
} ge_hd_fnv_entry;

#define GE_HD_FNV_CACHE_CAP 4096u

static CRITICAL_SECTION g_hd_ini_lock;
static int              g_hd_ini_lock_ready;
static int              g_hd_ini_parsed;
static int              g_hd_ini_active;
static int              g_hd_has_card_fronts;
#define GE_HD_CARD_FRONT_KEY 0x4D4F4B49u
static char             g_hd_base_dir[384];
static int              g_hd_hash_mode = GE_HD_HASH_QUICK;
static int              g_hd_ignore_address;
static int              g_hd_reduce_hash;
static float            g_hd_reduce_global = 0.5f;
static int              g_hd_skip_dxt1_128x64;
static int              g_hd_skip_dxt1_128x128;
static ge_hd_ini_slot  *g_hd_ini_table;
static uint32_t         g_hd_ini_cap;
static uint32_t         g_hd_ini_count;
static char            *g_hd_path_pool;
static size_t           g_hd_path_len;
static size_t           g_hd_path_cap;
static uint64_t         g_hd_dhash_bloom[256]; /* 16384-bit bloom on data_hash */
static int              g_hd_has_zero_dhash;
static ge_hd_hashrange  g_hd_hashranges[64];
static int              g_hd_nhashranges;
static ge_hd_reducerange g_hd_reduceranges[64];
static int              g_hd_nreduceranges;
static ge_hd_fnv_entry  g_hd_fnv_cache[GE_HD_FNV_CACHE_CAP];

static uint32_t ge_rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static uint64_t ge_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static uint32_t ge_read_le32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t ge_read_le64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint32_t ge_xxh32(const void *input, size_t len, uint32_t seed) {
    static const uint32_t P1 = 0x9E3779B1u;
    static const uint32_t P2 = 0x85EBCA77u;
    static const uint32_t P3 = 0xC2B2AE3Du;
    static const uint32_t P4 = 0x27D4EB2Fu;
    static const uint32_t P5 = 0x165667B1u;
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *b_end = p + len;
    uint32_t h32;

    if (len >= 16u) {
        const uint8_t *limit = b_end - 16u;
        uint32_t v1 = seed + P1 + P2;
        uint32_t v2 = seed + P2;
        uint32_t v3 = seed + 0u;
        uint32_t v4 = seed - P1;
        do {
            v1 = ge_rotl32(v1 + ge_read_le32(p + 0) * P2, 13) * P1;
            v2 = ge_rotl32(v2 + ge_read_le32(p + 4) * P2, 13) * P1;
            v3 = ge_rotl32(v3 + ge_read_le32(p + 8) * P2, 13) * P1;
            v4 = ge_rotl32(v4 + ge_read_le32(p + 12) * P2, 13) * P1;
            p += 16;
        } while (p <= limit);
        h32 = ge_rotl32(v1, 1) + ge_rotl32(v2, 7) + ge_rotl32(v3, 12) + ge_rotl32(v4, 18);
    } else {
        h32 = seed + P5;
    }

    h32 += (uint32_t)len;
    while (p + 4 <= b_end) {
        h32 += ge_read_le32(p) * P3;
        h32 = ge_rotl32(h32, 17) * P4;
        p += 4;
    }
    while (p < b_end) {
        h32 += (uint32_t)(*p++) * P5;
        h32 = ge_rotl32(h32, 11) * P1;
    }

    h32 ^= h32 >> 15;
    h32 *= P2;
    h32 ^= h32 >> 13;
    h32 *= P3;
    h32 ^= h32 >> 16;
    return h32;
}

static uint64_t ge_xxh64_round(uint64_t acc, uint64_t val) {
    static const uint64_t P1 = 11400714785074694791ULL;
    static const uint64_t P2 = 14029467366897019727ULL;
    acc += val * P2;
    acc = ge_rotl64(acc, 31);
    acc *= P1;
    return acc;
}

static uint64_t ge_xxh64_merge(uint64_t acc, uint64_t val) {
    static const uint64_t P1 = 11400714785074694791ULL;
    static const uint64_t P4 = 9650029242287828579ULL;
    val = ge_xxh64_round(0, val);
    acc ^= val;
    acc = acc * P1 + P4;
    return acc;
}

static uint64_t ge_xxh64(const void *input, size_t len, uint64_t seed) {
    static const uint64_t P1 = 11400714785074694791ULL;
    static const uint64_t P2 = 14029467366897019727ULL;
    static const uint64_t P3 = 1609587929392839161ULL;
    static const uint64_t P4 = 9650029242287828579ULL;
    static const uint64_t P5 = 2870177450012600261ULL;
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t *b_end = p + len;
    uint64_t h64;

    if (len >= 32u) {
        const uint8_t *limit = b_end - 32u;
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed + 0ULL;
        uint64_t v4 = seed - P1;
        do {
            v1 = ge_xxh64_round(v1, ge_read_le64(p + 0));
            v2 = ge_xxh64_round(v2, ge_read_le64(p + 8));
            v3 = ge_xxh64_round(v3, ge_read_le64(p + 16));
            v4 = ge_xxh64_round(v4, ge_read_le64(p + 24));
            p += 32;
        } while (p <= limit);
        h64 = ge_rotl64(v1, 1) + ge_rotl64(v2, 7) + ge_rotl64(v3, 12) + ge_rotl64(v4, 18);
        h64 = ge_xxh64_merge(h64, v1);
        h64 = ge_xxh64_merge(h64, v2);
        h64 = ge_xxh64_merge(h64, v3);
        h64 = ge_xxh64_merge(h64, v4);
    } else {
        h64 = seed + P5;
    }

    h64 += (uint64_t)len;
    while (p + 8 <= b_end) {
        uint64_t k1 = ge_xxh64_round(0, ge_read_le64(p));
        h64 ^= k1;
        h64 = ge_rotl64(h64, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= b_end) {
        h64 ^= (uint64_t)ge_read_le32(p) * P1;
        h64 = ge_rotl64(h64, 23) * P2 + P3;
        p += 4;
    }
    while (p < b_end) {
        h64 ^= (uint64_t)(*p++) * P5;
        h64 = ge_rotl64(h64, 11) * P1;
    }

    h64 ^= h64 >> 33;
    h64 *= P2;
    h64 ^= h64 >> 29;
    h64 *= P3;
    h64 ^= h64 >> 32;
    return h64;
}

static uint32_t ge_quick_tex_hash(const void *checkp, uint32_t size, int aligned16) {
    uint32_t check = 0;
    const uint8_t *bytes = (const uint8_t *)checkp;
    if (aligned16 && (size & 0x3fu) == 0u) {
        union {
            uint32_t x32[4];
            uint16_t x16[8];
        } cursor, cursor2, blk[4];
        static const uint16_t init_c2[8] = {
            0xc00bu, 0x9bd9u, 0x4b73u, 0xb651u, 0x4d9bu, 0x4309u, 0x0083u, 0x0001u
        };
        memset(&cursor, 0, sizeof cursor);
        for (int j = 0; j < 8; j++) cursor2.x16[j] = init_c2[j];

        const uint32_t n16 = size / 16u;
        for (uint32_t i = 0; i < n16; i += 4u) {
            memcpy(blk, bytes + (size_t)i * 16u, 64u);
            for (int j = 0; j < 8; j++)
                cursor.x16[j] = (uint16_t)(cursor.x16[j] + (uint16_t)(blk[0].x16[j] * cursor2.x16[j]));
            for (int j = 0; j < 4; j++) {
                cursor.x32[j] ^= blk[1].x32[j];
                cursor.x32[j] += blk[2].x32[j];
            }
            for (int j = 0; j < 8; j++)
                cursor.x16[j] ^= (uint16_t)(blk[3].x16[j] * cursor2.x16[j]);
            for (int j = 0; j < 8; j++)
                cursor2.x16[j] = (uint16_t)(cursor2.x16[j] + 0x2455u);
        }
        for (int j = 0; j < 4; j++)
            cursor.x32[j] += cursor2.x32[j];
        check = cursor.x32[0] + cursor.x32[1] + cursor.x32[2] + cursor.x32[3];
    } else {
        const uint32_t n8 = size / 8u;
        for (uint32_t i = 0; i < n8; i++) {
            check += ge_read_le32(bytes + (size_t)i * 8u);
            check ^= ge_read_le32(bytes + (size_t)i * 8u + 4u);
        }
    }
    return check;
}

static uint32_t ge_hd_slot_hash(uint64_t cachekey, uint32_t data_hash) {
    uint64_t k = cachekey ^ ((uint64_t)data_hash * 0x9E3779B97F4A7C15ULL);
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static int ge_hd_pool_append(const char *rel) {
    if (!rel || !rel[0]) return -2;
    const size_t n = strlen(rel) + 1u;
    if (g_hd_path_len + n > g_hd_path_cap) {
        size_t ncap = g_hd_path_cap ? g_hd_path_cap * 2u : 65536u;
        while (ncap < g_hd_path_len + n) ncap *= 2u;
        char *np = (char *)realloc(g_hd_path_pool, ncap);
        if (!np) return -1;
        g_hd_path_pool = np;
        g_hd_path_cap = ncap;
    }
    const int off = (int)g_hd_path_len;
    memcpy(g_hd_path_pool + g_hd_path_len, rel, n);
    g_hd_path_len += n;
    return off;
}

static void ge_hd_table_put(uint64_t cachekey, uint32_t data_hash, int32_t path_off) {
    if (g_hd_ini_cap == 0u || (g_hd_ini_count + 1u) * 2u > g_hd_ini_cap) {
        const uint32_t ncap = g_hd_ini_cap ? g_hd_ini_cap * 2u : 8192u;
        ge_hd_ini_slot *nt = (ge_hd_ini_slot *)malloc((size_t)ncap * sizeof *nt);
        if (!nt) return;
        for (uint32_t i = 0; i < ncap; i++) nt[i].path_off = -1;
        for (uint32_t i = 0; i < g_hd_ini_cap; i++) {
            if (g_hd_ini_table[i].path_off != -1) {
                uint32_t idx = ge_hd_slot_hash(g_hd_ini_table[i].cachekey,
                                               g_hd_ini_table[i].data_hash) & (ncap - 1u);
                while (nt[idx].path_off != -1) idx = (idx + 1u) & (ncap - 1u);
                nt[idx] = g_hd_ini_table[i];
            }
        }
        free(g_hd_ini_table);
        g_hd_ini_table = nt;
        g_hd_ini_cap = ncap;
    }
    uint32_t idx = ge_hd_slot_hash(cachekey, data_hash) & (g_hd_ini_cap - 1u);
    while (g_hd_ini_table[idx].path_off != -1) {
        if (g_hd_ini_table[idx].cachekey == cachekey &&
            g_hd_ini_table[idx].data_hash == data_hash) {
            g_hd_ini_table[idx].path_off = path_off;
            return;
        }
        idx = (idx + 1u) & (g_hd_ini_cap - 1u);
    }
    g_hd_ini_table[idx].cachekey = cachekey;
    g_hd_ini_table[idx].data_hash = data_hash;
    g_hd_ini_table[idx].path_off = path_off;
    g_hd_ini_count++;
    if (data_hash == 0u) {
        g_hd_has_zero_dhash = 1;
    } else {
        const uint32_t b1 = (data_hash * 0x9E3779B1u) >> 18;
        const uint32_t b2 = (data_hash * 0x85EBCA77u) >> 18;
        g_hd_dhash_bloom[b1 >> 6] |= (1ULL << (b1 & 63u));
        g_hd_dhash_bloom[b2 >> 6] |= (1ULL << (b2 & 63u));
    }
}

static int32_t ge_hd_table_get(uint64_t cachekey, uint32_t data_hash) {
    if (!g_hd_ini_table || g_hd_ini_cap == 0u) return -1;
    uint32_t idx = ge_hd_slot_hash(cachekey, data_hash) & (g_hd_ini_cap - 1u);
    while (g_hd_ini_table[idx].path_off != -1) {
        if (g_hd_ini_table[idx].cachekey == cachekey &&
            g_hd_ini_table[idx].data_hash == data_hash)
            return g_hd_ini_table[idx].path_off;
        idx = (idx + 1u) & (g_hd_ini_cap - 1u);
    }
    return -1;
}

static int ge_hd_dhash_maybe_present(uint32_t data_hash) {
    if (data_hash == 0u) return g_hd_has_zero_dhash;
    const uint32_t b1 = (data_hash * 0x9E3779B1u) >> 18;
    const uint32_t b2 = (data_hash * 0x85EBCA77u) >> 18;
    return ((g_hd_dhash_bloom[b1 >> 6] & (1ULL << (b1 & 63u))) != 0ULL) &&
           ((g_hd_dhash_bloom[b2 >> 6] & (1ULL << (b2 & 63u))) != 0ULL);
}

static int32_t ge_hd_lookup_wildcard(uint64_t cachekey, uint32_t hash) {
    const uint64_t eff_ck = g_hd_ignore_address ? (cachekey & 0xFFFFFFFFULL) : cachekey;
    int32_t r = ge_hd_table_get(eff_ck, hash);
    if (r != -1) return r;

    r = ge_hd_table_get(cachekey & 0xFFFFFFFFULL, 0u);
    if (r != -1) return r;

    if (!g_hd_ignore_address) {
        r = ge_hd_table_get(cachekey, 0u);
        if (r != -1) return r;
    }

    r = ge_hd_table_get(cachekey & 0xFFFFFFFFULL, hash);
    if (r != -1) return r;

    if (!g_hd_ignore_address) {
        r = ge_hd_table_get(cachekey & ~0xFFFFFFFFULL, hash);
        if (r != -1) return r;
        r = ge_hd_table_get(cachekey & ~0xFFFFFFFFULL, 0u);
        if (r != -1) return r;
    }

    return ge_hd_table_get(0ULL, hash);
}

static char *ge_trim_str(char *s) {
    while (*s && (unsigned char)*s <= ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && (unsigned char)s[len - 1u] <= ' ') s[--len] = '\0';
    return s;
}

static int ge_parse_hex24(const char *s, uint64_t *out_ck, uint32_t *out_dh) {
    if (!s || strlen(s) != 24u) return 0;
    for (int i = 0; i < 24; i++) {
        if (!isxdigit((unsigned char)s[i])) return 0;
    }
    char ck_str[17], dh_str[9];
    memcpy(ck_str, s, 16);
    ck_str[16] = '\0';
    memcpy(dh_str, s + 16, 8);
    dh_str[8] = '\0';
    *out_ck = strtoull(ck_str, NULL, 16);
    *out_dh = (uint32_t)strtoul(dh_str, NULL, 16);
    return 1;
}

static void ge_hd_ini_reset(void) {
    free(g_hd_ini_table);
    g_hd_ini_table = NULL;
    g_hd_ini_cap = 0;
    g_hd_ini_count = 0;
    free(g_hd_path_pool);
    g_hd_path_pool = NULL;
    g_hd_path_len = 0;
    g_hd_path_cap = 0;
    memset(g_hd_dhash_bloom, 0, sizeof g_hd_dhash_bloom);
    g_hd_has_zero_dhash = 0;
    g_hd_nhashranges = 0;
    g_hd_nreduceranges = 0;
    memset(g_hd_fnv_cache, 0, sizeof g_hd_fnv_cache);
    g_hd_ini_parsed = 0;
    g_hd_ini_active = 0;
    g_hd_has_card_fronts = 0;
    g_hd_base_dir[0] = '\0';
}

static void ge_hd_ini_load_once(const char *env_dir) {
    if (g_hd_ini_parsed) return;
    g_hd_ini_parsed = 1;
    if (!env_dir || !env_dir[0]) return;

    snprintf(g_hd_base_dir, sizeof g_hd_base_dir, "%s", env_dir);
    char ini_path[440];
    snprintf(ini_path, sizeof ini_path, "%s/textures.ini", g_hd_base_dir);
    FILE *f = fopen(ini_path, "rb");
    if (!f) {
        snprintf(ini_path, sizeof ini_path, "%s/ULUS10136/textures.ini", env_dir);
        f = fopen(ini_path, "rb");
        if (f)
            snprintf(g_hd_base_dir, sizeof g_hd_base_dir, "%s/ULUS10136", env_dir);
    }

    if (f) {
        g_hd_hash_mode = GE_HD_HASH_QUICK;
        g_hd_ignore_address = 0;
        g_hd_reduce_hash = 0;
        g_hd_reduce_global = 0.5f;
        g_hd_skip_dxt1_128x64 = 0;
        g_hd_skip_dxt1_128x128 = 0;

        char section[32] = "";
        char line[1024];
        int first_line = 1;
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            if (first_line) {
                first_line = 0;
                if ((uint8_t)p[0] == 0xEFu && (uint8_t)p[1] == 0xBBu && (uint8_t)p[2] == 0xBFu)
                    p += 3;
            }
            char *hash_comment = strchr(p, '#');
            if (hash_comment) *hash_comment = '\0';
            p = ge_trim_str(p);
            if (!p[0] || p[0] == ';') continue;

            if (p[0] == '[') {
                char *endb = strchr(p, ']');
                if (endb) {
                    *endb = '\0';
                    snprintf(section, sizeof section, "%s", ge_trim_str(p + 1));
                    for (int i = 0; section[i]; i++)
                        section[i] = (char)tolower((unsigned char)section[i]);
                }
                continue;
            }

            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = ge_trim_str(p);
            char *v = ge_trim_str(eq + 1);

            if (strcmp(section, "options") == 0) {
                if (_stricmp(k, "hash") == 0) {
                    if (_stricmp(v, "xxh32") == 0) g_hd_hash_mode = GE_HD_HASH_XXH32;
                    else if (_stricmp(v, "xxh64") == 0) g_hd_hash_mode = GE_HD_HASH_XXH64;
                    else g_hd_hash_mode = GE_HD_HASH_QUICK;
                } else if (_stricmp(k, "ignoreAddress") == 0) {
                    g_hd_ignore_address = (_stricmp(v, "true") == 0 || strcmp(v, "1") == 0);
                } else if (_stricmp(k, "reduceHash") == 0) {
                    g_hd_reduce_hash = (_stricmp(v, "true") == 0 || strcmp(v, "1") == 0);
                } else if (_stricmp(k, "reduceHashGlobalValue") == 0) {
                    g_hd_reduce_global = (float)atof(v);
                } else if (_stricmp(k, "skipLastDXT1Blocks128x64") == 0) {
                    g_hd_skip_dxt1_128x64 = atoi(v);
                } else if (_stricmp(k, "skipLastDXT1Blocks128x128") == 0) {
                    g_hd_skip_dxt1_128x128 = atoi(v);
                }
            } else if (strcmp(section, "hashes") == 0) {
                uint64_t ck = 0;
                uint32_t dh = 0;
                if (ge_parse_hex24(k, &ck, &dh)) {
                    char *pipe = strchr(v, '|');
                    if (pipe) {
                        *pipe = '\0';
                        v = ge_trim_str(v);
                    }
                    int32_t poff = v[0] ? ge_hd_pool_append(v) : -2;
                    if (poff != -1)
                        ge_hd_table_put(ck, dh, poff);
                    if (ck == GE_HD_CARD_FRONT_KEY && poff >= 0)
                        g_hd_has_card_fronts = 1;
                }
            } else if (strcmp(section, "hashranges") == 0 && g_hd_nhashranges < 64) {
                unsigned int addr = 0;
                int fw = 0, fh = 0, tw = 0, th = 0;
                if (sscanf(k, "%x,%d,%d", &addr, &fw, &fh) == 3 &&
                    sscanf(v, "%d,%d", &tw, &th) == 2) {
                    g_hd_hashranges[g_hd_nhashranges].addr = addr;
                    g_hd_hashranges[g_hd_nhashranges].from_w = fw;
                    g_hd_hashranges[g_hd_nhashranges].from_h = fh;
                    g_hd_hashranges[g_hd_nhashranges].to_w = tw;
                    g_hd_hashranges[g_hd_nhashranges].to_h = th;
                    g_hd_nhashranges++;
                }
            } else if (strcmp(section, "reducehashranges") == 0 && g_hd_nreduceranges < 64) {
                int rw = 0, rh = 0;
                float rf = 0.5f;
                if (sscanf(k, "%d,%d", &rw, &rh) == 2 && sscanf(v, "%f", &rf) == 1) {
                    g_hd_reduceranges[g_hd_nreduceranges].w = rw;
                    g_hd_reduceranges[g_hd_nreduceranges].h = rh;
                    g_hd_reduceranges[g_hd_nreduceranges].factor = rf;
                    g_hd_nreduceranges++;
                }
            }
        }
        fclose(f);
        if (g_hd_hash_mode == GE_HD_HASH_QUICK)
            g_hd_reduce_hash = 0;
    }

    /* Also scan g_hd_base_dir for direct <24-hex>.png filenames. */
    char pattern[440];
    snprintf(pattern, sizeof pattern, "%s/*.png", g_hd_base_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hfind = FindFirstFileA(pattern, &fd);
    if (hfind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            size_t flen = strlen(fd.cFileName);
            if (flen == 28u && _stricmp(fd.cFileName + 24, ".png") == 0) {
                char hex24[25];
                memcpy(hex24, fd.cFileName, 24);
                hex24[24] = '\0';
                uint64_t ck = 0;
                uint32_t dh = 0;
                if (ge_parse_hex24(hex24, &ck, &dh) && ge_hd_table_get(ck, dh) == -1) {
                    int32_t poff = ge_hd_pool_append(fd.cFileName);
                    if (poff >= 0) ge_hd_table_put(ck, dh, poff);
                }
            }
        } while (FindNextFileA(hfind, &fd));
        FindClose(hfind);
    }

    if (g_hd_ini_count > 0u) {
        g_hd_ini_active = 1;
        fprintf(stderr, "gl: loaded %u HD texture mappings from %s (mode=%s, ignoreAddress=%d)\n",
                g_hd_ini_count, g_hd_base_dir,
                g_hd_hash_mode == GE_HD_HASH_XXH32 ? "xxh32" :
                (g_hd_hash_mode == GE_HD_HASH_XXH64 ? "xxh64" : "quick"),
                g_hd_ignore_address);
    }
}

static uint32_t ge_compute_ppsspp_data_hash(const psp_draw_state *st, int h_override,
                                            int force_aligned, int skip_dxt1_blocks) {
    static const uint32_t TEX_BPP[16] = {
        16u, 16u, 16u, 32u, 4u, 8u, 16u, 32u, 4u, 8u, 8u, 0u, 0u, 0u, 0u, 0u
    };
    static const uint32_t ALIGN_MASK16[16] = {
        0x7F8u, 0x7F8u, 0x7F8u, 0x7FCu, 0x7E0u, 0x7F0u, 0x7F8u, 0x7FCu,
        0x7FFu, 0x7FFu, 0x7FFu, 0u, 0u, 0u, 0u, 0u
    };
    if (!st || st->tex_fmt < 0 || st->tex_fmt > PSP_TPSM_DXT5) return 0u;
    const uint32_t bpp = TEX_BPP[st->tex_fmt];
    if (bpp == 0u) return 0u;

    uint32_t bufw = (uint32_t)st->tex_bw & ALIGN_MASK16[st->tex_fmt];
    if (bufw == 0u) bufw = (8u * 16u) / bpp;

    int w = st->tex_w;
    int h = h_override > 0 ? h_override : st->tex_h;
    for (int i = 0; i < g_hd_nhashranges; i++) {
        if (g_hd_hashranges[i].addr == st->tex_addr &&
            g_hd_hashranges[i].from_w == st->tex_w &&
            g_hd_hashranges[i].from_h == st->tex_h) {
            w = g_hd_hashranges[i].to_w;
            h = g_hd_hashranges[i].to_h;
            break;
        }
    }
    if (w <= 0 || h <= 0) return 0u;

    float reduce_factor = 1.0f;
    if (g_hd_reduce_hash) {
        reduce_factor = g_hd_reduce_global;
        for (int i = 0; i < g_hd_nreduceranges; i++) {
            if (g_hd_reduceranges[i].w == w && g_hd_reduceranges[i].h == h) {
                reduce_factor = g_hd_reduceranges[i].factor;
                break;
            }
        }
        if (reduce_factor < 0.0f) reduce_factor = 0.0f;
        if (reduce_factor > 1.0f) reduce_factor = 1.0f;
    }

    const int aligned16 = force_aligned || ((st->tex_addr & 0xFu) == 0u);
    if ((int)bufw <= w) {
        const uint32_t total_px = bufw * (uint32_t)h + (uint32_t)(w - (int)bufw);
        uint32_t size_ram = (uint32_t)((float)((bpp * total_px) / 8u) * reduce_factor);
        if (skip_dxt1_blocks > 0 && (uint32_t)skip_dxt1_blocks < size_ram / 8u)
            size_ram -= (uint32_t)skip_dxt1_blocks * 8u;
        if (size_ram == 0u) return 0u;
        const uint8_t *checkp = (const uint8_t *)psp_mem_unwatched_ptr(st->tex_addr, size_ram);
        if (!checkp) return 0u;
        if (g_hd_hash_mode == GE_HD_HASH_XXH32)
            return ge_xxh32(checkp, size_ram, 0xBACD7814u);
        if (g_hd_hash_mode == GE_HD_HASH_XXH64)
            return (uint32_t)ge_xxh64(checkp, size_ram, 0xBACD7814u);
        return ge_quick_tex_hash(checkp, size_ram, aligned16);
    } else {
        const uint32_t bytes_per_line = (uint32_t)((float)((bpp * (uint32_t)w) / 8u) * reduce_factor);
        const uint32_t stride = (bpp * bufw) / 8u;
        const uint32_t size_ram = (uint32_t)(h - 1) * stride + bytes_per_line;
        if (size_ram == 0u) return 0u;
        const uint8_t *checkp = (const uint8_t *)psp_mem_unwatched_ptr(st->tex_addr, size_ram);
        if (!checkp) return 0u;
        uint32_t result = 0u;
        for (int y = 0; y < h; y++) {
            uint32_t row_hash;
            if (g_hd_hash_mode == GE_HD_HASH_XXH32)
                row_hash = ge_xxh32(checkp, bytes_per_line, 0xBACD7814u);
            else if (g_hd_hash_mode == GE_HD_HASH_XXH64)
                row_hash = (uint32_t)ge_xxh64(checkp, bytes_per_line, 0xBACD7814u);
            else
                row_hash = ge_quick_tex_hash(checkp, bytes_per_line,
                                             aligned16 && ((stride & 0xFu) == 0u));
            result = (result * 11u) ^ row_hash;
            checkp += stride;
        }
        return result;
    }
}

static int ge_log2_dim(int v) {
    int l = 0;
    while ((1 << l) < v && l < 15) l++;
    return l;
}

static int32_t ge_hd_match_state_locked(const psp_draw_state *st) {
    if (!g_hd_ini_active || !st || st->tex_w <= 0 || st->tex_h <= 0) return -1;
    const uint16_t dim = (uint16_t)((ge_log2_dim(st->tex_h) << 8) | ge_log2_dim(st->tex_w));
    const uint64_t addr_hi = ((uint64_t)(st->tex_addr & 0x3FFFFFFFu)) << 32;
    const int has_clut = (st->tex_fmt & 4) != 0;
    int skip_dxt1_blocks = 0;
    if (st->tex_fmt == PSP_TPSM_DXT1 && st->tex_w == 128) {
        if (st->tex_h == 64) skip_dxt1_blocks = g_hd_skip_dxt1_128x64;
        if (st->tex_h == 128) skip_dxt1_blocks = g_hd_skip_dxt1_128x128;
    }

    const int h_candidates[3] = { st->tex_h, 272, 256 };
    const int n_hcand = (st->tex_h == 512) ? 3 : 1;

    for (int hi = 0; hi < n_hcand; hi++) {
        for (int align_try = 0; align_try < 2; align_try++) {
            if (align_try == 1 && (st->tex_addr & 0xFu) == 0u) break;
            const uint32_t dhash = ge_compute_ppsspp_data_hash(st, h_candidates[hi], align_try, 0);
            if (!has_clut) {
                const uint64_t ck = addr_hi | (uint64_t)dim;
                const int32_t poff = ge_hd_lookup_wildcard(ck, dhash);
                if (poff != -1) return poff;
                if (skip_dxt1_blocks > 0) {
                    const uint32_t cropped = ge_compute_ppsspp_data_hash(
                        st, h_candidates[hi], align_try, skip_dxt1_blocks);
                    const int32_t fallback = ge_hd_lookup_wildcard(ck, cropped);
                    if (fallback != -1) return fallback;
                }
                if (hi == 0 && align_try == 0) {
                    static int n_trace_miss;
                    const char *tr = ge_trace_hd_tex_env();
                    if (tr && tr[0] && n_trace_miss < 64) {
                        n_trace_miss++;
                        fprintf(stderr, "gl: HD miss %016llx%08x (addr=%08X fmt=%d %dx%d bw=%d swz=%d)\n",
                                (unsigned long long)ck, dhash, st->tex_addr, st->tex_fmt,
                                st->tex_w, st->tex_h, st->tex_bw, st->tex_swizzle);
                    }
                }
            } else if (st->clut) {
                const uint32_t cmode32 = 0xC5000000u |
                    ((uint32_t)(st->clut_start & 0x1F) << 16) |
                    ((uint32_t)(st->clut_mask & 0xFF) << 8) |
                    ((uint32_t)(st->clut_shift & 0x1F) << 2) |
                    (uint32_t)(st->clut_fmt & 3);
                const uint32_t entry_sz = (st->clut_fmt == 3) ? 4u : 2u;
                const uint32_t base_b = (uint32_t)(st->clut_start & 0x1F) * 16u * entry_sz;
                const uint32_t def_load = (st->tex_fmt == PSP_TPSM_T4) ? (16u * entry_sz) : (256u * entry_sz);
                uint32_t def_ext = base_b + def_load;
                if (def_ext > 1024u) def_ext = 1024u;
                if (def_ext < 32u) def_ext = 32u;

                const uint32_t ch_def = ge_xxh32(st->clut, def_ext, 0xC0108888u) ^ cmode32;
                int32_t poff = ge_hd_lookup_wildcard(addr_hi | (uint64_t)((uint32_t)dim ^ ch_def), dhash);
                if (poff != -1) return poff;

                if (ge_hd_dhash_maybe_present(dhash) || g_hd_has_zero_dhash) {
                    for (uint32_t ext = 32u; ext <= 1024u; ext += 32u) {
                        if (ext == def_ext) continue;
                        const uint32_t ch = ge_xxh32(st->clut, ext, 0xC0108888u) ^ cmode32;
                        poff = ge_hd_lookup_wildcard(addr_hi | (uint64_t)((uint32_t)dim ^ ch), dhash);
                        if (poff != -1) return poff;
                    }
                }
            }
        }
    }
    return -1;
}

static int32_t ge_hd_resolve_texture(uint64_t fnv_hash, const psp_draw_state *st) {
    const uint64_t t0 = seto_perf_now_us();
    const char *dir = ge_hd_tex_dir_env();
    if (!dir || !dir[0] || !fnv_hash || !st || !g_hd_ini_lock_ready) return -1;

    int32_t path_off = -1;
    EnterCriticalSection(&g_hd_ini_lock);
    ge_hd_ini_load_once(dir);
    if (g_hd_ini_active) {
        const uint32_t slot = (uint32_t)(fnv_hash ^ (fnv_hash >> 32)) & (GE_HD_FNV_CACHE_CAP - 1u);
        if (!g_hd_fnv_cache[slot].valid || g_hd_fnv_cache[slot].fnv_hash != fnv_hash) {
            g_hd_fnv_cache[slot].fnv_hash = fnv_hash;
            g_hd_fnv_cache[slot].path_off = ge_hd_match_state_locked(st);
            g_hd_fnv_cache[slot].valid = 1;
        }
        path_off = g_hd_fnv_cache[slot].path_off;
    }
    LeaveCriticalSection(&g_hd_ini_lock);
    seto_perf_add(&g_seto_perf.p_hd_us, seto_perf_now_us() - t0);
    return path_off;
}

int32_t seto_ge_card_front_path(uint32_t texture_addr) {
    if (!g_ge_draw_mode || !seto_gl_present_active() || !g_hd_ini_lock_ready)
        return -1;

    int32_t path = -1;
    EnterCriticalSection(&g_hd_ini_lock);
    ge_hd_ini_load_once(ge_hd_tex_dir_env());
    if (g_hd_has_card_fronts) {
        const size_t bytes = 4096 - 128;
        const void *data = psp_mem_unwatched_ptr(texture_addr, bytes);
        if (data)
            path = ge_hd_table_get(GE_HD_CARD_FRONT_KEY,
                                   (uint32_t)ge_xxh64(data, bytes, 0xBACD7814u));
    }
    LeaveCriticalSection(&g_hd_ini_lock);
    return path;
}

#define GE_HD_ASYNC_JOBS 64
enum {
    GE_HD_JOB_EMPTY    = 0,
    GE_HD_JOB_PENDING  = 1,
    GE_HD_JOB_READY    = 2,
    GE_HD_JOB_FAILED   = 3,
    GE_HD_JOB_DECODING = 4
};
typedef struct {
    char     path[640];
    int      state;
    uint64_t seq;
    uint32_t *pixels;
    int      w;
    int      h;
} ge_hd_async_job;

static ge_hd_async_job g_hd_async_jobs[GE_HD_ASYNC_JOBS];
static uint64_t g_hd_async_seq;
static CRITICAL_SECTION g_hd_async_lock;
static CONDITION_VARIABLE g_hd_async_cv;
static HANDLE g_hd_worker_thread;
static int g_hd_async_init;
static int g_hd_async_stop;

static DWORD WINAPI ge_hd_worker_proc(LPVOID arg) {
    (void)arg;
    for (;;) {
        char path[640];
        int job_idx = -1;
        EnterCriticalSection(&g_hd_async_lock);
        while (!g_hd_async_stop) {
            for (int i = 0; i < GE_HD_ASYNC_JOBS; i++) {
                if (g_hd_async_jobs[i].state == GE_HD_JOB_PENDING) {
                    job_idx = i;
                    snprintf(path, sizeof path, "%s", g_hd_async_jobs[i].path);
                    g_hd_async_jobs[i].state = GE_HD_JOB_DECODING;
                    break;
                }
            }
            if (job_idx >= 0) break;
            SleepConditionVariableCS(&g_hd_async_cv, &g_hd_async_lock, INFINITE);
        }
        if (g_hd_async_stop) {
            LeaveCriticalSection(&g_hd_async_lock);
            return 0;
        }
        LeaveCriticalSection(&g_hd_async_lock);

        uint32_t *pixels = NULL;
        int w = 0, h = 0;
        const int ok = ge_load_png_rgba(path, &pixels, &w, &h) ||
                       ge_load_ppm_rgba(path, &pixels, &w, &h);

        EnterCriticalSection(&g_hd_async_lock);
        if (g_hd_async_stop) {
            free(pixels);
            g_hd_async_jobs[job_idx].pixels = NULL;
            g_hd_async_jobs[job_idx].state = GE_HD_JOB_EMPTY;
        } else if (ok && pixels) {
            g_hd_async_jobs[job_idx].pixels = pixels;
            g_hd_async_jobs[job_idx].w = w;
            g_hd_async_jobs[job_idx].h = h;
            g_hd_async_jobs[job_idx].state = GE_HD_JOB_READY;
        } else {
            free(pixels);
            g_hd_async_jobs[job_idx].pixels = NULL;
            g_hd_async_jobs[job_idx].state = GE_HD_JOB_FAILED;
        }
        LeaveCriticalSection(&g_hd_async_lock);
    }
}

static void ge_hd_async_ensure_started(void) {
    if (g_hd_async_init) return;
    ge_gdiplus_start_once();
    InitializeCriticalSection(&g_hd_async_lock);
    InitializeConditionVariable(&g_hd_async_cv);
    memset(g_hd_async_jobs, 0, sizeof g_hd_async_jobs);
    g_hd_async_stop = 0;
    g_hd_worker_thread = CreateThread(NULL, 0, ge_hd_worker_proc, NULL, 0, NULL);
    g_hd_async_init = 1;
}

static void ge_hd_async_shutdown(void) {
    if (!g_hd_async_init) return;
    EnterCriticalSection(&g_hd_async_lock);
    g_hd_async_stop = 1;
    WakeAllConditionVariable(&g_hd_async_cv);
    LeaveCriticalSection(&g_hd_async_lock);
    if (g_hd_worker_thread) {
        WaitForSingleObject(g_hd_worker_thread, 5000);
        CloseHandle(g_hd_worker_thread);
        g_hd_worker_thread = NULL;
    }
    for (int i = 0; i < GE_HD_ASYNC_JOBS; i++) {
        free(g_hd_async_jobs[i].pixels);
        g_hd_async_jobs[i].pixels = NULL;
        g_hd_async_jobs[i].state = GE_HD_JOB_EMPTY;
    }
    DeleteCriticalSection(&g_hd_async_lock);
    g_hd_async_init = 0;
}

static void ge_release_hd_texture(ge_hd_texture *hd) {
    if (hd && --hd->refs == 0 && !hd->retain) {
        if (g_gl_sc.bound_tex == hd->texture) g_gl_sc.bound_tex = 0;
        glDeleteTextures(1, &hd->texture);
        hd->texture = 0;
    }
}

static int ge_hd_file_exists(const char *path) {
    WCHAR wpath[520];
    if (!path || !path[0]) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 520) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 520) <= 0)
        return 0;
    const DWORD attr = GetFileAttributesW(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static ge_hd_texture *ge_get_hd_texture_file(const char *path, int allow_async, int *out_pending) {
    if (out_pending) *out_pending = 0;
    ge_hd_texture *victim = NULL;
    for (size_t i = 0; i < sizeof g_ge_hd_cache / sizeof g_ge_hd_cache[0]; i++) {
        ge_hd_texture *entry = &g_ge_hd_cache[i];
        if (entry->texture && strcmp(entry->path, path) == 0) {
            entry->used_at = ++g_ge_hd_clock;
            entry->refs++;
            seto_perf_add(&g_seto_perf.hd_hits, 1);
            return entry;
        }
        if (!entry->refs && (!victim || !entry->texture ||
            (victim->texture && entry->used_at < victim->used_at)))
            victim = entry;
    }
    if (!victim) return NULL;

    const uint64_t t0 = seto_perf_now_us();
    uint32_t *pixels = NULL;
    int w = 0, h = 0;

    if (allow_async) {
        ge_hd_async_ensure_started();
    }
    if (allow_async && g_hd_worker_thread) {
        EnterCriticalSection(&g_hd_async_lock);
        int found = -1;
        int empty = -1;
        int evict_failed = -1;
        int evict_ready = -1;
        for (int i = 0; i < GE_HD_ASYNC_JOBS; i++) {
            const int st = g_hd_async_jobs[i].state;
            if (st == GE_HD_JOB_EMPTY) {
                if (empty < 0) empty = i;
            } else {
                if (strcmp(g_hd_async_jobs[i].path, path) == 0) {
                    found = i;
                    break;
                }
                if (st == GE_HD_JOB_FAILED &&
                    (evict_failed < 0 || g_hd_async_jobs[i].seq < g_hd_async_jobs[evict_failed].seq)) {
                    evict_failed = i;
                } else if (st == GE_HD_JOB_READY &&
                           (evict_ready < 0 || g_hd_async_jobs[i].seq < g_hd_async_jobs[evict_ready].seq)) {
                    evict_ready = i;
                }
            }
        }
        if (found >= 0) {
            ge_hd_async_job *job = &g_hd_async_jobs[found];
            if (job->state == GE_HD_JOB_PENDING || job->state == GE_HD_JOB_DECODING) {
                if (out_pending) *out_pending = 1;
                LeaveCriticalSection(&g_hd_async_lock);
                return NULL;
            }
            if (job->state == GE_HD_JOB_FAILED) {
                job->state = GE_HD_JOB_EMPTY;
                LeaveCriticalSection(&g_hd_async_lock);
                seto_perf_add(&g_seto_perf.hd_miss_io, 1);
                seto_perf_add(&g_seto_perf.hd_miss_us, seto_perf_now_us() - t0);
                return NULL;
            }
            if (job->state == GE_HD_JOB_READY) {
                pixels = job->pixels;
                w = job->w;
                h = job->h;
                job->pixels = NULL;
                job->state = GE_HD_JOB_EMPTY;
                LeaveCriticalSection(&g_hd_async_lock);
            } else {
                LeaveCriticalSection(&g_hd_async_lock);
                return NULL;
            }
        } else {
            LeaveCriticalSection(&g_hd_async_lock);
            if (!ge_hd_file_exists(path)) {
                seto_perf_add(&g_seto_perf.hd_miss_io, 1);
                seto_perf_add(&g_seto_perf.hd_miss_us, seto_perf_now_us() - t0);
                return NULL;
            }
            EnterCriticalSection(&g_hd_async_lock);
            /* Re-scan empty/evict slot in case another state changed while unlocked. */
            empty = -1;
            evict_failed = -1;
            evict_ready = -1;
            for (int i = 0; i < GE_HD_ASYNC_JOBS; i++) {
                const int st = g_hd_async_jobs[i].state;
                if (st == GE_HD_JOB_EMPTY) {
                    if (empty < 0) empty = i;
                } else if (st == GE_HD_JOB_FAILED &&
                           (evict_failed < 0 || g_hd_async_jobs[i].seq < g_hd_async_jobs[evict_failed].seq)) {
                    evict_failed = i;
                } else if (st == GE_HD_JOB_READY &&
                           (evict_ready < 0 || g_hd_async_jobs[i].seq < g_hd_async_jobs[evict_ready].seq)) {
                    evict_ready = i;
                }
            }
            int dst = empty >= 0 ? empty : (evict_failed >= 0 ? evict_failed : evict_ready);
            if (dst >= 0) {
                free(g_hd_async_jobs[dst].pixels);
                snprintf(g_hd_async_jobs[dst].path, sizeof g_hd_async_jobs[dst].path, "%s", path);
                g_hd_async_jobs[dst].pixels = NULL;
                g_hd_async_jobs[dst].seq = ++g_hd_async_seq;
                g_hd_async_jobs[dst].state = GE_HD_JOB_PENDING;
                WakeConditionVariable(&g_hd_async_cv);
                if (out_pending) *out_pending = 1;
                LeaveCriticalSection(&g_hd_async_lock);
                return NULL;
            }
            LeaveCriticalSection(&g_hd_async_lock);
            /* Fallback to synchronous load if all 64 worker slots are actively decoding. */
            if (!ge_load_png_rgba(path, &pixels, &w, &h) &&
                !ge_load_ppm_rgba(path, &pixels, &w, &h)) {
                seto_perf_add(&g_seto_perf.hd_miss_io, 1);
                seto_perf_add(&g_seto_perf.hd_miss_us, seto_perf_now_us() - t0);
                return NULL;
            }
        }
    } else {
        if (!ge_load_png_rgba(path, &pixels, &w, &h) &&
            !ge_load_ppm_rgba(path, &pixels, &w, &h)) {
            seto_perf_add(&g_seto_perf.hd_miss_io, 1);
            seto_perf_add(&g_seto_perf.hd_miss_us, seto_perf_now_us() - t0);
            return NULL;
        }
    }

    if (!victim->texture) glGenTextures(1, &victim->texture);
    if (!victim->texture) {
        free(pixels);
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, victim->texture);
    g_gl_sc.bound_tex = victim->texture;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    seto_perf_add(&g_seto_perf.hd_loads, 1);
    seto_perf_add(&g_seto_perf.hd_load_us, seto_perf_now_us() - t0);
    victim->params.valid = 0;
    victim->refs = 1;
    victim->used_at = ++g_ge_hd_clock;
    // Keep large interface atlases and card fronts resident across slot rotations.
    victim->retain = (size_t)w * h * sizeof(uint32_t) <= 16u * 1024u * 1024u ||
                     (w == 813 && h == 1185 && strstr(path, "/moki-tcg/"));
    snprintf(victim->path, sizeof victim->path, "%s", path);
    if (g_hd_logged < 16) {
        g_hd_logged++;
        fprintf(stderr, "gl: HD texture %s (%dx%d)\n", path, w, h);
    }
    return victim;
}

static ge_hd_texture *ge_get_hd_texture(uint64_t hash, int32_t path_off,
                                        int allow_async, int *out_pending) {
    if (out_pending) *out_pending = 0;
    const char *dir = ge_hd_tex_dir_env();
    if (!dir || !dir[0] || !hash || path_off == -2) return NULL;

    char ini_rel[260] = "";
    char base_dir[384] = "";
    if (g_hd_ini_lock_ready) {
        EnterCriticalSection(&g_hd_ini_lock);
        ge_hd_ini_load_once(dir);
        snprintf(base_dir, sizeof base_dir, "%s", g_hd_base_dir[0] ? g_hd_base_dir : dir);
        if (path_off >= 0 && (size_t)path_off < g_hd_path_len)
            snprintf(ini_rel, sizeof ini_rel, "%s", g_hd_path_pool + path_off);
        LeaveCriticalSection(&g_hd_ini_lock);
    } else {
        snprintf(base_dir, sizeof base_dir, "%s", dir);
    }

    char path[640];
    int pending = 0;
    if (ini_rel[0]) {
        int p = 0;
        snprintf(path, sizeof path, "%s/%s", base_dir, ini_rel);
        ge_hd_texture *hd = ge_get_hd_texture_file(path, allow_async, &p);
        if (hd) return hd;
        if (p) {
            if (out_pending) *out_pending = 1;
            return NULL;
        }
    }
    for (int i = 0; i < 512; i++) {
        if (g_hd_miss_hashes[i] == hash) return NULL;
    }
    {
        int p = 0;
        snprintf(path, sizeof path, "%s/%016llx.png", base_dir, (unsigned long long)hash);
        ge_hd_texture *hd = ge_get_hd_texture_file(path, allow_async, &p);
        if (hd) return hd;
        pending |= p;
    }
    {
        int p = 0;
        snprintf(path, sizeof path, "%s/%016llx.ppm", base_dir, (unsigned long long)hash);
        ge_hd_texture *hd = ge_get_hd_texture_file(path, allow_async, &p);
        if (hd) return hd;
        pending |= p;
    }

    if (pending) {
        if (out_pending) *out_pending = 1;
        return NULL;
    }
    g_hd_miss_hashes[g_hd_miss_cursor & 511] = hash;
    g_hd_miss_cursor++;
    return NULL;
}

static void ge_sync_stencil_to_alpha(ge_gpu_fbo *tf) {
    if (!tf || !tf->used || !tf->stencil_alpha_dirty || tf->fmt == 0) return;
    ge_resolve_fbo_ex(tf, 1);
    ge_gl_state_invalidate();
    glDisable(GL_MULTISAMPLE);
    p_glBindFramebuffer(GL_FRAMEBUFFER, tf->fbo);
    glViewport(0, 0, tf->phys_w, tf->phys_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0u);
    glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD);
    p_glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ONE);

    p_glUseProgram(g_ge_geo_prog);
    p_glUniform2f(g_ge_geo_size, (GLfloat)tf->w, (GLfloat)tf->h);
    p_glUniform1f(g_ge_geo_half_px, 0.0f);
    p_glUniform1i(g_ge_geo_clear, 1);
    p_glUniform1i(g_ge_geo_zclamp_vert, 1);
    p_glUniform1i(g_ge_geo_use_subpixel, 0);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_ge_geo_vbo);
    p_glBindVertexArray(g_ge_geo_vao);

    psp_vertex q[6];
    memset(q, 0, sizeof q);
    q[1].x = tf->w; q[2].y = tf->h;
    q[3].x = tf->w; q[4].y = tf->h;
    q[5].x = tf->w; q[5].y = tf->h;

    const int start_bit = tf->fmt == 1 ? 7 : (tf->fmt == 2 ? 4 : 0);
    for (int bit = start_bit; bit <= 7; bit++) {
        const GLuint m = 1u << (unsigned)bit;
        const GLuint a = tf->fmt == 1 ? 0xFFu : (tf->fmt == 2 ? (m | (m >> 4)) : m);
        const uint32_t col = (uint32_t)(a << 24);
        for (int k = 0; k < 6; k++) q[k].rgba = col;
        glStencilFunc(GL_EQUAL, (GLint)m, m);
        const GLint base = ge_upload_geo_verts(q, 6u);
        glDrawArrays(GL_TRIANGLES, base, 6);
    }

    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    tf->stencil_alpha_dirty = 0;
    if (tf->samples >= 2 && tf->msaa_fbo) tf->ss_newer = 1;
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    g_geo_uc.valid = 0;
}

static void ge_process_draw_verts(const ge_cmd *c, const psp_vertex *verts, uint32_t count) {
    GLenum mode;
    GLfloat half_px = 0.0f;
    if (!c || !verts || count == 0u) {
        if (c && c->tex) free(c->tex);
        return;
    }
    if (c->fbo_slot < 0 || c->fbo_slot >= GE_MAX_FBOS ||
        !g_ge_fbos[c->fbo_slot].used) {
        free(c->tex);
        return;
    }
    const uint64_t t_proc0 = seto_perf_now_us();
    seto_perf_add(&g_seto_perf.g_draws, 1);
    if (c->state.tex_enable && !c->state.clear &&
        c->fbo_tex_slot >= 0 && c->fbo_tex_slot < GE_MAX_FBOS &&
        g_ge_fbos[c->fbo_tex_slot].used) {
        ge_gpu_fbo *tf_tex = &g_ge_fbos[c->fbo_tex_slot];
        ge_resolve_fbo_ex(tf_tex, tf_tex->stencil_alpha_dirty ? 1 : 0);
        if (tf_tex->stencil_alpha_dirty)
            ge_sync_stencil_to_alpha(tf_tex);
    }
    ge_gpu_fbo *target = &g_ge_fbos[c->fbo_slot];

    switch (c->prim) {
    case PSP_PRIM_POINTS:         mode = GL_POINTS;         half_px = 0.5f; break;
    case PSP_PRIM_LINES:          mode = GL_LINES;          half_px = 0.5f; break;
    case PSP_PRIM_LINE_STRIP:     mode = GL_LINE_STRIP;     half_px = 0.5f; break;
    case PSP_PRIM_TRIANGLES:      mode = GL_TRIANGLES;      break;
    case PSP_PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PSP_PRIM_TRIANGLE_FAN:   mode = GL_TRIANGLE_FAN;   break;
    default: free(c->tex); return;
    }

    GLuint tex = 0;
    int tex_is_fbo = 0;
    int tex_is_hd = 0;
    ge_hd_texture *card_front = NULL;
    int fbo_tw = 1, fbo_th = 1, fbo_ts = 1;
    if (c->state.tex_enable && !c->state.clear &&
        c->state.tex_w > 0 && c->state.tex_h > 0) {
        if (c->card_front_path_off >= 0) {
            card_front = ge_get_hd_texture(1, c->card_front_path_off, 0, NULL);
            if (!card_front) {
                seto_perf_add(&g_seto_perf.g_draw_us, seto_perf_now_us() - t_proc0);
                return;
            }
            tex = card_front->texture;
            tex_is_hd = 1;
            if (!card_front->params.valid || g_gl_sc.bound_tex != tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                g_gl_sc.bound_tex = tex;
            }
            if (!card_front->params.valid) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                card_front->params.valid = 1;
            }
        } else if (c->fbo_tex_slot >= 0 && c->fbo_tex_slot < GE_MAX_FBOS &&
            g_ge_fbos[c->fbo_tex_slot].used) {
            const ge_gpu_fbo *tf = &g_ge_fbos[c->fbo_tex_slot];
            tex_is_fbo = 1;
            fbo_tw = tf->phys_w;
            fbo_th = tf->phys_h;
            fbo_ts = tf->scale;
            if (c->fbo_tex_slot == c->fbo_slot) {
                if (!g_ge_self_tex) glGenTextures(1, &g_ge_self_tex);
                if (g_gl_sc.bound_tex != g_ge_self_tex) {
                    glBindTexture(GL_TEXTURE_2D, g_ge_self_tex);
                    g_gl_sc.bound_tex = g_ge_self_tex;
                }
                if (g_ge_self_w != tf->phys_w || g_ge_self_h != tf->phys_h) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tf->phys_w, tf->phys_h,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                    g_ge_self_w = tf->phys_w;
                    g_ge_self_h = tf->phys_h;
                }
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, tf->fbo);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tf->phys_w, tf->phys_h);
                tex = g_ge_self_tex;
            } else {
                tex = tf->color_tex;
                if (g_gl_sc.bound_tex != tex) {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    g_gl_sc.bound_tex = tex;
                }
            }
        } else if (c->tex_slot >= 0 && c->tex_slot < GE_TEX_CACHE_SLOTS) {
            const int slot = c->tex_slot;
            ge_hd_texture *hd = g_ge_hd_slots[slot];
            if (c->tex) {
                int hd_pending = 0;
                if (c->tex_hash != 0u)
                    ge_dump_tex_png(c->tex_hash, c->tex, c->state.tex_w, c->state.tex_h);
                ge_hd_texture *replacement = ge_get_hd_texture(c->tex_hash, c->hd_path_off, 1, &hd_pending);
                ge_release_hd_texture(hd);
                g_ge_hd_slots[slot] = hd = replacement;
                if (!hd && hd_pending) {
                    g_ge_hd_pending_slots[slot] = 1u;
                    g_ge_hd_pending_path_off[slot] = c->hd_path_off;
                } else {
                    g_ge_hd_pending_slots[slot] = 0u;
                    g_ge_hd_pending_path_off[slot] = -1;
                }
            } else if (!hd && g_ge_hd_pending_slots[slot] && c->tex_hash != 0u) {
                int hd_pending = 0;
                ge_hd_texture *replacement = ge_get_hd_texture(c->tex_hash, g_ge_hd_pending_path_off[slot], 1, &hd_pending);
                if (replacement) {
                    g_ge_hd_slots[slot] = hd = replacement;
                    g_ge_hd_pending_slots[slot] = 0u;
                    g_ge_hd_pending_path_off[slot] = -1;
                } else if (!hd_pending) {
                    g_ge_hd_pending_slots[slot] = 0u;
                    g_ge_hd_pending_path_off[slot] = -1;
                }
            }
            if (hd) {
                tex = hd->texture;
                hd->used_at = ++g_ge_hd_clock;
            } else {
                if (!g_ge_tex_cache[slot]) glGenTextures(1, &g_ge_tex_cache[slot]);
                tex = g_ge_tex_cache[slot];
            }
            tex_is_hd = hd != NULL;
            const GLint tfilt = (tex_is_hd || (c->state.tex_filter & 0x101)) ? GL_LINEAR : GL_NEAREST;
            const GLint ws = c->state.tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            const GLint wt = c->state.tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            ge_tex_param_cache *tp = hd ? &hd->params : &g_ge_tex_params[slot];
            const int need_upload = (!hd && c->tex);
            const int need_params = (!tp->valid || tp->filt != tfilt || tp->wrap_s != ws || tp->wrap_t != wt);
            if (need_upload || need_params || g_gl_sc.bound_tex != tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                g_gl_sc.bound_tex = tex;
            }
            if (need_upload) {
                seto_perf_add(&g_seto_perf.g_tex_up, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->state.tex_w, c->state.tex_h,
                             0, GL_RGBA, GL_UNSIGNED_BYTE, c->tex);
            }
            if (need_params) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tfilt);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tfilt);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
                tp->filt = tfilt;
                tp->wrap_s = ws;
                tp->wrap_t = wt;
                tp->valid = 1;
            }
        }
    }
    free(c->tex);

    if (c->state.clear && target->msaa_fbo && ge_is_full_clear(c, target)) {
        if (c->state.clear_color && (c->state.write_mask & 0x00FFFFFFu) == 0u) {
            target->ss_newer = 0;
            target->msaa_dirty = 0;
        }
        if (c->state.clear_depth && (c->state.clear_alpha || target->fmt == 0) &&
            ((~c->state.write_mask >> 24) == 0xFFu || !c->state.clear_alpha)) {
            target->ss_ds_newer = 0;
            target->msaa_ds_dirty = 0;
        }
    }
    if (!c->state.clear && !c->state.through && g_ge_msaa >= 2 && !target->msaa_fbo)
        ge_alloc_msaa_for_fbo(target);
    const int use_msaa = (target->samples >= 2 && target->msaa_fbo != 0);

    ge_bind_target_for_draw(c->fbo_slot, use_msaa, 0);
    const int vp_full = !g_gl_sc.vp_valid;
    if (vp_full || g_gl_sc.vp_w != target->phys_w || g_gl_sc.vp_h != target->phys_h) {
        glViewport(0, 0, target->phys_w, target->phys_h);
        g_gl_sc.vp_w = target->phys_w;
        g_gl_sc.vp_h = target->phys_h;
    }
    const GLfloat pt_sz = c->prim == PSP_PRIM_POINTS ? (GLfloat)target->scale : 1.0f;
    if (vp_full || g_gl_sc.point_size != pt_sz) {
        glPointSize(pt_sz);
        g_gl_sc.point_size = pt_sz;
    }
    ge_apply_state(&c->state, verts[0].rgba, target->w, target->h, target->scale);
    const int ms_on = (use_msaa && !c->state.through && !c->state.clear) ? 1 : 0;
    if (vp_full || g_gl_sc.multisample != ms_on) {
        if (ms_on) glEnable(GL_MULTISAMPLE);
        else glDisable(GL_MULTISAMPLE);
        g_gl_sc.multisample = ms_on;
    }
    g_gl_sc.vp_valid = 1;
    if (ge_debug_draw_env()) {
        static int nlogged;
        if (nlogged < 6) {
            nlogged++;
            fprintf(stderr, "gpu: draw fbo=%d(%dx%d@%dx) prim=%u n=%u clear=%d cc=%d ca=%d cd=%d "
                    "sc=%d,%d-%d,%d reg=%d/%d v0=%d,%d z=%.0f col=%08X slot=%d fbo_tex=%d err=%u\n",
                    c->fbo_slot, target->w, target->h, target->scale, c->prim, count, c->state.clear,
                    c->state.clear_color, c->state.clear_alpha, c->state.clear_depth,
                    c->state.sc_x0, c->state.sc_y0, c->state.sc_x1, c->state.sc_y1,
                    c->state.reg_enable, c->state.reg_enable ? c->state.reg_x1 : 0,
                    verts[0].x, verts[0].y, verts[0].z,
                    verts[0].rgba, c->tex_slot, c->fbo_tex_slot, glGetError());
        }
    }

    const int full = !g_geo_uc.valid;
    if (full) {
        p_glUseProgram(g_ge_geo_prog);
        p_glBindVertexArray(g_ge_geo_vao);
        p_glUniform1i(g_ge_geo_tex_sampler, 0);
        g_geo_uc.sub_init = 0;
        g_geo_uc.valid = 1;
    }

    if (full || g_geo_uc.size_w != target->w || g_geo_uc.size_h != target->h) {
        p_glUniform2f(g_ge_geo_size, (GLfloat)target->w, (GLfloat)target->h);
        g_geo_uc.size_w = target->w;
        g_geo_uc.size_h = target->h;
    }
    if (full || g_geo_uc.half_px != half_px) {
        p_glUniform1f(g_ge_geo_half_px, half_px);
        g_geo_uc.half_px = half_px;
    }
    const int u_clr = c->state.clear ? 1 : 0;
    if (full || g_geo_uc.clear != u_clr) {
        p_glUniform1i(g_ge_geo_clear, u_clr);
        g_geo_uc.clear = u_clr;
    }
    const int u_sub = (target->scale > 1 && !c->state.through && !c->state.clear) ? 1 : 0;
    if (full || g_geo_uc.use_subpixel != u_sub) {
        p_glUniform1i(g_ge_geo_use_subpixel, u_sub);
        g_geo_uc.use_subpixel = u_sub;
    }

    const int u_ton = tex ? 1 : 0;
    const int u_tw = tex ? c->state.tex_w : 1;
    const int u_th = tex ? c->state.tex_h : 1;
    if (full || g_geo_uc.tex_on != u_ton) {
        p_glUniform1i(g_ge_geo_tex_on, u_ton);
        g_geo_uc.tex_on = u_ton;
    }
    if (full || (u_ton && (g_geo_uc.tex_w != u_tw || g_geo_uc.tex_h != u_th))) {
        p_glUniform2f(g_ge_geo_tex_size, (GLfloat)u_tw, (GLfloat)u_th);
        g_geo_uc.tex_w = u_tw;
        g_geo_uc.tex_h = u_th;
    }
    if (full || g_geo_uc.tex_is_fbo != tex_is_fbo) {
        p_glUniform1i(g_ge_geo_tex_is_fbo, tex_is_fbo);
        g_geo_uc.tex_is_fbo = tex_is_fbo;
    }
    if (tex_is_fbo) {
        const int finit = !(g_geo_uc.sub_init & 2u);
        if (finit || g_geo_uc.fbo_tw != fbo_tw || g_geo_uc.fbo_th != fbo_th) {
            p_glUniform2i(g_ge_geo_fbo_tex_size, fbo_tw, fbo_th);
            g_geo_uc.fbo_tw = fbo_tw;
            g_geo_uc.fbo_th = fbo_th;
        }
        if (finit || g_geo_uc.fbo_ts != fbo_ts) {
            p_glUniform1i(g_ge_geo_fbo_scale, fbo_ts);
            g_geo_uc.fbo_ts = fbo_ts;
        }
        g_geo_uc.sub_init |= 2u;
    }
    if (u_ton) {
        const int cu = c->state.tex_clamp_u ? 1 : 0;
        const int cv = c->state.tex_clamp_v ? 1 : 0;
        const int tf_val = (tex_is_hd || (c->state.tex_filter & 0x101)) ? 1 : 0;
        const int s2d = (g_ge_smart_2d && !tex_is_hd && target->scale > 1 && c->state.through &&
                         !tex_is_fbo && (c->state.tex_filter & 0x101) != 0) ? 1 : 0;
        const int tinit = !(g_geo_uc.sub_init & 1u);
        if (tinit || g_geo_uc.clamp_u != cu || g_geo_uc.clamp_v != cv) {
            p_glUniform2i(g_ge_geo_tex_clamp, cu, cv);
            g_geo_uc.clamp_u = cu;
            g_geo_uc.clamp_v = cv;
        }
        if (tinit || g_geo_uc.tex_filter != tf_val) {
            p_glUniform1i(g_ge_geo_tex_filter, tf_val);
            g_geo_uc.tex_filter = tf_val;
        }
        if (tinit || g_geo_uc.smart_2d != s2d) {
            p_glUniform1i(g_ge_geo_smart_2d, s2d);
            g_geo_uc.smart_2d = s2d;
        }
        if (tinit || g_geo_uc.tex_func != c->state.tex_func) {
            p_glUniform1i(g_ge_geo_tex_func, c->state.tex_func);
            g_geo_uc.tex_func = c->state.tex_func;
        }
        const int ta = c->state.tex_alpha ? 1 : 0;
        if (tinit || g_geo_uc.tex_alpha != ta) {
            p_glUniform1i(g_ge_geo_tex_alpha, ta);
            g_geo_uc.tex_alpha = ta;
        }
        const int td = c->state.tex_double ? 1 : 0;
        if (tinit || g_geo_uc.tex_double != td) {
            p_glUniform1i(g_ge_geo_tex_double, td);
            g_geo_uc.tex_double = td;
        }
        const uint32_t e = c->state.tex_env;
        if (tinit || g_geo_uc.tex_env != e) {
            p_glUniform4f(g_ge_geo_tex_env, (GLfloat)(e & 0xFFu) / 255.0f,
                          (GLfloat)((e >> 8) & 0xFFu) / 255.0f,
                          (GLfloat)((e >> 16) & 0xFFu) / 255.0f, 1.0f);
            g_geo_uc.tex_env = e;
        }
        g_geo_uc.sub_init |= 1u;
    }
    if (full || g_geo_uc.render_scale != target->scale) {
        p_glUniform1f(g_ge_geo_render_scale, (GLfloat)target->scale);
        g_geo_uc.render_scale = target->scale;
    }

    const int at_on = c->state.atest ? 1 : 0;
    if (full || g_geo_uc.atest != at_on) {
        p_glUniform1i(g_ge_geo_atest, at_on);
        g_geo_uc.atest = at_on;
    }
    if (at_on) {
        const int ar = c->state.atest_ref & 0xFF;
        const int am = c->state.atest_mask & 0xFF;
        const int ainit = !(g_geo_uc.sub_init & 4u);
        if (ainit || g_geo_uc.afunc != c->state.atest_func) {
            p_glUniform1i(g_ge_geo_afunc, c->state.atest_func);
            g_geo_uc.afunc = c->state.atest_func;
        }
        if (ainit || g_geo_uc.aref != ar) {
            p_glUniform1i(g_ge_geo_aref, ar);
            g_geo_uc.aref = ar;
        }
        if (ainit || g_geo_uc.amask != am) {
            p_glUniform1i(g_ge_geo_amask, am);
            g_geo_uc.amask = am;
        }
        g_geo_uc.sub_init |= 4u;
    }

    const int ct_on = c->state.ctest ? 1 : 0;
    if (full || g_geo_uc.ctest != ct_on) {
        p_glUniform1i(g_ge_geo_ctest, ct_on);
        g_geo_uc.ctest = ct_on;
    }
    if (ct_on) {
        const uint32_t cr = c->state.ctest_ref & 0x00FFFFFFu;
        const uint32_t cm = c->state.ctest_mask & 0x00FFFFFFu;
        const int cinit = !(g_geo_uc.sub_init & 8u);
        if (cinit || g_geo_uc.cfunc != c->state.ctest_func) {
            p_glUniform1i(g_ge_geo_cfunc, c->state.ctest_func);
            g_geo_uc.cfunc = c->state.ctest_func;
        }
        if (cinit || g_geo_uc.cref != cr) {
            p_glUniform3i(g_ge_geo_cref, (GLint)(cr & 0xFFu),
                          (GLint)((cr >> 8) & 0xFFu),
                          (GLint)((cr >> 16) & 0xFFu));
            g_geo_uc.cref = cr;
        }
        if (cinit || g_geo_uc.cmask != cm) {
            p_glUniform3i(g_ge_geo_cmask, (GLint)(cm & 0xFFu),
                          (GLint)((cm >> 8) & 0xFFu),
                          (GLint)((cm >> 16) & 0xFFu));
            g_geo_uc.cmask = cm;
        }
        g_geo_uc.sub_init |= 8u;
    }

    const int lm = c->state.lmode ? 1 : 0;
    if (full || g_geo_uc.lmode != lm) {
        p_glUniform1i(g_ge_geo_lmode, lm);
        g_geo_uc.lmode = lm;
    }
    const int fog_on = (c->state.fog_enable && !c->state.through) ? 1 : 0;
    if (full || g_geo_uc.fog_on != fog_on) {
        p_glUniform1i(g_ge_geo_fog_on, fog_on);
        g_geo_uc.fog_on = fog_on;
    }
    if (fog_on) {
        const uint32_t fc = c->state.fog_color & 0x00FFFFFFu;
        if (!(g_geo_uc.sub_init & 16u) || g_geo_uc.fog_color != fc) {
            p_glUniform3f(g_ge_geo_fog_color, (GLfloat)(fc & 0xFFu) / 255.0f,
                          (GLfloat)((fc >> 8) & 0xFFu) / 255.0f,
                          (GLfloat)((fc >> 16) & 0xFFu) / 255.0f);
            g_geo_uc.fog_color = fc;
            g_geo_uc.sub_init |= 16u;
        }
    }

    const int zr = (c->state.zrange_enable && !c->state.through) ? 1 : 0;
    if (full || g_geo_uc.zrange != zr) {
        p_glUniform1i(g_ge_geo_zrange, zr);
        g_geo_uc.zrange = zr;
    }
    if (zr) {
        const int zinit = !(g_geo_uc.sub_init & 32u);
        if (zinit || g_geo_uc.zmin != c->state.zmin) {
            p_glUniform1i(g_ge_geo_zmin, c->state.zmin);
            g_geo_uc.zmin = c->state.zmin;
        }
        if (zinit || g_geo_uc.zmax != c->state.zmax) {
            p_glUniform1i(g_ge_geo_zmax, c->state.zmax);
            g_geo_uc.zmax = c->state.zmax;
        }
        g_geo_uc.sub_init |= 32u;
    }
    const int zcv = (c->state.clear || c->state.through || !c->state.ztest) ? 1 : 0;
    if (full || g_geo_uc.zclamp_vert != zcv) {
        p_glUniform1i(g_ge_geo_zclamp_vert, zcv);
        g_geo_uc.zclamp_vert = zcv;
    }
    {
        GLenum sfactor, dfactor;
        int use_const, src_premul, alpha_2x;
        uint32_t const_rgb;
        const uint32_t sf = c->state.blend_sfix & 0x00FFFFFFu;
        ge_plan_blend(&c->state, &sfactor, &dfactor, &use_const, &const_rgb,
                      &src_premul, &alpha_2x);
        if (full || g_geo_uc.src_premul != src_premul) {
            p_glUniform1i(g_ge_geo_src_premul, src_premul);
            g_geo_uc.src_premul = src_premul;
        }
        if (src_premul && (full || !(g_geo_uc.sub_init & 64u) || g_geo_uc.sfix != sf)) {
            p_glUniform3f(g_ge_geo_sfix_rgb, (GLfloat)(sf & 0xFFu) / 255.0f,
                          (GLfloat)((sf >> 8) & 0xFFu) / 255.0f,
                          (GLfloat)((sf >> 16) & 0xFFu) / 255.0f);
            g_geo_uc.sfix = sf;
            g_geo_uc.sub_init |= 64u;
        }
        if (full || g_geo_uc.alpha_2x != alpha_2x) {
            p_glUniform1i(g_ge_geo_alpha_2x, alpha_2x);
            g_geo_uc.alpha_2x = alpha_2x;
        }
    }
    const GLint base_vert = ge_upload_geo_verts(verts, count);
    glDrawArrays(mode, base_vert, (GLsizei)count);
    ge_release_hd_texture(card_front);
    if (use_msaa) {
        if (c->state.clear) {
            if (c->state.clear_color && (c->state.write_mask & 0x00FFFFFFu) != 0x00FFFFFFu) {
                target->msaa_dirty = 1;
                target->ss_newer = 0;
            }
            if (c->state.clear_depth || c->state.clear_alpha) {
                target->msaa_ds_dirty = 1;
                target->ss_ds_newer = 0;
            }
        } else {
            target->msaa_dirty = 1;
            target->ss_newer = 0;
            if (c->state.zwrite || c->state.stencil) {
                target->msaa_ds_dirty = 1;
                target->ss_ds_newer = 0;
            }
        }
    } else {
        if (c->state.clear) {
            if (c->state.clear_color && (c->state.write_mask & 0x00FFFFFFu) != 0x00FFFFFFu)
                target->ss_newer = 1;
            if (c->state.clear_depth || c->state.clear_alpha)
                target->ss_ds_newer = 1;
        } else {
            target->ss_newer = 1;
            if (c->state.zwrite || c->state.stencil)
                target->ss_ds_newer = 1;
        }
    }
    if (target->fmt != 0) {
        const uint32_t amask = (c->state.write_mask >> 24) & 0xFFu;
        if ((!c->state.clear && c->state.stencil && amask != 0xFFu) ||
            (c->state.clear && c->state.clear_alpha && amask != 0u && amask != 0xFFu))
            target->stencil_alpha_dirty = 1;
    }
    if (ge_debug_draw_env() && count > 0u) {
        static int npost;
        if (npost < 4) {
            unsigned char px[4] = {0, 0, 0, 0};
            npost++;
            ge_resolve_fbo_ex(target, 0);
            p_glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
            g_ge_cur_fbo_slot = c->fbo_slot;
            g_ge_cur_is_msaa = 0;
            glReadPixels(target->phys_w / 2, target->phys_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "gpu: post draw prim=%u n=%u tex=%dx%d center=%02X%02X%02X%02X\n",
                    c->prim, count, c->state.tex_w, c->state.tex_h,
                    px[0], px[1], px[2], px[3]);
        }
    }
    seto_perf_add(&g_seto_perf.g_draw_us, seto_perf_now_us() - t_proc0);
}

static void ge_destroy_fbo(ge_gpu_fbo *f) {
    if (!f) return;
    if (f->msaa_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &f->msaa_fbo);
    if (f->msaa_color_rb && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &f->msaa_color_rb);
    if (f->msaa_depth_rb && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &f->msaa_depth_rb);
    if (f->fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &f->fbo);
    if (f->depth_rb && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &f->depth_rb);
    if (f->color_tex) glDeleteTextures(1, &f->color_tex);
    memset(f, 0, sizeof *f);
}

static void ge_alloc_msaa_for_fbo(ge_gpu_fbo *f) {
    if (!f || !p_glRenderbufferStorageMultisample || g_ge_msaa < 2) {
        if (f) f->samples = 0;
        return;
    }
    int eff_samples = g_ge_msaa;
    const char *fm = getenv("SETO_FORCE_MSAA");
    if (!fm || !fm[0] || fm[0] == '0') {
        while (eff_samples > 2 &&
               (int64_t)f->phys_w * (int64_t)f->phys_h * (int64_t)eff_samples > 10000000LL)
            eff_samples /= 2;
    }
    if (eff_samples != g_ge_msaa) {
        static int capped_logged;
        if (capped_logged < 4) {
            capped_logged++;
            fprintf(stderr, "gl: capped MSAA %dx -> %dx for %dx%d target\n",
                    g_ge_msaa, eff_samples, f->phys_w, f->phys_h);
        }
    }
    GLuint mfbo = 0, mcol = 0, mdep = 0;
    p_glGenRenderbuffers(1, &mcol);
    p_glBindRenderbuffer(GL_RENDERBUFFER, mcol);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, eff_samples, GL_RGBA8,
                                       f->phys_w, f->phys_h);
    p_glGenRenderbuffers(1, &mdep);
    p_glBindRenderbuffer(GL_RENDERBUFFER, mdep);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, eff_samples, GL_DEPTH24_STENCIL8,
                                       f->phys_w, f->phys_h);
    p_glGenFramebuffers(1, &mfbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, mfbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, mcol);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mdep);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteFramebuffers(1, &mfbo);
        p_glDeleteRenderbuffers(1, &mcol);
        p_glDeleteRenderbuffers(1, &mdep);
        f->samples = 0;
        g_ge_cur_fbo_slot = -1;
        g_ge_cur_is_msaa = 0;
        return;
    }
    ge_gl_state_invalidate();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glClearColor(0.0f, 0.0f, 0.0f, f->fmt == 0 ? 1.0f : 0.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    f->samples = eff_samples;
    f->msaa_fbo = mfbo;
    f->msaa_color_rb = mcol;
    f->msaa_depth_rb = mdep;
    f->msaa_dirty = 0;
    f->msaa_ds_dirty = 0;
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static int ge_alloc_fbo(int w, int h, int fmt, GLuint *out_fbo, GLuint *out_tex, GLuint *out_depth) {
    GLuint tex = 0, fbo = 0, depth = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    p_glGenRenderbuffers(1, &depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: GE FBO incomplete (%dx%d)\n", w, h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        p_glDeleteRenderbuffers(1, &depth);
        return 0;
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glClearColor(0.0f, 0.0f, 0.0f, fmt == 0 ? 1.0f : 0.0f);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    ge_gl_state_invalidate();
    *out_fbo = fbo;
    *out_tex = tex;
    *out_depth = depth;
    return 1;
}

static int ge_clamp_scale(int s) {
    if (s < 1) return 1;
    if (s > 10) return 10;
    return s;
}

static int ge_scale_for_dims(int base, int w, int h) {
    int s = ge_clamp_scale(base);
    const int max_d = g_ge_max_dim > 256 ? g_ge_max_dim : 4096;
    while (s > 1 && ((int64_t)w * s > max_d || (int64_t)h * s > max_d))
        s--;
    return s;
}

static int ge_auto_scale_for_client(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return 1;
    int dw;
    if ((int64_t)cw * 17 > (int64_t)ch * 30)
        dw = (int)((int64_t)ch * 30 / 17);
    else
        dw = cw;
    int s = (dw + GL_SCREEN_W - 1) / GL_SCREEN_W;
    return ge_clamp_scale(s);
}

static int ge_ensure_stage(int w, int h) {
    int sw = w > 512 ? w : 512;
    int sh = h > 272 ? h : 272;
    if (g_ge_stage_fbo && sw <= g_ge_stage_w && sh <= g_ge_stage_h)
        return 1;
    if (sw < g_ge_stage_w) sw = g_ge_stage_w;
    if (sh < g_ge_stage_h) sh = g_ge_stage_h;
    GLuint nfbo = 0, ntex = 0, ndepth = 0;
    if (!ge_alloc_fbo(sw, sh, 3, &nfbo, &ntex, &ndepth))
        return 0;
    if (g_ge_stage_fbo) p_glDeleteFramebuffers(1, &g_ge_stage_fbo);
    if (g_ge_stage_tex) glDeleteTextures(1, &g_ge_stage_tex);
    if (g_ge_stage_depth) p_glDeleteRenderbuffers(1, &g_ge_stage_depth);
    g_ge_stage_fbo = nfbo;
    g_ge_stage_tex = ntex;
    g_ge_stage_depth = ndepth;
    g_ge_stage_w = sw;
    g_ge_stage_h = sh;
    p_glBindFramebuffer(GL_FRAMEBUFFER,
                        (g_ge_cur_fbo_slot >= 0 && g_ge_fbos[g_ge_cur_fbo_slot].used)
                            ? g_ge_fbos[g_ge_cur_fbo_slot].fbo : 0);
    return 1;
}

static int ge_alloc_front_target(int scale, GLuint *out_fbo, GLuint *out_tex,
                                 int *out_w, int *out_h) {
    const int fs = ge_scale_for_dims(scale, GL_SCREEN_W, GL_SCREEN_H);
    const int fw = GL_SCREEN_W * fs;
    const int fh = GL_SCREEN_H * fs;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: front FBO incomplete (%dx%d)\n", fw, fh);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    *out_fbo = fbo;
    *out_tex = tex;
    *out_w = fw;
    *out_h = fh;
    return 1;
}

static int ge_make_front_target(int scale) {
    GLuint nfbo = 0, ntex = 0;
    int fw = 0, fh = 0;
    if (!ge_alloc_front_target(scale, &nfbo, &ntex, &fw, &fh))
        return 0;
    if (g_ge_front_fbo && p_glDeleteFramebuffers)
        p_glDeleteFramebuffers(1, &g_ge_front_fbo);
    if (g_ge_front_tex)
        glDeleteTextures(1, &g_ge_front_tex);
    g_ge_front_fbo = nfbo;
    g_ge_front_tex = ntex;
    g_ge_front_w = fw;
    g_ge_front_h = fh;
    g_ge_w = fw;
    g_ge_h = fh;
    return 1;
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
    if (g_ge_depth) {
        p_glDeleteRenderbuffers(1, &g_ge_depth);
        g_ge_depth = 0;
    }
    if (!ge_alloc_fbo(w, h, 0, &g_ge_fbo, &g_ge_tex, &g_ge_depth))
        return 0;
    glViewport(0, 0, w, h);
    g_ge_w = w;
    g_ge_h = h;
    return 1;
}

static void ge_free_gl(void) {
    EnterCriticalSection(&g_ge_lock);
    if (p_glDeleteSync) {
        if (g_ge_front_sync) { p_glDeleteSync(g_ge_front_sync); g_ge_front_sync = NULL; }
        if (g_present_read_sync) { p_glDeleteSync(g_present_read_sync); g_present_read_sync = NULL; }
    }
    atomic_store_explicit(&g_ge_gpu_frame_ready, 0, memory_order_release);
    atomic_store_explicit(&g_ge_front_seq, 0u, memory_order_release);
    atomic_store_explicit(&g_present_force_redraw, 1, memory_order_release);
    LeaveCriticalSection(&g_ge_lock);

    for (int i = 0; i < GE_MAX_FBOS; i++)
        ge_destroy_fbo(&g_ge_fbos[i]);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    if (g_ge_stage_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_stage_fbo);
    if (g_ge_stage_depth && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &g_ge_stage_depth);
    if (g_ge_stage_tex) glDeleteTextures(1, &g_ge_stage_tex);
    g_ge_stage_fbo = g_ge_stage_depth = g_ge_stage_tex = 0;
    g_ge_stage_w = g_ge_stage_h = 0;
    if (g_ge_blit_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_blit_fbo);
    if (g_ge_self_tex) glDeleteTextures(1, &g_ge_self_tex);
    g_ge_blit_fbo = g_ge_self_tex = 0;
    g_ge_self_w = g_ge_self_h = 0;
    if (g_ge_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_fbo);
    if (g_ge_depth && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &g_ge_depth);
    if (g_ge_tex) glDeleteTextures(1, &g_ge_tex);
    if (g_ge_front_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &g_ge_front_fbo);
    if (g_ge_front_tex) glDeleteTextures(1, &g_ge_front_tex);
    glDeleteTextures(GE_TEX_CACHE_SLOTS, g_ge_tex_cache);
    memset(g_ge_tex_cache, 0, sizeof g_ge_tex_cache);
    memset(g_ge_tex_params, 0, sizeof g_ge_tex_params);
    for (size_t i = 0; i < sizeof g_ge_hd_cache / sizeof g_ge_hd_cache[0]; i++) {
        if (g_ge_hd_cache[i].texture) glDeleteTextures(1, &g_ge_hd_cache[i].texture);
    }
    memset(g_ge_hd_cache, 0, sizeof g_ge_hd_cache);
    memset(g_ge_hd_slots, 0, sizeof g_ge_hd_slots);
    memset(g_ge_hd_pending_slots, 0, sizeof g_ge_hd_pending_slots);
    for (int i = 0; i < GE_TEX_CACHE_SLOTS; i++) g_ge_hd_pending_path_off[i] = -1;
    g_ge_hd_clock = 0;
    memset(g_dumped_hashes, 0, sizeof g_dumped_hashes);
    g_dumped_count = 0;
    memset(g_hd_miss_hashes, 0, sizeof g_hd_miss_hashes);
    g_hd_miss_cursor = 0;
    g_hd_logged = 0;
    if (g_ge_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_prog);
    if (g_ge_geo_prog && p_glDeleteProgram) p_glDeleteProgram(g_ge_geo_prog);
    if (g_ge_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_vao);
    if (g_ge_geo_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_ge_geo_vao);
    if (g_ge_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_vbo);
    if (g_ge_geo_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_ge_geo_vbo);
    g_ge_fbo = g_ge_depth = g_ge_tex = g_ge_front_fbo = g_ge_front_tex = 0;
    g_ge_prog = g_ge_vao = g_ge_vbo = g_ge_geo_prog = g_ge_geo_vao = g_ge_geo_vbo = 0;
    g_ge_geo_vbo_head = 0u;
    memset(&g_geo_uc, 0, sizeof g_geo_uc);
    ge_gl_state_invalidate();
    g_ge_w = g_ge_h = 0;
    g_ge_front_w = GL_SCREEN_W;
    g_ge_front_h = GL_SCREEN_H;
    g_ge_have_front = 0;
    g_ge_passthrough_sw = 0;
    g_ge_msaa = 0;
}

static void ge_wait_present_read_sync(void) {
    if (g_present_read_sync) {
        if (p_glWaitSync)
            p_glWaitSync(g_present_read_sync, 0, GL_TIMEOUT_IGNORED);
        if (p_glDeleteSync)
            p_glDeleteSync(g_present_read_sync);
        g_present_read_sync = NULL;
    }
}

static void ge_record_front_sync(void) {
    if (g_ge_front_sync && p_glDeleteSync) {
        p_glDeleteSync(g_ge_front_sync);
        g_ge_front_sync = NULL;
    }
    if (p_glFenceSync) {
        g_ge_front_sync = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    } else {
        glFinish();
    }
    atomic_store_explicit(&g_ge_gpu_frame_ready,
                          (g_ge_have_front && !g_ge_passthrough_sw) ? 1 : 0,
                          memory_order_release);
    atomic_fetch_add_explicit(&g_ge_front_seq, 1u, memory_order_acq_rel);
}

static void ge_write_ppm_file(const char *path, const uint32_t *rgba_bottom_up,
                              int src_w, int src_h, int out_w, int out_h) {
    if (!path || !rgba_bottom_up || src_w <= 0 || src_h <= 0 ||
        out_w <= 0 || out_h <= 0)
        return;
    const size_t rgb_bytes = (size_t)out_w * (size_t)out_h * 3u;
    uint8_t *rgb = (uint8_t *)calloc(1, rgb_bytes);
    if (!rgb) return;
    const int cw = src_w < out_w ? src_w : out_w;
    const int ch = src_h < out_h ? src_h : out_h;
    for (int y = 0; y < ch; y++) {
        const uint32_t *src = rgba_bottom_up + (size_t)(ch - 1 - y) * (size_t)src_w;
        uint8_t *dst = rgb + (size_t)y * (size_t)out_w * 3u;
        for (int x = 0; x < cw; x++) {
            const uint32_t c = src[x];
            dst[x * 3 + 0] = (uint8_t)(c & 0xFFu);
            dst[x * 3 + 1] = (uint8_t)((c >> 8) & 0xFFu);
            dst[x * 3 + 2] = (uint8_t)((c >> 16) & 0xFFu);
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", out_w, out_h);
        fwrite(rgb, 1, rgb_bytes, f);
        fclose(f);
    }
    free(rgb);
}

static void ge_capture_1x_ppm(uint32_t capture_id, ge_gpu_fbo *df) {
    static uint32_t px[GL_SCREEN_W * GL_SCREEN_H];
    const char *dir = ge_shot_dir_env();
    if (!dir || !dir[0] || !capture_id || !df || !df->used) return;
    ge_resolve_fbo_ex(df, 0);
    const int cw = df->w < GL_SCREEN_W ? df->w : GL_SCREEN_W;
    const int ch = df->h < GL_SCREEN_H ? df->h : GL_SCREEN_H;
    if (cw <= 0 || ch <= 0) return;
    if (df->scale == 1) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, df->fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, df->h - ch, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
    } else {
        if (!ge_ensure_stage(cw, ch)) return;
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, df->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ge_stage_fbo);
        p_glBlitFramebuffer(0, (df->h - ch) * df->scale,
                            cw * df->scale, df->h * df->scale,
                            0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_stage_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    char path[260];
    snprintf(path, sizeof path, "%s/gl-%05u.ppm", dir, capture_id);
    ge_write_ppm_file(path, px, cw, ch, GL_SCREEN_W, GL_SCREEN_H);
}

static void ge_capture_hires_ppm(uint32_t capture_id) {
    const char *dir = ge_hires_shot_dir_env();
    if (!dir || !dir[0] || !capture_id || !g_ge_front_fbo ||
        g_ge_front_w <= 0 || g_ge_front_h <= 0)
        return;
    const size_t npx = (size_t)g_ge_front_w * (size_t)g_ge_front_h;
    uint32_t *px = (uint32_t *)malloc(npx * sizeof(uint32_t));
    if (!px) return;
    EnterCriticalSection(&g_ge_lock);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_front_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, g_ge_front_w, g_ge_front_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LeaveCriticalSection(&g_ge_lock);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    char path[260];
    snprintf(path, sizeof path, "%s/hires-%05u.ppm", dir, capture_id);
    ge_write_ppm_file(path, px, g_ge_front_w, g_ge_front_h,
                      g_ge_front_w, g_ge_front_h);
    free(px);
}

static void ge_publish_fbo_slot(int slot) {
    if (slot < 0 || slot >= GE_MAX_FBOS || !g_ge_fbos[slot].used || !g_ge_front_fbo)
        return;
    ge_gpu_fbo *df = &g_ge_fbos[slot];
    ge_resolve_fbo_ex(df, 0);
    const int cw = df->w < GL_SCREEN_W ? df->w : GL_SCREEN_W;
    const int ch = df->h < GL_SCREEN_H ? df->h : GL_SCREEN_H;
    int fs = g_ge_front_w / GL_SCREEN_W;
    if (fs < 1) fs = 1;
    EnterCriticalSection(&g_ge_lock);
    ge_wait_present_read_sync();
    ge_gl_state_invalidate();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, df->fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ge_front_fbo);
    if (cw < GL_SCREEN_W || ch < GL_SCREEN_H) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    p_glBlitFramebuffer(0, (df->h - ch) * df->scale,
                        cw * df->scale, df->h * df->scale,
                        0, (GL_SCREEN_H - ch) * fs,
                        cw * fs, GL_SCREEN_H * fs,
                        GL_COLOR_BUFFER_BIT,
                        df->scale == fs ? GL_NEAREST : GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ge_have_front = 1;
    g_ge_passthrough_sw = 0;
    ge_record_front_sync();
    LeaveCriticalSection(&g_ge_lock);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static int ge_rescale_fbo_slot(ge_gpu_fbo *f, int new_h, int base_scale) {
    if (!f || !f->used || f->w <= 0 || new_h <= 0) return 0;
    const int target_s = ge_scale_for_dims(base_scale, f->w, new_h);
    if (new_h == f->h && target_s == f->scale) return 1;
    ge_resolve_fbo_ex(f, 1);
    const int nw = f->w * target_s;
    const int nh = new_h * target_s;
    GLuint nfbo = 0, ntex = 0, ndepth = 0;
    if (!ge_alloc_fbo(nw, nh, f->fmt, &nfbo, &ntex, &ndepth))
        return 0;
    const int src_h_log = f->h < new_h ? f->h : new_h;
    const int src_y0 = (f->h - src_h_log) * f->scale;
    const int src_y1 = f->h * f->scale;
    const int dst_y0 = (new_h - src_h_log) * target_s;
    const int dst_y1 = new_h * target_s;
    ge_gl_state_invalidate();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, nfbo);
    p_glBlitFramebuffer(0, src_y0, f->phys_w, src_y1,
                        0, dst_y0, nw, dst_y1,
                        GL_COLOR_BUFFER_BIT,
                        (target_s == f->scale) ? GL_NEAREST : GL_LINEAR);
    p_glBlitFramebuffer(0, src_y0, f->phys_w, src_y1,
                        0, dst_y0, nw, dst_y1,
                        GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (f->msaa_fbo && p_glDeleteFramebuffers) p_glDeleteFramebuffers(1, &f->msaa_fbo);
    if (f->msaa_color_rb && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &f->msaa_color_rb);
    if (f->msaa_depth_rb && p_glDeleteRenderbuffers) p_glDeleteRenderbuffers(1, &f->msaa_depth_rb);
    f->msaa_fbo = f->msaa_color_rb = f->msaa_depth_rb = 0;
    f->samples = 0;
    f->msaa_dirty = 0;
    f->msaa_ds_dirty = 0;
    p_glDeleteFramebuffers(1, &f->fbo);
    glDeleteTextures(1, &f->color_tex);
    p_glDeleteRenderbuffers(1, &f->depth_rb);
    f->fbo = nfbo;
    f->color_tex = ntex;
    f->depth_rb = ndepth;
    f->h = new_h;
    f->scale = target_s;
    f->phys_w = nw;
    f->phys_h = nh;
    f->ss_newer = 1;
    f->ss_ds_newer = 1;
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    return 1;
}

static void ge_apply_resize(int new_scale) {
    new_scale = ge_clamp_scale(new_scale);
    if (new_scale == g_ge_scale) return;

    GLuint nfbo = 0, ntex = 0;
    int fw = 0, fh = 0;
    if (!ge_alloc_front_target(new_scale, &nfbo, &ntex, &fw, &fh))
        return;

    EnterCriticalSection(&g_ge_lock);
    ge_wait_present_read_sync();
    ge_gl_state_invalidate();
    if (g_ge_have_front && g_ge_front_fbo) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ge_front_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, nfbo);
        p_glBlitFramebuffer(0, 0, g_ge_front_w, g_ge_front_h,
                            0, 0, fw, fh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (g_ge_front_fbo && p_glDeleteFramebuffers)
        p_glDeleteFramebuffers(1, &g_ge_front_fbo);
    if (g_ge_front_tex)
        glDeleteTextures(1, &g_ge_front_tex);
    g_ge_front_fbo = nfbo;
    g_ge_front_tex = ntex;
    g_ge_front_w = fw;
    g_ge_front_h = fh;
    g_ge_w = fw;
    g_ge_h = fh;
    ge_record_front_sync();
    LeaveCriticalSection(&g_ge_lock);

    g_ge_scale = new_scale;
    for (int i = 0; i < GE_MAX_FBOS; i++) {
        if (g_ge_fbos[i].used)
            ge_rescale_fbo_slot(&g_ge_fbos[i], g_ge_fbos[i].h, g_ge_scale);
    }
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static void ge_bind_or_create_fbo(const ge_cmd *c) {
    if (!c || c->fbo_slot < 0 || c->fbo_slot >= GE_MAX_FBOS ||
        c->w <= 0 || c->h <= 0)
        return;
    ge_gpu_fbo *f = &g_ge_fbos[c->fbo_slot];
    if (!c->color && f->used && f->addr == c->addr && f->stride == c->stride &&
        f->fmt == (int)c->fmt && f->w == c->w) {
        const int target_h = c->h > f->h ? c->h : f->h;
        if (target_h > f->h || ge_scale_for_dims(g_ge_scale, f->w, target_h) != f->scale)
            ge_rescale_fbo_slot(f, target_h, g_ge_scale);
        p_glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        g_ge_cur_fbo_slot = c->fbo_slot;
        g_ge_cur_is_msaa = 0;
        return;
    }

    ge_destroy_fbo(f);
    const int s = ge_scale_for_dims(g_ge_scale, c->w, c->h);
    const int pw = c->w * s;
    const int ph = c->h * s;
    GLuint nfbo = 0, ntex = 0, ndepth = 0;
    if (!ge_alloc_fbo(pw, ph, (int)c->fmt, &nfbo, &ntex, &ndepth)) return;
    f->used = 1;
    f->addr = c->addr;
    f->stride = c->stride;
    f->fmt = (int)c->fmt;
    f->w = c->w;
    f->h = c->h;
    f->scale = s;
    f->phys_w = pw;
    f->phys_h = ph;
    f->stencil_alpha_dirty = 0;
    f->fbo = nfbo;
    f->color_tex = ntex;
    f->depth_rb = ndepth;
    p_glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    g_ge_cur_fbo_slot = c->fbo_slot;
    g_ge_cur_is_msaa = 0;
}

static void ge_upload_target_cmd(const ge_cmd *c) {
    if (!c) return;
    if (c->tex && c->fbo_slot >= 0 && c->fbo_slot < GE_MAX_FBOS &&
        g_ge_fbos[c->fbo_slot].used && c->w > 0 && c->h > 0) {
        ge_gl_state_invalidate();
        ge_gpu_fbo *f = &g_ge_fbos[c->fbo_slot];
        ge_resolve_fbo_ex(f, 0);
        const int uw = c->w < f->w ? c->w : f->w;
        const int uh = c->h < f->h ? c->h : f->h;
        if (uw == c->w && uh > 0) {
            const uint32_t *src = c->tex + (size_t)(c->h - uh) * (size_t)c->w;
            if (f->scale == 1) {
                glBindTexture(GL_TEXTURE_2D, f->color_tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, f->h - uh, uw, uh,
                                GL_RGBA, GL_UNSIGNED_BYTE, src);
                glBindTexture(GL_TEXTURE_2D, 0);
            } else if (ge_ensure_stage(uw, uh)) {
                glBindTexture(GL_TEXTURE_2D, g_ge_stage_tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uw, uh,
                                GL_RGBA, GL_UNSIGNED_BYTE, src);
                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ge_stage_fbo);
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
                p_glBlitFramebuffer(0, 0, uw, uh,
                                    0, (f->h - uh) * f->scale, uw * f->scale, f->phys_h,
                                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
                p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g_ge_cur_fbo_slot = -1;
                g_ge_cur_is_msaa = 0;
            }
            f->stencil_alpha_dirty = 0;
            f->ss_newer = 1;
        }
    }
    free(c->tex);
}

static void ge_blit_cmd(const ge_cmd *c) {
    if (!c || c->w <= 0 || c->h <= 0) return;
    if (c->src_slot < 0 || c->src_slot >= GE_MAX_FBOS ||
        c->fbo_slot < 0 || c->fbo_slot >= GE_MAX_FBOS)
        return;
    ge_gpu_fbo *src = &g_ge_fbos[c->src_slot];
    ge_gpu_fbo *dst = &g_ge_fbos[c->fbo_slot];
    if (!src->used || !dst->used) return;
    ge_resolve_fbo_ex(src, 0);
    if (src != dst) ge_resolve_fbo_ex(dst, 0);
    if (src->stencil_alpha_dirty) ge_sync_stencil_to_alpha(src);
    ge_gl_state_invalidate();
    const int sx0 = c->sx * src->scale;
    const int sy0 = (src->h - (c->sy + c->h)) * src->scale;
    const int sw = c->w * src->scale;
    const int sh = c->h * src->scale;
    const int dx0 = c->dx * dst->scale;
    const int dy0 = (dst->h - (c->dy + c->h)) * dst->scale;
    const int dw = c->w * dst->scale;
    const int dh = c->h * dst->scale;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (c->src_slot == c->fbo_slot) {
        if (!g_ge_self_tex) glGenTextures(1, &g_ge_self_tex);
        glBindTexture(GL_TEXTURE_2D, g_ge_self_tex);
        if (g_ge_self_w != src->phys_w || g_ge_self_h != src->phys_h) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src->phys_w, src->phys_h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            g_ge_self_w = src->phys_w;
            g_ge_self_h = src->phys_h;
        }
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx0, sy0, sw, sh);
        if (!g_ge_blit_fbo) p_glGenFramebuffers(1, &g_ge_blit_fbo);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ge_blit_fbo);
        p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, g_ge_self_tex, 0);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
        p_glBlitFramebuffer(0, 0, sw, sh,
                            dx0, dy0, dx0 + dw, dy0 + dh,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
        p_glBlitFramebuffer(sx0, sy0, sx0 + sw, sy0 + sh,
                            dx0, dy0, dx0 + dw, dy0 + dh,
                            GL_COLOR_BUFFER_BIT,
                            src->scale == dst->scale ? GL_NEAREST : GL_LINEAR);
    }
    dst->stencil_alpha_dirty = 0;
    dst->ss_newer = 1;
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
}

static void ge_readback_cmd(const ge_cmd *c) {
    if (c && c->fbo_slot >= 0 && c->fbo_slot < GE_MAX_FBOS &&
        g_ge_fbos[c->fbo_slot].used && c->w > 0 && c->h > 0) {
        ge_gpu_fbo *f = &g_ge_fbos[c->fbo_slot];
        ge_resolve_fbo_ex(f, c->rb_ds != NULL ? 1 : 0);
        if (c->rb_rgba && !c->rb_ds && f->stencil_alpha_dirty)
            ge_sync_stencil_to_alpha(f);
        const int rw = c->w < f->w ? c->w : f->w;
        const int rh = c->h < f->h ? c->h : f->h;
        if (rw > 0 && rh > 0) {
            ge_gl_state_invalidate();
            if (f->scale == 1) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                if (c->rb_rgba)
                    glReadPixels(0, f->h - rh, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, c->rb_rgba);
                if (c->rb_ds)
                    glReadPixels(0, f->h - rh, rw, rh, GL_DEPTH_STENCIL,
                                 GL_UNSIGNED_INT_24_8, c->rb_ds);
            } else if (ge_ensure_stage(rw, rh)) {
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ge_stage_fbo);
                if (c->rb_rgba) {
                    p_glBlitFramebuffer(0, (f->h - rh) * f->scale,
                                        rw * f->scale, f->phys_h,
                                        0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
                if (c->rb_ds) {
                    glDepthMask(GL_TRUE);
                    glStencilMask(0xFFu);
                    p_glBlitFramebuffer(0, (f->h - rh) * f->scale,
                                        rw * f->scale, f->phys_h,
                                        0, 0, rw, rh,
                                        GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                        GL_NEAREST);
                }
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_stage_fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                if (c->rb_rgba)
                    glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, c->rb_rgba);
                if (c->rb_ds)
                    glReadPixels(0, 0, rw, rh, GL_DEPTH_STENCIL,
                                 GL_UNSIGNED_INT_24_8, c->rb_ds);
            }
            glFinish();
            p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            g_ge_cur_fbo_slot = -1;
            g_ge_cur_is_msaa = 0;
        }
    }
    if (g_ge_rb_done) SetEvent(g_ge_rb_done);
}

static void ge_service_front_readback(void) {
    if (!atomic_exchange_explicit(&g_ge_front_rb_req, 0, memory_order_acq_rel))
        return;
    int ok = 0;
    uint32_t tmp[GL_SCREEN_W * GL_SCREEN_H];
    EnterCriticalSection(&g_ge_lock);
    if (g_ge_have_front && !g_ge_passthrough_sw && g_ge_front_fbo) {
        if (g_ge_front_w == GL_SCREEN_W && g_ge_front_h == GL_SCREEN_H) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_front_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, GL_SCREEN_W, GL_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
            ok = 1;
        } else if (ge_ensure_stage(GL_SCREEN_W, GL_SCREEN_H)) {
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ge_front_fbo);
            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ge_stage_fbo);
            p_glBlitFramebuffer(0, 0, g_ge_front_w, g_ge_front_h,
                                0, 0, GL_SCREEN_W, GL_SCREEN_H,
                                GL_COLOR_BUFFER_BIT, GL_LINEAR);
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_ge_stage_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, GL_SCREEN_W, GL_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
            ok = 1;
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    LeaveCriticalSection(&g_ge_lock);
    g_ge_cur_fbo_slot = -1;
    g_ge_cur_is_msaa = 0;
    if (ok) {
        for (int y = 0; y < GL_SCREEN_H; y++) {
            const uint32_t *srow = tmp + (size_t)(GL_SCREEN_H - 1 - y) * GL_SCREEN_W;
            uint32_t *drow = g_ge_front_rb_buf + (size_t)y * GL_SCREEN_W;
            for (int x = 0; x < GL_SCREEN_W; x++)
                drow[x] = srow[x] | 0xFF000000u;
        }
    }
    g_ge_front_rb_ok = ok;
    if (g_ge_rb_done) SetEvent(g_ge_rb_done);
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
        GLint max_tex = 4096, max_rb = 4096;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
        g_ge_max_dim = max_tex < max_rb ? max_tex : max_rb;
        if (g_ge_max_dim < 1024) g_ge_max_dim = 1024;

        g_ge_msaa = 0;
        const char *ms = getenv("SETO_MSAA");
        if (ms && ms[0] && p_glRenderbufferStorageMultisample) {
            int want = atoi(ms);
            GLint max_s = 0;
            glGetIntegerv(GL_MAX_SAMPLES, &max_s);
            if (want >= 2 && max_s >= 2) {
                if (want > max_s) want = max_s;
                if (want >= 8) g_ge_msaa = 8;
                else if (want >= 4) g_ge_msaa = 4;
                else g_ge_msaa = 2;
            }
        }

        const char *s2d = getenv("SETO_SMART_2D");
        g_ge_smart_2d = (!s2d || !s2d[0] || s2d[0] != '0') ? 1 : 0;

        const char *rs = getenv("SETO_RENDER_SCALE");
        g_ge_auto_scale = 0;
        if (rs && rs[0]) {
            if ((rs[0] == 'a' || rs[0] == 'A') && (rs[1] == 'u' || rs[1] == 'U') &&
                (rs[2] == 't' || rs[2] == 'T') && (rs[3] == 'o' || rs[3] == 'O') &&
                rs[4] == '\0') {
                g_ge_auto_scale = 1;
                GetClientRect(g_hwnd, &rc);
                g_ge_scale = ge_auto_scale_for_client(rc.right - rc.left, rc.bottom - rc.top);
            } else {
                g_ge_scale = ge_clamp_scale(atoi(rs));
            }
        } else {
            const char *sdir = getenv("SETO_GL_SHOT_DIR");
            const char *wsc = getenv("SETO_SCALE");
            if (sdir && sdir[0]) {
                g_ge_scale = 1;
            } else if (wsc && atoi(wsc) > 0) {
                int s = atoi(wsc);
                if (s > 4) s = 4;
                g_ge_scale = ge_clamp_scale(s);
            } else {
                g_ge_scale = 1;
            }
        }
        g_ge_scale = ge_scale_for_dims(g_ge_scale, 512, 272);

        if (!ge_make_geo_program() || !ge_make_geo_vao() ||
            !ge_make_front_target(g_ge_scale)) {
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
            ge_service_front_readback();
            int popped = 0;
            while (ge_queue_pop(&g_ge_q, &cmd)) {
                if (popped < 4 && ge_debug_fbo_env()) {
                    fprintf(stderr, "gl: pop type=%u fbo=%d prim=%u count=%u v0=%d,%d "
                            "col=%08X slot=%d fbo_tex=%d tex=%p\n",
                            cmd.type, cmd.fbo_slot, cmd.prim, cmd.count,
                            cmd.verts[0].x, cmd.verts[0].y, cmd.verts[0].rgba,
                            cmd.tex_slot, cmd.fbo_tex_slot, (void *)cmd.tex);
                }
                popped++;
                if (cmd.type == GE_CMD_TARGET) {
                    ge_bind_or_create_fbo(&cmd);
                } else if (cmd.type == GE_CMD_DRAW) {
                    static psp_vertex g_merge_verts[4096];
                    if (cmd.prim == PSP_PRIM_TRIANGLES && !cmd.state.clear &&
                        cmd.fbo_tex_slot != cmd.fbo_slot && cmd.card_front_path_off < 0) {
                        const ge_cmd *nx = ge_queue_peek(&g_ge_q);
                        if (nx && nx->type == GE_CMD_DRAW &&
                            nx->prim == PSP_PRIM_TRIANGLES &&
                            !nx->state.clear && nx->tex == NULL &&
                            nx->card_front_path_off < 0 &&
                            nx->fbo_slot == cmd.fbo_slot &&
                            nx->tex_slot == cmd.tex_slot &&
                            nx->fbo_tex_slot == cmd.fbo_tex_slot &&
                            cmd.count + nx->count <= 4096u &&
                            memcmp(&cmd.state, &nx->state, sizeof(psp_draw_state)) == 0) {
                            uint32_t total = cmd.count;
                            memcpy(g_merge_verts, cmd.verts, (size_t)total * sizeof(psp_vertex));
                            do {
                                memcpy(&g_merge_verts[total], nx->verts,
                                       (size_t)nx->count * sizeof(psp_vertex));
                                total += nx->count;
                                ge_queue_drop(&g_ge_q);
                                nx = ge_queue_peek(&g_ge_q);
                            } while (nx && nx->type == GE_CMD_DRAW &&
                                     nx->prim == PSP_PRIM_TRIANGLES &&
                                     !nx->state.clear && nx->tex == NULL &&
                                     nx->card_front_path_off < 0 &&
                                     nx->fbo_slot == cmd.fbo_slot &&
                                     nx->tex_slot == cmd.tex_slot &&
                                     nx->fbo_tex_slot == cmd.fbo_tex_slot &&
                                     total + nx->count <= 4096u &&
                                     memcmp(&cmd.state, &nx->state, sizeof(psp_draw_state)) == 0);
                            ge_process_draw_verts(&cmd, g_merge_verts, total);
                        } else {
                            ge_process_draw_verts(&cmd, cmd.verts, cmd.count);
                        }
                    } else {
                        ge_process_draw_verts(&cmd, cmd.verts, cmd.count);
                    }
                    draws_since_present++;
                } else if (cmd.type == GE_CMD_UPLOAD_TARGET) {
                    ge_upload_target_cmd(&cmd);
                    draws_since_present++;
                } else if (cmd.type == GE_CMD_BLIT) {
                    ge_blit_cmd(&cmd);
                    draws_since_present++;
                } else if (cmd.type == GE_CMD_READBACK) {
                    const uint64_t t_rb0 = seto_perf_now_us();
                    ge_readback_cmd(&cmd);
                    seto_perf_add(&g_seto_perf.g_rb_us, seto_perf_now_us() - t_rb0);
                } else if (cmd.type == GE_CMD_RESIZE) {
                    ge_apply_resize(cmd.w);
                } else if (cmd.type == GE_CMD_PRESENT) {
                    const uint64_t t_p0 = seto_perf_now_us();
                    if (cmd.fbo_slot >= 0 && cmd.fbo_slot < GE_MAX_FBOS &&
                        g_ge_fbos[cmd.fbo_slot].used) {
                        ge_gpu_fbo *df = &g_ge_fbos[cmd.fbo_slot];
                        if (cmd.color) ge_capture_1x_ppm(cmd.color, df);
                        ge_publish_fbo_slot(cmd.fbo_slot);
                        if (cmd.color) ge_capture_hires_ppm(cmd.color);
                        free(cmd.tex);
                    } else if (cmd.tex) {
                        const char *sdir = ge_shot_dir_env();
                        if (cmd.color && sdir && sdir[0]) {
                            char path[260];
                            snprintf(path, sizeof path, "%s/gl-%05u.ppm", sdir, cmd.color);
                            ge_write_ppm_file(path, cmd.tex, GL_SCREEN_W, GL_SCREEN_H,
                                              GL_SCREEN_W, GL_SCREEN_H);
                        }
                        if (g_ge_front_w == GL_SCREEN_W && g_ge_front_h == GL_SCREEN_H) {
                            EnterCriticalSection(&g_ge_lock);
                            ge_wait_present_read_sync();
                            glBindTexture(GL_TEXTURE_2D, g_ge_front_tex);
                            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GL_SCREEN_W, GL_SCREEN_H,
                                            GL_RGBA, GL_UNSIGNED_BYTE, cmd.tex);
                            g_ge_have_front = 1;
                            g_ge_passthrough_sw = 0;
                            ge_record_front_sync();
                            LeaveCriticalSection(&g_ge_lock);
                            glBindTexture(GL_TEXTURE_2D, 0);
                        } else if (ge_ensure_stage(GL_SCREEN_W, GL_SCREEN_H)) {
                            glBindTexture(GL_TEXTURE_2D, g_ge_stage_tex);
                            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GL_SCREEN_W, GL_SCREEN_H,
                                            GL_RGBA, GL_UNSIGNED_BYTE, cmd.tex);
                            glBindTexture(GL_TEXTURE_2D, 0);
                            EnterCriticalSection(&g_ge_lock);
                            ge_wait_present_read_sync();
                            glDisable(GL_SCISSOR_TEST);
                            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ge_stage_fbo);
                            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ge_front_fbo);
                            p_glBlitFramebuffer(0, 0, GL_SCREEN_W, GL_SCREEN_H,
                                                0, 0, g_ge_front_w, g_ge_front_h,
                                                GL_COLOR_BUFFER_BIT, GL_LINEAR);
                            p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                            g_ge_have_front = 1;
                            g_ge_passthrough_sw = 0;
                            ge_record_front_sync();
                            LeaveCriticalSection(&g_ge_lock);
                            g_ge_cur_fbo_slot = -1;
                            g_ge_cur_is_msaa = 0;
                        }
                        if (cmd.color) ge_capture_hires_ppm(cmd.color);
                        free(cmd.tex);
                    } else {
                        EnterCriticalSection(&g_ge_lock);
                        g_ge_passthrough_sw = 1;
                        atomic_store_explicit(&g_ge_gpu_frame_ready, 0, memory_order_release);
                        LeaveCriticalSection(&g_ge_lock);
                    }
                    ge_service_front_readback();
                    draws_since_present = 0;
                    last_pub = GetTickCount();
                    seto_perf_add(&g_seto_perf.g_pres_us, seto_perf_now_us() - t_p0);
                } else if (cmd.type == GE_CMD_TEST_COLOR) {
                    g_ge_rgb = cmd.color;
                } else if (cmd.type == GE_CMD_QUIT) {
                    free(cmd.tex);
                    break;
                }
                ge_service_front_readback();
            }
            {
                int pending = atomic_exchange_explicit(&g_ge_pending_scale, 0,
                                                       memory_order_acq_rel);
                if (pending > 0)
                    ge_apply_resize(pending);
            }
            if (draws_since_present > 0 && !g_ge_have_front &&
                g_ge_cur_fbo_slot >= 0 && g_ge_fbos[g_ge_cur_fbo_slot].used &&
                GetTickCount() - last_pub >= 250u) {
                ge_publish_fbo_slot(g_ge_cur_fbo_slot);
                draws_since_present = 0;
                last_pub = GetTickCount();
            }
            ge_service_front_readback();
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

    if (g_ge_rb_done) SetEvent(g_ge_rb_done);
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
    if (g_ge_rb_done) {
        CloseHandle(g_ge_rb_done);
        g_ge_rb_done = NULL;
    }
    if (g_ge_wake) {
        CloseHandle(g_ge_wake);
        g_ge_wake = NULL;
    }
    if (g_ge_ready) {
        CloseHandle(g_ge_ready);
        g_ge_ready = NULL;
    }
    if (g_ge_rb_lock_ready) {
        DeleteCriticalSection(&g_ge_rb_lock);
        g_ge_rb_lock_ready = 0;
    }
    if (g_hd_ini_lock_ready) {
        EnterCriticalSection(&g_hd_ini_lock);
        ge_hd_ini_reset();
        LeaveCriticalSection(&g_hd_ini_lock);
        DeleteCriticalSection(&g_hd_ini_lock);
        g_hd_ini_lock_ready = 0;
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
    InitializeCriticalSection(&g_ge_rb_lock);
    g_ge_rb_lock_ready = 1;
    InitializeCriticalSection(&g_hd_ini_lock);
    g_hd_ini_lock_ready = 1;
    if (ge_queue_init(&g_ge_q, 2048) != 0) {
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
    g_ge_rb_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_ge_rb_done) {
        fprintf(stderr, "gl: GE readback event failed\n");
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
    fprintf(stderr, "gl: GE GPU %s backend active (%dx%d, scale=%dx%s, msaa=%dx, post=%d)\n",
            g_ge_draw_mode ? "draw" : "test", g_ge_w, g_ge_h,
            g_ge_scale, g_ge_auto_scale ? " auto" : "",
            g_ge_msaa, g_post_filter);
    return 1;
}

static void gl_cleanup(void) {
    ge_stop();
    if (g_ctx) {
        if (g_ovl_tex) glDeleteTextures(1, &g_ovl_tex);
        if (g_prog && p_glDeleteProgram) p_glDeleteProgram(g_prog);
        if (g_vao && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &g_vao);
        if (g_vbo && p_glDeleteBuffers) p_glDeleteBuffers(1, &g_vbo);
        if (g_tex) glDeleteTextures(1, &g_tex);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_ctx);
    }
    if (g_ovl_lock_ready) {
        DeleteCriticalSection(&g_ovl_lock);
        g_ovl_lock_ready = 0;
    }
    free(g_ovl_px);
    g_ovl_px = NULL;
    g_ovl_cap = 0;
    g_ovl_x = g_ovl_y = g_ovl_w = g_ovl_h = g_ovl_dirty = 0;
    g_ovl_tex = 0;
    g_ovl_tex_w = g_ovl_tex_h = 0;
    g_last_pres_seq = 0;
    g_last_pres_cw = g_last_pres_ch = 0;
    atomic_store_explicit(&g_present_force_redraw, 0, memory_order_release);
    atomic_store_explicit(&g_ovl_pending_redraw, 0, memory_order_release);
    ge_hd_async_shutdown();
    if (g_gdiplus_ready) {
        GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_ready = 0;
        g_gdiplus_token = 0;
    }
    if (g_dc && g_hwnd) ReleaseDC(g_hwnd, g_dc);
    g_prog = g_vao = g_vbo = g_tex = 0;
    g_ctx = NULL;
    g_dc = NULL;
    g_active = 0;
}

static int parse_post_filter(const char *s) {
    if (!s || !s[0]) return 0;
    if (strcmp(s, "1") == 0 || strcmp(s, "cas") == 0 || strcmp(s, "CAS") == 0) return 1;
    if (strcmp(s, "2") == 0 || strcmp(s, "lcd") == 0 || strcmp(s, "LCD") == 0) return 2;
    if (strcmp(s, "3") == 0 || strcmp(s, "crt") == 0 || strcmp(s, "CRT") == 0) return 3;
    int v = atoi(s);
    return (v >= 0 && v <= 3) ? v : 0;
}

int seto_gl_present_init(HWND hwnd) {
    const char *present = getenv("SETO_PRESENT");
    const char *f;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC dummy;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_ctx;

    if (!present || strcmp(present, "gl") != 0) return 0;
    g_seto_perf_enabled = getenv("SETO_PERF_LOG") != NULL ? 1 : 0;
    f = getenv("SETO_FILTER");
    g_filter = f ? atoi(f) : 0;
    if (g_filter < 0 || g_filter > 2) g_filter = 0;
    g_post_filter = parse_post_filter(getenv("SETO_POST_FILTER"));

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
    InitializeCriticalSection(&g_ovl_lock);
    g_ovl_lock_ready = 1;
    g_active = 1;
    {
        const char *ven = (const char *)glGetString(GL_VENDOR);
        const char *ren = (const char *)glGetString(GL_RENDERER);
        const char *ver = (const char *)glGetString(GL_VERSION);
        snprintf(g_gl_gpu_info, sizeof g_gl_gpu_info, "%s | %s | OpenGL %s",
                 ven ? ven : "?", ren ? ren : "?", ver ? ver : "?");
        fprintf(stderr, "gl: %s presenter active\n", g_gl_gpu_info);
    }

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

static void ge_draw_overlay(int cw, int ch) {
    if (!g_ovl_lock_ready || cw <= 0 || ch <= 0) return;
    int ox = 0, oy = 0, ow = 0, oh = 0;
    EnterCriticalSection(&g_ovl_lock);
    if (g_ovl_w > 0 && g_ovl_h > 0 && g_ovl_px) {
        if (!g_ovl_tex) glGenTextures(1, &g_ovl_tex);
        glBindTexture(GL_TEXTURE_2D, g_ovl_tex);
        if (g_ovl_dirty || g_ovl_tex_w != g_ovl_w || g_ovl_tex_h != g_ovl_h) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_ovl_w, g_ovl_h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, g_ovl_px);
            g_ovl_tex_w = g_ovl_w;
            g_ovl_tex_h = g_ovl_h;
            g_ovl_dirty = 0;
        }
        ox = g_ovl_x;
        oy = g_ovl_y;
        ow = g_ovl_w;
        oh = g_ovl_h;
    }
    LeaveCriticalSection(&g_ovl_lock);
    if (ow <= 0 || oh <= 0 || !g_ovl_tex) return;

    glViewport(ox, ch - (oy + oh), ow, oh);
    glEnable(GL_BLEND);
    p_glBlendEquation(GL_FUNC_ADD);
    p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                          GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g_prog);
    p_glUniform1i(g_u_mode, 0);
    p_glUniform1i(g_u_flip, 0);
    p_glUniform1i(g_u_post, 0);
    p_glUniform1i(g_u_overlay, 1);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void ge_capture_present_ppm(int cw, int ch) {
    static uint32_t pres_seq;
    const char *dir = ge_present_shot_dir_env();
    if (!dir || !dir[0] || cw <= 0 || ch <= 0) return;
    const uint32_t seq = ++pres_seq;
    const char *ev = getenv("SETO_GL_PRESENT_SHOT_EVERY");
    int every = ev ? atoi(ev) : 60;
    if (every <= 0) every = 60;
    if ((seq % (uint32_t)every) != 0u) return;
    const size_t npx = (size_t)cw * (size_t)ch;
    uint32_t *px = (uint32_t *)malloc(npx * 4u);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char path[260];
    snprintf(path, sizeof path, "%s/pres-%05u.ppm", dir, seq);
    ge_write_ppm_file(path, px, cw, ch, cw, ch);
    free(px);
}

void seto_gl_present_frame(HWND hwnd, const uint32_t *rgba) {
    RECT rc;
    int cw, ch, dw, dh;
    GLint filter;

    if (!g_active) return;
    (void)hwnd;

    if (g_ge_active) {
        if (g_ge_draw_mode) {
            static uint64_t last_pres_us;
            const uint64_t t_sw0 = seto_perf_now_us();
            int use_ge = 0;
            cw = 0; ch = 0;
            EnterCriticalSection(&g_ge_lock);
            use_ge = (g_ge_front_tex && g_ge_have_front && !g_ge_passthrough_sw);
            if (use_ge) {
                GetClientRect(g_hwnd, &rc);
                cw = rc.right - rc.left;
                ch = rc.bottom - rc.top;
                if (cw > 0 && ch > 0) {
                    const uint32_t cur_seq = atomic_load_explicit(&g_ge_front_seq, memory_order_acquire);
                    const int force = atomic_load_explicit(&g_present_force_redraw, memory_order_acquire);
                    const int ovl_ch = atomic_load_explicit(&g_ovl_pending_redraw, memory_order_acquire);
                    const char *pdir = ge_present_shot_dir_env();
                    if ((!pdir || !pdir[0]) && !force && !ovl_ch && cur_seq != 0u &&
                        cur_seq == g_last_pres_seq && cw == g_last_pres_cw && ch == g_last_pres_ch) {
                        LeaveCriticalSection(&g_ge_lock);
                        return;
                    }
                    atomic_store_explicit(&g_present_force_redraw, 0, memory_order_release);
                    atomic_store_explicit(&g_ovl_pending_redraw, 0, memory_order_release);
                    g_last_pres_seq = cur_seq;
                    g_last_pres_cw = cw;
                    g_last_pres_ch = ch;
                    if (g_ge_front_sync) {
                        if (p_glWaitSync) p_glWaitSync(g_ge_front_sync, 0, GL_TIMEOUT_IGNORED);
                        if (p_glDeleteSync) p_glDeleteSync(g_ge_front_sync);
                        g_ge_front_sync = NULL;
                    }
                    if ((int64_t)cw * 17 > (int64_t)ch * 30) {
                        dh = ch;
                        dw = (int)((int64_t)ch * 30 / 17);
                    } else {
                        dw = cw;
                        dh = (int)((int64_t)cw * 17 / 30);
                    }
                    int eff_mode = g_filter;
                    if (eff_mode >= 2 && g_ge_front_w >= dw && g_ge_front_h >= dh)
                        eff_mode = (g_ge_front_w == dw && g_ge_front_h == dh) ? 0 : 1;
                    glBindTexture(GL_TEXTURE_2D, g_ge_front_tex);
                    filter = eff_mode == 0 ? GL_NEAREST : GL_LINEAR;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    glViewport((cw - dw) / 2, (ch - dh) / 2, dw, dh);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    p_glUseProgram(g_prog);
                    p_glUniform2f(g_u_texel,
                                  1.0f / (GLfloat)(g_ge_front_w > 0 ? g_ge_front_w : GL_SCREEN_W),
                                  1.0f / (GLfloat)(g_ge_front_h > 0 ? g_ge_front_h : GL_SCREEN_H));
                    p_glUniform1i(g_u_mode, eff_mode);
                    p_glUniform1i(g_u_flip, 1);
                    p_glUniform1i(g_u_post, g_post_filter);
                    p_glUniform1i(g_u_overlay, 0);
                    p_glBindVertexArray(g_vao);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    if (g_present_read_sync && p_glDeleteSync) {
                        p_glDeleteSync(g_present_read_sync);
                        g_present_read_sync = NULL;
                    }
                    if (p_glFenceSync)
                        g_present_read_sync = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();
                }
            }
            LeaveCriticalSection(&g_ge_lock);
            if (use_ge) {
                if (cw > 0 && ch > 0) {
                    ge_draw_overlay(cw, ch);
                    ge_capture_present_ppm(cw, ch);
                    SwapBuffers(g_dc);
                    const uint64_t t_sw1 = seto_perf_now_us();
                    seto_perf_add(&g_seto_perf.swap_us, t_sw1 - t_sw0);
                    seto_perf_add(&g_seto_perf.pres_frames, 1);
                    if (last_pres_us) seto_perf_max(&g_seto_perf.pres_max_gap_us, t_sw1 - last_pres_us);
                    last_pres_us = t_sw1;
                }
                return;
            }
            /* Passthrough software frame (before first GE frame or during movies). */
            g_last_pres_seq = 0;
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
            p_glUniform1i(g_u_post, 0);
            p_glUniform1i(g_u_overlay, 0);
            p_glBindVertexArray(g_vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFlush();
        }
        LeaveCriticalSection(&g_ge_lock);
        ge_draw_overlay(cw, ch);
        SwapBuffers(g_dc);
        return;
    }

    present_sw_frame:
    if (!rgba) return;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
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
    p_glUniform1i(g_u_post, g_post_filter);
    p_glUniform1i(g_u_overlay, 0);
    p_glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ge_draw_overlay(cw, ch);
    ge_capture_present_ppm(cw, ch);
    SwapBuffers(g_dc);
}

int seto_ge_gpu_draw_mode(void) { return g_ge_draw_mode; }

static int ge_enqueue_wait(const ge_cmd *c, int max_ms) {
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready) return 0;
    uint64_t wait_t0 = 0;
    for (int i = 0; i < max_ms; i++) {
        if (g_ge_quit) return 0;
        if (ge_queue_push_ptr(&g_ge_q, c)) {
            const uint32_t d = atomic_load_explicit(&g_ge_q.head, memory_order_relaxed) -
                               atomic_load_explicit(&g_ge_q.tail, memory_order_relaxed);
            seto_perf_max(&g_seto_perf.q_max, (uint64_t)d);
            if (i > 0 && wait_t0)
                seto_perf_add(&g_seto_perf.p_qwait_us, seto_perf_now_us() - wait_t0);
            if (g_ge_wake) SetEvent(g_ge_wake);
            return 1;
        }
        if (i == 0) wait_t0 = seto_perf_now_us();
        Sleep(1);
    }
    if (wait_t0)
        seto_perf_add(&g_seto_perf.p_qwait_us, seto_perf_now_us() - wait_t0);
    return 0;
}

int seto_ge_gpu_enqueue_target(int fbo_slot, uint32_t addr, uint32_t stride,
                               int fmt, int w, int h, int force_new) {
    ge_cmd c;
    if (fbo_slot < 0 || fbo_slot >= GE_MAX_FBOS || w <= 0 || h <= 0) return 0;
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_TARGET;
    c.color = (uint32_t)(force_new ? 1 : 0);
    c.fbo_slot = fbo_slot;
    c.addr = addr;
    c.stride = stride;
    c.fmt = (uint32_t)fmt;
    c.w = w;
    c.h = h;
    c.tex_slot = -1;
    c.fbo_tex_slot = -1;
    return ge_enqueue_wait(&c, 1000);
}

int seto_ge_gpu_enqueue_draw(int prim, const psp_vertex *v, int count,
                             const psp_draw_state *st, int fbo_slot,
                             uint32_t *tex, int tex_slot, int fbo_tex_slot,
                             uint64_t tex_hash, int32_t card_front_path_off) {
    ge_cmd c;
    if (!v || !st || count <= 0 || count > GE_MAX_VERTS) return 0;
    if (fbo_slot < 0 || fbo_slot >= GE_MAX_FBOS) return 0;
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_DRAW;
    c.prim = (uint32_t)prim;
    c.count = (uint32_t)count;
    c.fbo_slot = fbo_slot;
    c.tex = tex;
    c.tex_slot = tex_slot;
    c.fbo_tex_slot = fbo_tex_slot;
    c.tex_hash = tex_hash;
    c.hd_path_off = tex ? ge_hd_resolve_texture(tex_hash, st) : -1;
    c.card_front_path_off = card_front_path_off;
    c.state = *st;
    memcpy(c.verts, v, (size_t)count * sizeof *v);
    return ge_enqueue_wait(&c, 1000);
}

int seto_ge_gpu_enqueue_upload(int fbo_slot, int w, int h, uint32_t *rgba) {
    ge_cmd c;
    if (!rgba || fbo_slot < 0 || fbo_slot >= GE_MAX_FBOS || w <= 0 || h <= 0)
        return 0;
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_UPLOAD_TARGET;
    c.fbo_slot = fbo_slot;
    c.w = w;
    c.h = h;
    c.tex = rgba;
    c.tex_slot = -1;
    c.fbo_tex_slot = -1;
    return ge_enqueue_wait(&c, 1000);
}

int seto_ge_gpu_enqueue_blit(int src_slot, int sx, int sy,
                             int dst_slot, int dx, int dy, int w, int h) {
    ge_cmd c;
    if (src_slot < 0 || src_slot >= GE_MAX_FBOS ||
        dst_slot < 0 || dst_slot >= GE_MAX_FBOS || w <= 0 || h <= 0)
        return 0;
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_BLIT;
    c.src_slot = src_slot;
    c.fbo_slot = dst_slot;
    c.sx = sx;
    c.sy = sy;
    c.dx = dx;
    c.dy = dy;
    c.w = w;
    c.h = h;
    c.tex_slot = -1;
    c.fbo_tex_slot = -1;
    return ge_enqueue_wait(&c, 1000);
}

int seto_ge_gpu_readback(int fbo_slot, int w, int h,
                         uint32_t *rgba, uint32_t *ds) {
    ge_cmd c;
    if (!g_ge_active || !g_ge_draw_mode || !g_ge_queue_ready || !g_ge_rb_lock_ready)
        return -1;
    if (fbo_slot < 0 || fbo_slot >= GE_MAX_FBOS || w <= 0 || h <= 0)
        return -1;
    EnterCriticalSection(&g_ge_rb_lock);
    ResetEvent(g_ge_rb_done);
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_READBACK;
    c.fbo_slot = fbo_slot;
    c.w = w;
    c.h = h;
    c.rb_rgba = rgba;
    c.rb_ds = ds;
    c.tex_slot = -1;
    c.fbo_tex_slot = -1;
    int pushed = ge_enqueue_wait(&c, 1000);
    DWORD wr = pushed ? WaitForSingleObject(g_ge_rb_done, 3000) : WAIT_FAILED;
    LeaveCriticalSection(&g_ge_rb_lock);
    return (pushed && wr == WAIT_OBJECT_0 && !g_ge_quit) ? 0 : -1;
}

void seto_ge_gpu_present(int disp_slot, uint32_t *ram_rgba, uint32_t capture_id) {
    ge_cmd c;
    memset(&c, 0, offsetof(ge_cmd, verts));
    c.type = GE_CMD_PRESENT;
    c.fbo_slot = disp_slot;
    c.tex = ram_rgba;
    c.color = capture_id;
    c.tex_slot = -1;
    c.fbo_tex_slot = -1;
    if (!ge_enqueue_wait(&c, 1000))
        free(ram_rgba);
}

int seto_ge_gpu_current_scale(void) {
    return g_ge_scale;
}

int seto_ge_gpu_current_msaa(void) {
    return g_ge_msaa;
}

void seto_perf_tick(double fps, double vps, int scale, int msaa) {
    if (!g_seto_perf_enabled) return;

    static ULONGLONG last_flush;
    static FILE *perf_fp;
    static int perf_init;
    const ULONGLONG now = GetTickCount64();
    if (last_flush && (now - last_flush) < 1000u) return;
    last_flush = now;

    if (!perf_init) {
        char log_path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, log_path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            char *sep = strrchr(log_path, '\\');
            char *sep2 = strrchr(log_path, '/');
            if (sep2 && (!sep || sep2 > sep)) sep = sep2;
            if (sep) strcpy(sep + 1, "perf.log");
            else strcpy(log_path, "perf.log");
        } else {
            strcpy(log_path, "perf.log");
        }
        perf_fp = fopen(log_path, "wb");
        if (perf_fp) {
            fprintf(perf_fp, "# Seto-static live performance log | GPU: %s\n", g_gl_gpu_info);
            fflush(perf_fp);
        }
        perf_init = 1;
    }

    const uint64_t p_draws     = atomic_exchange_explicit(&g_seto_perf.p_draws, 0, memory_order_relaxed);
    const uint64_t p_gen_hits  = atomic_exchange_explicit(&g_seto_perf.p_gen_hits, 0, memory_order_relaxed);
    const uint64_t p_raw_hits  = atomic_exchange_explicit(&g_seto_perf.p_raw_hits, 0, memory_order_relaxed);
    const uint64_t p_decodes   = atomic_exchange_explicit(&g_seto_perf.p_decodes, 0, memory_order_relaxed);
    const uint64_t p_sw_fb     = atomic_exchange_explicit(&g_seto_perf.p_sw_fb, 0, memory_order_relaxed);
    const uint64_t p_readbacks = atomic_exchange_explicit(&g_seto_perf.p_readbacks, 0, memory_order_relaxed);
    const uint64_t p_draw_us   = atomic_exchange_explicit(&g_seto_perf.p_draw_us, 0, memory_order_relaxed);
    const uint64_t p_hash_us   = atomic_exchange_explicit(&g_seto_perf.p_hash_us, 0, memory_order_relaxed);
    const uint64_t p_dec_us    = atomic_exchange_explicit(&g_seto_perf.p_dec_us, 0, memory_order_relaxed);
    const uint64_t p_rb_us     = atomic_exchange_explicit(&g_seto_perf.p_rb_us, 0, memory_order_relaxed);
    const uint64_t p_hd_us     = atomic_exchange_explicit(&g_seto_perf.p_hd_us, 0, memory_order_relaxed);
    const uint64_t p_qwait_us  = atomic_exchange_explicit(&g_seto_perf.p_qwait_us, 0, memory_order_relaxed);
    const uint64_t q_max       = atomic_exchange_explicit(&g_seto_perf.q_max, 0, memory_order_relaxed);

    const uint64_t g_draws     = atomic_exchange_explicit(&g_seto_perf.g_draws, 0, memory_order_relaxed);
    const uint64_t g_tex_up    = atomic_exchange_explicit(&g_seto_perf.g_tex_up, 0, memory_order_relaxed);
    const uint64_t hd_hits     = atomic_exchange_explicit(&g_seto_perf.hd_hits, 0, memory_order_relaxed);
    const uint64_t hd_loads    = atomic_exchange_explicit(&g_seto_perf.hd_loads, 0, memory_order_relaxed);
    const uint64_t hd_load_us  = atomic_exchange_explicit(&g_seto_perf.hd_load_us, 0, memory_order_relaxed);
    const uint64_t hd_miss_io  = atomic_exchange_explicit(&g_seto_perf.hd_miss_io, 0, memory_order_relaxed);
    const uint64_t hd_miss_us  = atomic_exchange_explicit(&g_seto_perf.hd_miss_us, 0, memory_order_relaxed);
    const uint64_t g_draw_us   = atomic_exchange_explicit(&g_seto_perf.g_draw_us, 0, memory_order_relaxed);
    const uint64_t g_pres_us   = atomic_exchange_explicit(&g_seto_perf.g_pres_us, 0, memory_order_relaxed);
    const uint64_t g_rb_us     = atomic_exchange_explicit(&g_seto_perf.g_rb_us, 0, memory_order_relaxed);

    const uint64_t pres_frames     = atomic_exchange_explicit(&g_seto_perf.pres_frames, 0, memory_order_relaxed);
    const uint64_t swap_us         = atomic_exchange_explicit(&g_seto_perf.swap_us, 0, memory_order_relaxed);
    const uint64_t pres_max_gap_us = atomic_exchange_explicit(&g_seto_perf.pres_max_gap_us, 0, memory_order_relaxed);

    if (!perf_fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(perf_fp,
            "[%02u:%02u:%02u] fps=%.1f vps=%.1f pres=%llu gapMax=%.1fms swap=%.1fms scale=%dx msaa=%dx | "
            "PROD draws=%llu gen=%llu raw=%llu dec=%llu sw=%llu rb=%llu draw=%.1fms hash=%.1fms dec=%.1fms hdRes=%.1fms qWait=%.1fms rb=%.1fms qMax=%llu | "
            "GPU draws=%llu texUp=%llu hdHit=%llu hdLoad=%llu(%.1fms) hdMiss=%llu(%.1fms) draw=%.1fms pres=%.1fms rb=%.1fms\n",
            st.wHour, st.wMinute, st.wSecond, fps, vps,
            (unsigned long long)pres_frames, (double)pres_max_gap_us / 1000.0,
            (double)swap_us / 1000.0, scale, msaa,
            (unsigned long long)p_draws, (unsigned long long)p_gen_hits,
            (unsigned long long)p_raw_hits, (unsigned long long)p_decodes,
            (unsigned long long)p_sw_fb, (unsigned long long)p_readbacks,
            (double)p_draw_us / 1000.0, (double)p_hash_us / 1000.0,
            (double)p_dec_us / 1000.0, (double)p_hd_us / 1000.0,
            (double)p_qwait_us / 1000.0, (double)p_rb_us / 1000.0,
            (unsigned long long)q_max,
            (unsigned long long)g_draws, (unsigned long long)g_tex_up,
            (unsigned long long)hd_hits, (unsigned long long)hd_loads,
            (double)hd_load_us / 1000.0, (unsigned long long)hd_miss_io,
            (double)hd_miss_us / 1000.0, (double)g_draw_us / 1000.0,
            (double)g_pres_us / 1000.0, (double)g_rb_us / 1000.0);
    fflush(perf_fp);
}

int seto_ge_gpu_set_scale(int scale) {
    if (!g_active || !g_ge_active || !g_ge_draw_mode) return 0;
    atomic_store_explicit(&g_ge_pending_scale, ge_clamp_scale(scale),
                          memory_order_release);
    if (g_ge_wake) SetEvent(g_ge_wake);
    return 1;
}

void seto_gl_present_resize(int client_w, int client_h) {
    if (!g_active || !g_ge_active || !g_ge_draw_mode || !g_ge_auto_scale)
        return;
    if (client_w <= 0 || client_h <= 0) return;
    const int want = ge_auto_scale_for_client(client_w, client_h);
    if (want != g_ge_scale)
        seto_ge_gpu_set_scale(want);
}

void seto_gl_present_shutdown(HWND hwnd) {
    (void)hwnd;
    gl_cleanup();
}

int seto_gl_present_active(void) {
    return g_active;
}

int seto_gl_present_has_gpu_frame(void) {
    return g_active && g_ge_active && g_ge_draw_mode &&
           atomic_load_explicit(&g_ge_gpu_frame_ready, memory_order_acquire) != 0;
}

int seto_ge_gpu_read_front(uint32_t *out_top_down) {
    if (!out_top_down || !g_ge_active || !g_ge_draw_mode ||
        !g_ge_rb_lock_ready || !g_ge_rb_done ||
        !atomic_load_explicit(&g_ge_gpu_frame_ready, memory_order_acquire))
        return -1;
    EnterCriticalSection(&g_ge_rb_lock);
    ResetEvent(g_ge_rb_done);
    g_ge_front_rb_ok = 0;
    atomic_store_explicit(&g_ge_front_rb_req, 1, memory_order_release);
    if (g_ge_wake) SetEvent(g_ge_wake);
    DWORD wr = WaitForSingleObject(g_ge_rb_done, 3000);
    int ok = (wr == WAIT_OBJECT_0 && !g_ge_quit && g_ge_front_rb_ok);
    if (ok)
        memcpy(out_top_down, g_ge_front_rb_buf, sizeof g_ge_front_rb_buf);
    LeaveCriticalSection(&g_ge_rb_lock);
    return ok ? 0 : -1;
}

void seto_gl_present_set_overlay(const uint32_t *rgba_top_down, int x, int y,
                                 int w, int h) {
    if (!g_active || !g_ovl_lock_ready) return;
    EnterCriticalSection(&g_ovl_lock);
    if (!rgba_top_down || w <= 0 || h <= 0) {
        g_ovl_w = 0;
        g_ovl_h = 0;
        g_ovl_dirty = 0;
    } else {
        const size_t npx = (size_t)w * (size_t)h;
        if (npx > g_ovl_cap) {
            uint32_t *p = (uint32_t *)realloc(g_ovl_px, npx * 4u);
            if (!p) {
                LeaveCriticalSection(&g_ovl_lock);
                return;
            }
            g_ovl_px = p;
            g_ovl_cap = npx;
        }
        memcpy(g_ovl_px, rgba_top_down, npx * 4u);
        g_ovl_x = x;
        g_ovl_y = y;
        g_ovl_w = w;
        g_ovl_h = h;
        g_ovl_dirty = 1;
    }
    LeaveCriticalSection(&g_ovl_lock);
    atomic_store_explicit(&g_ovl_pending_redraw, 1, memory_order_release);
}

void seto_gl_present_invalidate(void) {
    atomic_store_explicit(&g_present_force_redraw, 1, memory_order_release);
}
