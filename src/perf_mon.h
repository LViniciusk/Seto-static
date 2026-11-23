#ifndef SETO_PERF_MON_H
#define SETO_PERF_MON_H

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    _Atomic uint64_t p_draws;
    _Atomic uint64_t p_gen_hits;
    _Atomic uint64_t p_raw_hits;
    _Atomic uint64_t p_decodes;
    _Atomic uint64_t p_sw_fb;
    _Atomic uint64_t p_readbacks;
    _Atomic uint64_t p_draw_us;
    _Atomic uint64_t p_hash_us;
    _Atomic uint64_t p_dec_us;
    _Atomic uint64_t p_rb_us;
    _Atomic uint64_t p_hd_us;
    _Atomic uint64_t p_qwait_us;
    _Atomic uint64_t q_max;

    _Atomic uint64_t g_draws;
    _Atomic uint64_t g_tex_up;
    _Atomic uint64_t hd_hits;
    _Atomic uint64_t hd_loads;
    _Atomic uint64_t hd_load_us;
    _Atomic uint64_t hd_miss_io;
    _Atomic uint64_t hd_miss_us;
    _Atomic uint64_t g_draw_us;
    _Atomic uint64_t g_pres_us;
    _Atomic uint64_t g_rb_us;

    _Atomic uint64_t pres_frames;
    _Atomic uint64_t swap_us;
    _Atomic uint64_t pres_max_gap_us;
} seto_perf_counters;

extern seto_perf_counters g_seto_perf;
extern int g_seto_perf_enabled;

void seto_perf_tick(double fps, double vps, int scale, int msaa);

static inline uint64_t seto_perf_now_us(void) {
    if (!g_seto_perf_enabled) return 0;
#ifdef _WIN32
    static uint64_t freq;
    LARGE_INTEGER li;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart > 0 ? (uint64_t)f.QuadPart : 1000000ULL;
    }
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart * 1000000ULL / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
#endif
}

static inline void seto_perf_add(_Atomic uint64_t *dst, uint64_t v) {
    if (!g_seto_perf_enabled) return;
    atomic_fetch_add_explicit(dst, v, memory_order_relaxed);
}

static inline void seto_perf_max(_Atomic uint64_t *dst, uint64_t v) {
    if (!g_seto_perf_enabled) return;
    uint64_t cur = atomic_load_explicit(dst, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(dst, &cur, v,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

#endif
