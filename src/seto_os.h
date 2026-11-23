#ifndef SETO_OS_H
#define SETO_OS_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef CRITICAL_SECTION seto_mutex_t;
typedef CONDITION_VARIABLE seto_cond_t;
typedef HANDLE seto_event_t;
typedef HANDLE seto_thread_t;
typedef DWORD seto_thread_ret_t;
typedef LPVOID seto_thread_arg_t;
#define SETO_THREAD_CALL WINAPI

static inline void seto_mutex_init(seto_mutex_t *m)    { InitializeCriticalSection(m); }
static inline void seto_mutex_destroy(seto_mutex_t *m) { DeleteCriticalSection(m); }
static inline void seto_mutex_lock(seto_mutex_t *m)    { EnterCriticalSection(m); }
static inline void seto_mutex_unlock(seto_mutex_t *m)  { LeaveCriticalSection(m); }

static inline void seto_cond_init(seto_cond_t *c)      { InitializeConditionVariable(c); }
static inline void seto_cond_destroy(seto_cond_t *c)   { (void)c; }
static inline void seto_cond_wait(seto_cond_t *c, seto_mutex_t *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
static inline void seto_cond_signal(seto_cond_t *c)    { WakeConditionVariable(c); }
static inline void seto_cond_broadcast(seto_cond_t *c) { WakeAllConditionVariable(c); }

static inline int seto_event_init(seto_event_t *e, int manual_reset) {
    *e = CreateEventW(NULL, manual_reset ? TRUE : FALSE, FALSE, NULL);
    return *e != NULL ? 0 : -1;
}
static inline void seto_event_destroy(seto_event_t *e) {
    if (*e) { CloseHandle(*e); *e = NULL; }
}
static inline void seto_event_set(seto_event_t *e)   { if (*e) SetEvent(*e); }
static inline void seto_event_reset(seto_event_t *e) { if (*e) ResetEvent(*e); }
static inline int seto_event_wait_ms(seto_event_t *e, uint32_t timeout_ms) {
    if (!*e) return 0;
    return WaitForSingleObject(*e, timeout_ms == UINT32_MAX ? INFINITE : (DWORD)timeout_ms) == WAIT_OBJECT_0 ? 1 : 0;
}

static inline int seto_thread_create(seto_thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t != NULL ? 0 : -1;
}
static inline void seto_thread_join(seto_thread_t *t, uint32_t timeout_ms) {
    if (*t) {
        WaitForSingleObject(*t, timeout_ms == UINT32_MAX ? INFINITE : (DWORD)timeout_ms);
        CloseHandle(*t);
        *t = NULL;
    }
}
static inline void seto_thread_detach(seto_thread_t t) {
    if (t) CloseHandle(t);
}

static inline void seto_sleep_ms(uint32_t ms)  { Sleep((DWORD)ms); }
static inline uint64_t seto_ticks_ms(void)     { return (uint64_t)GetTickCount64(); }
static inline void seto_mkdir(const char *dir) { CreateDirectoryA(dir, NULL); }

#else /* !_WIN32 */

#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

typedef pthread_mutex_t seto_mutex_t;
typedef pthread_cond_t seto_cond_t;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             signaled;
    int             manual_reset;
    int             valid;
} seto_event_t;
typedef struct {
    pthread_t th;
    int       valid;
} seto_thread_t;
typedef void *seto_thread_ret_t;
typedef void *seto_thread_arg_t;
#define SETO_THREAD_CALL

#ifndef _stricmp
#define _stricmp strcasecmp
#endif

static inline void seto_mutex_init(seto_mutex_t *m)    { pthread_mutex_init(m, NULL); }
static inline void seto_mutex_destroy(seto_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void seto_mutex_lock(seto_mutex_t *m)    { pthread_mutex_lock(m); }
static inline void seto_mutex_unlock(seto_mutex_t *m)  { pthread_mutex_unlock(m); }

static inline void seto_cond_init(seto_cond_t *c)      { pthread_cond_init(c, NULL); }
static inline void seto_cond_destroy(seto_cond_t *c)   { pthread_cond_destroy(c); }
static inline void seto_cond_wait(seto_cond_t *c, seto_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static inline void seto_cond_signal(seto_cond_t *c)    { pthread_cond_signal(c); }
static inline void seto_cond_broadcast(seto_cond_t *c) { pthread_cond_broadcast(c); }

static inline int seto_event_init(seto_event_t *e, int manual_reset) {
    pthread_condattr_t attr;
    if (pthread_mutex_init(&e->m, NULL) != 0) return -1;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&e->c, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&e->m);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    e->signaled = 0;
    e->manual_reset = manual_reset ? 1 : 0;
    e->valid = 1;
    return 0;
}
static inline void seto_event_destroy(seto_event_t *e) {
    if (e->valid) {
        pthread_cond_destroy(&e->c);
        pthread_mutex_destroy(&e->m);
        e->valid = 0;
    }
}
static inline void seto_event_set(seto_event_t *e) {
    if (!e->valid) return;
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    if (e->manual_reset) pthread_cond_broadcast(&e->c);
    else pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}
static inline void seto_event_reset(seto_event_t *e) {
    if (!e->valid) return;
    pthread_mutex_lock(&e->m);
    e->signaled = 0;
    pthread_mutex_unlock(&e->m);
}
static inline int seto_event_wait_ms(seto_event_t *e, uint32_t timeout_ms) {
    if (!e->valid) return 0;
    pthread_mutex_lock(&e->m);
    if (timeout_ms == UINT32_MAX) {
        while (!e->signaled)
            pthread_cond_wait(&e->c, &e->m);
    } else if (!e->signaled && timeout_ms > 0u) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(timeout_ms / 1000u);
        ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (!e->signaled) {
            int rc = pthread_cond_timedwait(&e->c, &e->m, &ts);
            if (rc == ETIMEDOUT) break;
        }
    }
    int ok = e->signaled ? 1 : 0;
    if (ok && !e->manual_reset) e->signaled = 0;
    pthread_mutex_unlock(&e->m);
    return ok;
}

static inline int seto_thread_create(seto_thread_t *t, void *(*fn)(void *), void *arg) {
    t->valid = (pthread_create(&t->th, NULL, fn, arg) == 0);
    return t->valid ? 0 : -1;
}
static inline void seto_thread_join(seto_thread_t *t, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (t->valid) {
        pthread_join(t->th, NULL);
        t->valid = 0;
    }
}
static inline void seto_thread_detach(seto_thread_t t) {
    if (t.valid) pthread_detach(t.th);
}

static inline void seto_sleep_ms(uint32_t ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
}
static inline uint64_t seto_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
static inline void seto_mkdir(const char *dir) {
    mkdir(dir, 0755);
}

#endif /* _WIN32 */

#endif /* SETO_OS_H */
