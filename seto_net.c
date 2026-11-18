/*
 * seto_net.c — Seto-static HLE networking client for Seto-Server.
 *
 * Portable across Win32 (MinGW/MSVC + ws2_32) and POSIX (Linux/macOS + pthreads).
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET seto_sock_t;
#define SETO_INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int seto_sock_t;
#define SETO_INVALID_SOCK (-1)
#endif

#include "seto_net.h"
#include "seto_proto.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Portable Locking & Threading Primitives
 * -------------------------------------------------------------------------- */

#ifdef _WIN32
typedef CRITICAL_SECTION seto_mutex_t;
static void mtx_init(seto_mutex_t *m)   { InitializeCriticalSection(m); }
static void mtx_lock(seto_mutex_t *m)   { EnterCriticalSection(m); }
static void mtx_unlock(seto_mutex_t *m) { LeaveCriticalSection(m); }
static void sock_close(seto_sock_t s)   { if (s != INVALID_SOCKET) closesocket(s); }
static uint64_t host_ms(void)           { return (uint64_t)GetTickCount64(); }
#else
typedef pthread_mutex_t seto_mutex_t;
static void mtx_init(seto_mutex_t *m)   { pthread_mutex_init(m, NULL); }
static void mtx_lock(seto_mutex_t *m)   { pthread_mutex_lock(m); }
static void mtx_unlock(seto_mutex_t *m) { pthread_mutex_unlock(m); }
static void sock_close(seto_sock_t s)   { if (s >= 0) close(s); }
static uint64_t host_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
#endif

static int sock_set_nonblock(seto_sock_t s, int nb) {
#ifdef _WIN32
    u_long mode = nb ? 1UL : 0UL;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0 ? 0 : -1;
#endif
}

static int sock_wait(seto_sock_t s, int want_write, int timeout_ms) {
#ifdef _WIN32
    fd_set rset, wset, eset;
    FD_ZERO(&rset);
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    if (want_write) FD_SET(s, &wset);
    else FD_SET(s, &rset);
    FD_SET(s, &eset);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, want_write ? NULL : &rset, want_write ? &wset : NULL, &eset, &tv);
    if (rc <= 0 || FD_ISSET(s, &eset)) return -1;
    return 1;
#else
    struct pollfd pfd;
    pfd.fd      = s;
    pfd.events  = want_write ? POLLOUT : POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    return 1;
#endif
}

/* --------------------------------------------------------------------------
 * Limits & State Structures
 * -------------------------------------------------------------------------- */

#define NET_MAX_PEERS         64
#define NET_MAX_SCAN_GROUPS   32
#define NET_MAX_CTL_HANDLERS  8
#define NET_MAX_MATCH_SLOTS   8
#define NET_MAX_PDP_SOCKS     32
#define NET_MAX_PDP_QUEUE     64
#define NET_MAX_PTP_SOCKS     32
#define NET_MAX_PTP_BACKLOG   8
#define NET_MAX_CB_EVENTS     256

#define PSP_ADHOC_ERROR_NOT_ENOUGH_SPACE 0x8041070Au
#define PSP_UTILITY_STATUS_NONE          0
#define PSP_UTILITY_STATUS_INIT          1
#define PSP_UTILITY_STATUS_RUNNING       2
#define PSP_UTILITY_STATUS_QUIT          3
#define PSP_UTILITY_STATUS_SHUTDOWN      4

/* Guest memory block layout: 16 KB callback stack + 4 KB scratch buffer */
#define CB_GUEST_STACK_BYTES   16384u
#define CB_GUEST_SCRATCH_BYTES 4096u
#define CB_GUEST_TOTAL_BYTES   (CB_GUEST_STACK_BYTES + CB_GUEST_SCRATCH_BYTES)

typedef struct {
    int      used;
    int      id;
    uint32_t fn;
    uint32_t arg;
    uint32_t gp;
    uint32_t k0;
} ctl_handler_t;

typedef struct {
    int      used;
    int      running;
    int      id;
    int      mode;
    int      max_peers;
    uint16_t port;
    uint32_t hello_delay_us;
    uint32_t cb_fn;
    uint32_t cb_gp;
    uint32_t cb_k0;
    uint16_t hello_len;
    uint8_t  hello_opt[SETO_MAX_OPT_LEN];
} match_slot_t;

typedef struct pdp_pkt {
    uint8_t  src_mac[SETO_MAC_LEN];
    uint16_t src_port;
    uint16_t len;
    uint8_t *data;
} pdp_pkt_t;

typedef struct {
    int       used;
    int       id;
    uint8_t   mac[SETO_MAC_LEN];
    uint16_t  port;
    uint32_t  bufsize;
    pdp_pkt_t q[NET_MAX_PDP_QUEUE];
    int       q_head;
    int       q_count;
    uint32_t  rcvd_bytes;
} pdp_sock_t;

enum ptp_sock_kind {
    PTP_KIND_UNUSED   = 0,
    PTP_KIND_LISTENER = 1,
    PTP_KIND_STREAM   = 2
};

typedef struct {
    uint32_t stream_id;
    uint8_t  peer_mac[SETO_MAC_LEN];
    uint16_t peer_port;
} ptp_pending_inc_t;

typedef struct {
    int      kind;
    int      id;
    uint16_t local_port;
    uint8_t  peer_mac[SETO_MAC_LEN];
    uint16_t peer_port;
    uint32_t stream_id;
    int      connected;
    int      peer_closed;
    uint32_t close_reason;

    /* For PTP_KIND_LISTENER */
    ptp_pending_inc_t inc[NET_MAX_PTP_BACKLOG];
    int               inc_head;
    int               inc_count;

    /* For PTP_KIND_STREAM */
    uint8_t *rx_buf;
    size_t   rx_len;
    size_t   rx_cap;
    uint32_t sent_total;
    uint32_t rcvd_total;
} ptp_sock_t;

enum cb_ev_kind {
    CB_EV_ADHOCCTL = 1,
    CB_EV_MATCHING = 2
};

typedef struct {
    int      kind;
    int      id;        /* matchingId for MATCHING */
    int      event;     /* event/flag code */
    uint32_t error;     /* error code for ADHOCCTL */
    uint8_t  mac[SETO_MAC_LEN];
    uint16_t opt_len;
    uint8_t *opt_data;
} cb_event_t;

/* --------------------------------------------------------------------------
 * Module Globals
 * -------------------------------------------------------------------------- */

static int               g_locks_inited;
static seto_mutex_t      g_state_lock;
static seto_mutex_t      g_tx_lock;

static seto_sock_t       g_sock = SETO_INVALID_SOCK;
static volatile int      g_online;
static volatile int      g_rx_stop;
static int               g_offline_logged;
static int               g_rx_thread_live;
#ifdef _WIN32
static HANDLE            g_rx_thread;
#else
static pthread_t         g_rx_thread;
#endif

static uint32_t          g_session_id;
static uint8_t           g_local_mac[SETO_MAC_LEN] = { 0x02u, 0x00u, 0x5Eu, 0x54u, 0x4Fu, 0x01u };
static char              g_product[SETO_PRODUCT_LEN + 1] = "ULUS10136";
static char              g_nickname[SETO_NICKNAME_LEN + 1] = "Seto";

static int               g_netconf_status;
static uint32_t          g_netconf_param;
static volatile int      g_join_confirmed;

static int               g_adhocctl_state;
static char              g_adhoc_group[SETO_GROUP_NAME_LEN];
static uint8_t           g_group_bssid[SETO_MAC_LEN];
static uint16_t          g_group_channel = 1u;

static seto_peer_entry_t g_peers[NET_MAX_PEERS];
static int               g_peer_count;

static seto_scan_entry_t g_scan_groups[NET_MAX_SCAN_GROUPS];
static int               g_scan_count;

static ctl_handler_t     g_ctl_handlers[NET_MAX_CTL_HANDLERS];
static int               g_next_ctl_id = 1;

static match_slot_t      g_match[NET_MAX_MATCH_SLOTS];
static int               g_next_match_id = 1;

static pdp_sock_t        g_pdp[NET_MAX_PDP_SOCKS];
static int               g_next_pdp_id = 1;

static ptp_sock_t        g_ptp[NET_MAX_PTP_SOCKS];
static int               g_next_ptp_id = 1;

static cb_event_t        g_cb_q[NET_MAX_CB_EVENTS];
static int               g_cb_head;
static int               g_cb_count;
static int               g_cb_delivering;
static uint32_t          g_cb_guest_mem;

static void ensure_locks(void) {
    if (g_locks_inited) return;
#ifdef _WIN32
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    mtx_init(&g_state_lock);
    mtx_init(&g_tx_lock);
    g_locks_inited = 1;
}

static int trace_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SETO_TRACE_NET");
        v = (e && atoi(e) > 0) ? 1 : 0;
    }
    return v;
}

/* --------------------------------------------------------------------------
 * Config Discovery (env vars + launcher.ini / seto_net.ini)
 * -------------------------------------------------------------------------- */

static void trim_inplace(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static int read_ini_key(const char *path, const char *key, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        trim_inplace(k);
        trim_inplace(v);
        if (strlen(k) == klen && strncmp(k, key, klen) == 0 && v[0] != '\0') {
            snprintf(out, cap, "%s", v);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int load_server_endpoint(char *host_out, size_t host_cap, uint16_t *port_out) {
    char raw[256] = "";
    const char *env_srv = getenv("SETO_SERVER");
    if (env_srv && env_srv[0]) {
        snprintf(raw, sizeof(raw), "%s", env_srv);
    } else if (!read_ini_key("seto_net.ini", "server", raw, sizeof(raw)) &&
               !read_ini_key("launcher.ini", "server", raw, sizeof(raw))) {
        return 0;
    }
    trim_inplace(raw);
    if (!raw[0]) return 0;

    *port_out = SETO_DEFAULT_PORT;
    char *colon = strrchr(raw, ':');
    if (colon && colon != raw) {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) *port_out = (uint16_t)p;
    }
    snprintf(host_out, host_cap, "%s", raw);

    /* Load optional nickname override */
    char nick[SETO_NICKNAME_LEN + 1] = "";
    const char *env_nick = getenv("SETO_NICKNAME");
    if (env_nick && env_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", env_nick);
    } else if (!read_ini_key("seto_net.ini", "nickname", nick, sizeof(nick)) &&
               !read_ini_key("launcher.ini", "nickname", nick, sizeof(nick))) {
        const char *u = getenv("USERNAME");
        if (!u || !u[0]) u = getenv("USER");
        if (u && u[0]) snprintf(nick, sizeof(nick), "%s", u);
    }
    if (nick[0]) {
        snprintf(g_nickname, sizeof(g_nickname), "%s", nick);
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Wire Send / Recv Helpers
 * -------------------------------------------------------------------------- */

static int raw_send_all(seto_sock_t s, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
#ifdef _WIN32
        int n = send(s, (const char *)p, (int)len, 0);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                if (sock_wait(s, 1, 2000) < 0) return -1;
                continue;
            }
            return -1;
        }
#else
        ssize_t n = send(s, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sock_wait(s, 1, 2000) < 0) return -1;
                continue;
            }
            return -1;
        }
#endif
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int raw_recv_all(seto_sock_t s, void *buf, size_t len, int timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        if (g_rx_stop) return -1;
#ifdef _WIN32
        int n = recv(s, (char *)p, (int)len, 0);
        if (n == 0) return -1;
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                if (sock_wait(s, 0, timeout_ms) < 0) return -1;
                continue;
            }
            return -1;
        }
#else
        ssize_t n = recv(s, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sock_wait(s, 0, timeout_ms) < 0) return -1;
                continue;
            }
            return -1;
        }
#endif
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int net_send2(uint8_t opcode, const void *p1, size_t n1, const void *p2, size_t n2) {
    if (!g_online || g_sock == SETO_INVALID_SOCK) return -1;
    seto_hdr_t hdr = {
        .magic   = SETO_PROTO_MAGIC,
        .version = SETO_PROTO_VERSION,
        .opcode  = opcode,
        .length  = (uint32_t)(n1 + n2)
    };
    mtx_lock(&g_tx_lock);
    int rc = 0;
    if (!g_online || g_sock == SETO_INVALID_SOCK) {
        rc = -1;
    } else {
        if (raw_send_all(g_sock, &hdr, sizeof(hdr)) < 0) rc = -1;
        if (rc == 0 && n1 > 0 && raw_send_all(g_sock, p1, n1) < 0) rc = -1;
        if (rc == 0 && n2 > 0 && raw_send_all(g_sock, p2, n2) < 0) rc = -1;
        if (rc < 0) g_online = 0;
    }
    mtx_unlock(&g_tx_lock);
    return rc;
}

static int net_send(uint8_t opcode, const void *p, size_t n) {
    return net_send2(opcode, p, n, NULL, 0);
}

/* --------------------------------------------------------------------------
 * Callback Queue Management (locked by g_state_lock)
 * -------------------------------------------------------------------------- */

static void enqueue_ctl_event_locked(int event, uint32_t error) {
    if (g_cb_count >= NET_MAX_CB_EVENTS) return;
    int idx = (g_cb_head + g_cb_count) % NET_MAX_CB_EVENTS;
    memset(&g_cb_q[idx], 0, sizeof(g_cb_q[idx]));
    g_cb_q[idx].kind  = CB_EV_ADHOCCTL;
    g_cb_q[idx].event = event;
    g_cb_q[idx].error = error;
    g_cb_count++;
}

static void enqueue_match_event_locked(int matching_id, int event,
                                       const uint8_t mac[SETO_MAC_LEN],
                                       const uint8_t *opt, uint16_t opt_len) {
    if (g_cb_count >= NET_MAX_CB_EVENTS) return;
    uint8_t *copy = NULL;
    if (opt_len > 0 && opt) {
        copy = (uint8_t *)malloc(opt_len);
        if (!copy) return;
        memcpy(copy, opt, opt_len);
    }
    int idx = (g_cb_head + g_cb_count) % NET_MAX_CB_EVENTS;
    memset(&g_cb_q[idx], 0, sizeof(g_cb_q[idx]));
    g_cb_q[idx].kind     = CB_EV_MATCHING;
    g_cb_q[idx].id       = matching_id;
    g_cb_q[idx].event    = event;
    if (mac) memcpy(g_cb_q[idx].mac, mac, SETO_MAC_LEN);
    g_cb_q[idx].opt_len  = copy ? opt_len : 0;
    g_cb_q[idx].opt_data = copy;
    g_cb_count++;
}

/* --------------------------------------------------------------------------
 * Incoming Frame Processing (called on background RX thread)
 * -------------------------------------------------------------------------- */

static void handle_rx_frame(uint8_t opcode, const uint8_t *payload, uint32_t len) {
    switch (opcode) {
    case SETO_OP_PING:
        (void)net_send(SETO_OP_PONG, payload, len);
        break;

    case SETO_OP_ADHOCCTL_JOINED:
        if (len >= sizeof(seto_adhocctl_joined_t)) {
            seto_adhocctl_joined_t j;
            memcpy(&j, payload, sizeof(j));
            mtx_lock(&g_state_lock);
            memcpy(g_adhoc_group, j.group_name, SETO_GROUP_NAME_LEN);
            memcpy(g_group_bssid, j.bssid, SETO_MAC_LEN);
            g_group_channel  = j.channel ? j.channel : 1u;
            g_adhocctl_state = PSP_ADHOCCTL_STATE_CONNECTED;
            g_join_confirmed = 1;
            enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_CONNECT, 0);
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_ADHOCCTL_LEFT:
        mtx_lock(&g_state_lock);
        g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
        g_peer_count     = 0;
        enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_DISCONNECT, 0);
        mtx_unlock(&g_state_lock);
        break;

    case SETO_OP_PEER_SNAPSHOT:
        if (len >= sizeof(seto_peer_snapshot_t)) {
            seto_peer_snapshot_t snap;
            memcpy(&snap, payload, sizeof(snap));
            uint32_t avail = (len - (uint32_t)sizeof(snap)) / (uint32_t)sizeof(seto_peer_entry_t);
            uint16_t cnt = snap.peer_count;
            if (cnt > avail) cnt = (uint16_t)avail;
            if (cnt > NET_MAX_PEERS) cnt = NET_MAX_PEERS;

            mtx_lock(&g_state_lock);
            g_peer_count = (int)cnt;
            if (cnt > 0) {
                memcpy(g_peers, payload + sizeof(snap), (size_t)cnt * sizeof(seto_peer_entry_t));
            }
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_SCAN_RSP:
        if (len >= sizeof(seto_scan_rsp_t)) {
            seto_scan_rsp_t sr;
            memcpy(&sr, payload, sizeof(sr));
            uint32_t avail = (len - (uint32_t)sizeof(sr)) / (uint32_t)sizeof(seto_scan_entry_t);
            uint16_t cnt = sr.group_count;
            if (cnt > avail) cnt = (uint16_t)avail;
            if (cnt > NET_MAX_SCAN_GROUPS) cnt = NET_MAX_SCAN_GROUPS;

            mtx_lock(&g_state_lock);
            g_scan_count = (int)cnt;
            if (cnt > 0) {
                memcpy(g_scan_groups, payload + sizeof(sr), (size_t)cnt * sizeof(seto_scan_entry_t));
            }
            if (g_adhocctl_state == PSP_ADHOCCTL_STATE_SCANNING)
                g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
            enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_SCAN, 0);
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_MATCH_EVENT:
        if (len >= sizeof(seto_match_event_t)) {
            seto_match_event_t ev;
            memcpy(&ev, payload, sizeof(ev));
            if (sizeof(ev) + ev.opt_len <= len && ev.opt_len <= SETO_MAX_OPT_LEN) {
                const uint8_t *opt = ev.opt_len ? (payload + sizeof(ev)) : NULL;
                mtx_lock(&g_state_lock);
                for (int i = 0; i < NET_MAX_MATCH_SLOTS; i++) {
                    if (g_match[i].used && g_match[i].running &&
                        (g_match[i].id == (int)ev.client_matching_id || g_match[i].port == ev.port)) {
                        enqueue_match_event_locked(g_match[i].id, (int)ev.event,
                                                   ev.peer_mac, opt, ev.opt_len);
                        break;
                    }
                }
                mtx_unlock(&g_state_lock);
            }
        }
        break;

    case SETO_OP_PDP_RECV:
        if (len >= sizeof(seto_pdp_recv_t)) {
            seto_pdp_recv_t rx;
            memcpy(&rx, payload, sizeof(rx));
            if (sizeof(rx) + rx.data_len <= len) {
                const uint8_t *data = rx.data_len ? (payload + sizeof(rx)) : NULL;
                mtx_lock(&g_state_lock);
                for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
                    pdp_sock_t *s = &g_pdp[i];
                    if (!s->used || s->port != rx.dst_port) continue;
                    if (s->q_count < NET_MAX_PDP_QUEUE) {
                        uint8_t *copy = NULL;
                        if (rx.data_len > 0) {
                            copy = (uint8_t *)malloc(rx.data_len);
                            if (!copy) break;
                            memcpy(copy, data, rx.data_len);
                        }
                        int idx = (s->q_head + s->q_count) % NET_MAX_PDP_QUEUE;
                        memcpy(s->q[idx].src_mac, rx.src_mac, SETO_MAC_LEN);
                        s->q[idx].src_port = rx.src_port;
                        s->q[idx].len      = rx.data_len;
                        s->q[idx].data     = copy;
                        s->q_count++;
                        s->rcvd_bytes += rx.data_len;
                    }
                }
                mtx_unlock(&g_state_lock);
            }
        }
        break;

    case SETO_OP_PTP_INCOMING:
        if (len >= sizeof(seto_ptp_incoming_t)) {
            seto_ptp_incoming_t inc;
            memcpy(&inc, payload, sizeof(inc));
            mtx_lock(&g_state_lock);
            for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
                ptp_sock_t *s = &g_ptp[i];
                if (s->kind == PTP_KIND_LISTENER &&
                    (s->id == (int)inc.client_listen_id || s->local_port == inc.local_port)) {
                    if (s->inc_count < NET_MAX_PTP_BACKLOG) {
                        int idx = (s->inc_head + s->inc_count) % NET_MAX_PTP_BACKLOG;
                        s->inc[idx].stream_id = inc.stream_id;
                        memcpy(s->inc[idx].peer_mac, inc.peer_mac, SETO_MAC_LEN);
                        s->inc[idx].peer_port = inc.peer_port;
                        s->inc_count++;
                    }
                    break;
                }
            }
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_PTP_CONNECTED:
        if (len >= sizeof(seto_ptp_connected_t)) {
            seto_ptp_connected_t con;
            memcpy(&con, payload, sizeof(con));
            mtx_lock(&g_state_lock);
            for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
                ptp_sock_t *s = &g_ptp[i];
                if (s->kind == PTP_KIND_STREAM && s->id == (int)con.client_sock_id) {
                    s->stream_id = con.stream_id;
                    s->connected = 1;
                    break;
                }
            }
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_PTP_DATA:
        if (len >= sizeof(seto_ptp_data_t)) {
            seto_ptp_data_t dh;
            memcpy(&dh, payload, sizeof(dh));
            if (dh.data_len > 0 && sizeof(dh) + dh.data_len <= len) {
                const uint8_t *src = payload + sizeof(dh);
                mtx_lock(&g_state_lock);
                for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
                    ptp_sock_t *s = &g_ptp[i];
                    if (s->kind != PTP_KIND_STREAM || s->id != (int)dh.client_sock_id) continue;
                    if (s->rx_len + dh.data_len > s->rx_cap) {
                        size_t ncap = s->rx_cap ? s->rx_cap * 2u : 8192u;
                        while (ncap < s->rx_len + dh.data_len) ncap *= 2u;
                        uint8_t *nb = (uint8_t *)realloc(s->rx_buf, ncap);
                        if (!nb) break;
                        s->rx_buf = nb;
                        s->rx_cap = ncap;
                    }
                    memcpy(s->rx_buf + s->rx_len, src, dh.data_len);
                    s->rx_len += dh.data_len;
                    s->rcvd_total += dh.data_len;
                    break;
                }
                mtx_unlock(&g_state_lock);
            }
        }
        break;

    case SETO_OP_PTP_CLOSED:
        if (len >= sizeof(seto_ptp_closed_t)) {
            seto_ptp_closed_t cls;
            memcpy(&cls, payload, sizeof(cls));
            mtx_lock(&g_state_lock);
            for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
                ptp_sock_t *s = &g_ptp[i];
                if (s->kind == PTP_KIND_STREAM && s->id == (int)cls.client_sock_id) {
                    s->peer_closed  = 1;
                    s->close_reason = cls.reason;
                    break;
                }
            }
            mtx_unlock(&g_state_lock);
        }
        break;

    case SETO_OP_ERROR:
        if (len >= sizeof(seto_error_t)) {
            seto_error_t err;
            memcpy(&err, payload, sizeof(err));
            err.message[sizeof(err.message) - 1] = '\0';
            fprintf(stderr, "seto_net: server error 0x%08X (op 0x%02X): %s\n",
                    err.psp_error, err.ref_opcode, err.message);
        }
        break;

    default:
        break;
    }
}

#ifdef _WIN32
static DWORD WINAPI rx_thread_main(LPVOID arg)
#else
static void *rx_thread_main(void *arg)
#endif
{
    (void)arg;
    uint8_t *payload = NULL;
    uint32_t cap = 0;

    while (!g_rx_stop && g_online && g_sock != SETO_INVALID_SOCK) {
        if (sock_wait(g_sock, 0, 100) < 0) {
            /* Timeout is fine; inspect if socket is still valid */
            if (g_rx_stop) break;
            continue;
        }
        seto_hdr_t hdr;
        if (raw_recv_all(g_sock, &hdr, sizeof(hdr), 2000) < 0) {
            break;
        }
        if (hdr.magic != SETO_PROTO_MAGIC || hdr.version != SETO_PROTO_VERSION ||
            hdr.length > SETO_MAX_PAYLOAD) {
            fprintf(stderr, "seto_net: bad frame header from server\n");
            break;
        }
        if (hdr.length > 0) {
            if (hdr.length > cap) {
                uint8_t *np = (uint8_t *)realloc(payload, hdr.length);
                if (!np) break;
                payload = np;
                cap = hdr.length;
            }
            if (raw_recv_all(g_sock, payload, hdr.length, 3000) < 0) break;
        }
        handle_rx_frame(hdr.opcode, payload, hdr.length);
    }

    free(payload);
    g_online = 0;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* --------------------------------------------------------------------------
 * Connection Lifecycle
 * -------------------------------------------------------------------------- */

static void ensure_connected(void) {
    ensure_locks();
    if (g_online && g_sock != SETO_INVALID_SOCK) return;
    if (g_rx_thread_live) {
        g_rx_stop = 1;
#ifdef _WIN32
        WaitForSingleObject(g_rx_thread, 2000);
        CloseHandle(g_rx_thread);
#else
        pthread_join(g_rx_thread, NULL);
#endif
        g_rx_thread_live = 0;
    }
    if (g_sock != SETO_INVALID_SOCK) {
        sock_close(g_sock);
        g_sock = SETO_INVALID_SOCK;
    }

    char host[256];
    uint16_t port = SETO_DEFAULT_PORT;
    if (!load_server_endpoint(host, sizeof(host), &port)) {
        if (!g_offline_logged) {
            g_offline_logged = 1;
            fprintf(stderr, "seto_net: offline mode (set SETO_SERVER=<host>:%u or server=<host>:%u in launcher.ini)\n",
                    SETO_DEFAULT_PORT, SETO_DEFAULT_PORT);
        }
        return;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res = NULL, *it;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "seto_net: cannot resolve '%s:%u'; using offline fallback\n", host, port);
        return;
    }

    seto_sock_t s = SETO_INVALID_SOCK;
    for (it = res; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == SETO_INVALID_SOCK) continue;
        (void)sock_set_nonblock(s, 1);

        int rc = connect(s, it->ai_addr, (socklen_t)it->ai_addrlen);
        if (rc != 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            int in_prog = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
            int in_prog = (errno == EINPROGRESS);
#endif
            if (!in_prog || sock_wait(s, 1, 1500) < 0) {
                sock_close(s);
                s = SETO_INVALID_SOCK;
                continue;
            }
            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &slen) < 0 || so_err != 0) {
                sock_close(s);
                s = SETO_INVALID_SOCK;
                continue;
            }
        }
        int one = 1;
        (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        break;
    }
    freeaddrinfo(res);

    if (s == SETO_INVALID_SOCK) {
        fprintf(stderr, "seto_net: connection to %s:%u failed; using offline fallback\n", host, port);
        return;
    }

    /* Synchronous HELLO -> WELCOME handshake before starting RX thread */
    seto_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    memcpy(hello.product, g_product, SETO_PRODUCT_LEN);
    memcpy(hello.nickname, g_nickname, SETO_NICKNAME_LEN);
    memcpy(hello.preferred_mac, g_local_mac, SETO_MAC_LEN);

    seto_hdr_t hdr = {
        .magic   = SETO_PROTO_MAGIC,
        .version = SETO_PROTO_VERSION,
        .opcode  = SETO_OP_HELLO,
        .length  = sizeof(hello)
    };
    if (raw_send_all(s, &hdr, sizeof(hdr)) < 0 ||
        raw_send_all(s, &hello, sizeof(hello)) < 0) {
        sock_close(s);
        return;
    }

    seto_hdr_t rhdr;
    seto_welcome_t wel;
    if (sock_wait(s, 0, 1500) < 0 ||
        raw_recv_all(s, &rhdr, sizeof(rhdr), 1500) < 0 ||
        rhdr.magic != SETO_PROTO_MAGIC ||
        rhdr.opcode != SETO_OP_WELCOME ||
        rhdr.length != sizeof(wel) ||
        raw_recv_all(s, &wel, sizeof(wel), 1500) < 0) {
        fprintf(stderr, "seto_net: invalid WELCOME from %s:%u\n", host, port);
        sock_close(s);
        return;
    }

    g_sock       = s;
    g_session_id = wel.session_id;
    memcpy(g_local_mac, wel.assigned_mac, SETO_MAC_LEN);
    g_rx_stop    = 0;
    g_online     = 1;

#ifdef _WIN32
    g_rx_thread = CreateThread(NULL, 0, rx_thread_main, NULL, 0, NULL);
    g_rx_thread_live = (g_rx_thread != NULL);
#else
    g_rx_thread_live = (pthread_create(&g_rx_thread, NULL, rx_thread_main, NULL) == 0);
#endif

    fprintf(stderr, "seto_net: connected to %s:%u as '%s' (session=%u mac=%02x:%02x:%02x:%02x:%02x:%02x)\n",
            host, port, g_nickname, g_session_id,
            g_local_mac[0], g_local_mac[1], g_local_mac[2],
            g_local_mac[3], g_local_mac[4], g_local_mac[5]);
}

int seto_net_is_online(void) {
    return g_online ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * Safe Guest Callback Dispatcher (CPU Baton Owner Only)
 * -------------------------------------------------------------------------- */

void seto_net_pump_callbacks(void) {
    if (!g_locks_inited || g_cb_delivering) return;
    if (psp_threadman_intr_disabled()) return;

    mtx_lock(&g_state_lock);
    if (g_cb_count == 0) {
        mtx_unlock(&g_state_lock);
        return;
    }
    mtx_unlock(&g_state_lock);

    if (!g_cb_guest_mem) {
        g_cb_guest_mem = psp_sysmem_alloc(CB_GUEST_TOTAL_BYTES, 1);
        if (!g_cb_guest_mem) return;
    }

    g_cb_delivering = 1;
    const uint32_t stack_top = (g_cb_guest_mem + CB_GUEST_STACK_BYTES - 32u) & ~15u;
    const uint32_t scratch   = g_cb_guest_mem + CB_GUEST_STACK_BYTES;

    for (int budget = 0; budget < 32; budget++) {
        cb_event_t ev;
        ctl_handler_t ctl_snap[NET_MAX_CTL_HANDLERS];
        int nctl = 0;
        uint32_t match_fn = 0, match_gp = 0, match_k0 = 0;

        mtx_lock(&g_state_lock);
        if (g_cb_count == 0) {
            mtx_unlock(&g_state_lock);
            break;
        }
        ev = g_cb_q[g_cb_head];
        memset(&g_cb_q[g_cb_head], 0, sizeof(g_cb_q[g_cb_head]));
        g_cb_head = (g_cb_head + 1) % NET_MAX_CB_EVENTS;
        g_cb_count--;

        if (ev.kind == CB_EV_ADHOCCTL) {
            for (int i = 0; i < NET_MAX_CTL_HANDLERS; i++) {
                if (g_ctl_handlers[i].used && g_ctl_handlers[i].fn)
                    ctl_snap[nctl++] = g_ctl_handlers[i];
            }
        } else if (ev.kind == CB_EV_MATCHING) {
            for (int i = 0; i < NET_MAX_MATCH_SLOTS; i++) {
                if (g_match[i].used && g_match[i].id == ev.id) {
                    match_fn = g_match[i].cb_fn;
                    match_gp = g_match[i].cb_gp;
                    match_k0 = g_match[i].cb_k0;
                    break;
                }
            }
        }
        mtx_unlock(&g_state_lock);

        if (ev.kind == CB_EV_ADHOCCTL) {
            for (int i = 0; i < nctl; i++) {
                if (!psp_lookup(ctl_snap[i].fn)) continue;
                psp_cpu_state saved = psp_cpu;
                psp_cpu.r[PSP_REG_SP] = stack_top;
                if (ctl_snap[i].gp) psp_cpu.r[PSP_REG_GP] = ctl_snap[i].gp;
                if (ctl_snap[i].k0) psp_cpu.r[PSP_REG_K0] = ctl_snap[i].k0;
                psp_cpu.r[PSP_REG_A0] = (uint32_t)ev.event;
                psp_cpu.r[PSP_REG_A1] = ev.error;
                psp_cpu.r[PSP_REG_A2] = ctl_snap[i].arg;
                psp_cpu.r[PSP_REG_RA] = 0;
                psp_call_from_host(ctl_snap[i].fn);
                psp_cpu = saved;
            }
        } else if (ev.kind == CB_EV_MATCHING && match_fn && psp_lookup(match_fn)) {
            uint32_t mac_ptr = scratch;
            for (uint32_t b = 0; b < SETO_MAC_LEN; b++)
                psp_write8(mac_ptr + b, ev.mac[b]);

            uint32_t opt_ptr = 0;
            uint16_t copy_len = ev.opt_len;
            if (copy_len > (CB_GUEST_SCRATCH_BYTES - 32u))
                copy_len = (uint16_t)(CB_GUEST_SCRATCH_BYTES - 32u);
            if (copy_len > 0 && ev.opt_data) {
                opt_ptr = scratch + 16u;
                for (uint16_t b = 0; b < copy_len; b++)
                    psp_write8(opt_ptr + b, ev.opt_data[b]);
            }

            if (trace_enabled()) {
                fprintf(stderr, "seto_net: matching cb(id=%d, ev=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x, opt=%u) -> 0x%08X\n",
                        ev.id, ev.event,
                        ev.mac[0], ev.mac[1], ev.mac[2], ev.mac[3], ev.mac[4], ev.mac[5],
                        copy_len, match_fn);
            }

            psp_cpu_state saved = psp_cpu;
            psp_cpu.r[PSP_REG_SP] = stack_top;
            if (match_gp) psp_cpu.r[PSP_REG_GP] = match_gp;
            if (match_k0) psp_cpu.r[PSP_REG_K0] = match_k0;
            psp_cpu.r[PSP_REG_A0] = (uint32_t)ev.id;
            psp_cpu.r[PSP_REG_A1] = (uint32_t)ev.event;
            psp_cpu.r[PSP_REG_A2] = mac_ptr;
            psp_cpu.r[PSP_REG_A3] = (uint32_t)copy_len;
            psp_cpu.r[PSP_REG_T0] = opt_ptr; /* Allegrex EABI 5th argument */
            psp_write32(stack_top + 16u, opt_ptr);
            psp_cpu.r[PSP_REG_RA] = 0;
            psp_call_from_host(match_fn);
            psp_cpu = saved;
        }

        free(ev.opt_data);
    }

    g_cb_delivering = 0;
}

static void clear_runtime_queues_locked(void) {
    while (g_cb_count > 0) {
        free(g_cb_q[g_cb_head].opt_data);
        memset(&g_cb_q[g_cb_head], 0, sizeof(g_cb_q[g_cb_head]));
        g_cb_head = (g_cb_head + 1) % NET_MAX_CB_EVENTS;
        g_cb_count--;
    }
    for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
        while (g_pdp[i].q_count > 0) {
            free(g_pdp[i].q[g_pdp[i].q_head].data);
            g_pdp[i].q_head = (g_pdp[i].q_head + 1) % NET_MAX_PDP_QUEUE;
            g_pdp[i].q_count--;
        }
        memset(&g_pdp[i], 0, sizeof(g_pdp[i]));
    }
    for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
        free(g_ptp[i].rx_buf);
        memset(&g_ptp[i], 0, sizeof(g_ptp[i]));
    }
    memset(g_match, 0, sizeof(g_match));
    g_peer_count = 0;
    g_scan_count = 0;
}

void seto_net_shutdown(void) {
    if (!g_locks_inited) return;
    g_rx_stop = 1;
    g_online  = 0;
    if (g_sock != SETO_INVALID_SOCK) {
        sock_close(g_sock);
        g_sock = SETO_INVALID_SOCK;
    }
    if (g_rx_thread_live) {
#ifdef _WIN32
        WaitForSingleObject(g_rx_thread, 2000);
        CloseHandle(g_rx_thread);
#else
        pthread_join(g_rx_thread, NULL);
#endif
        g_rx_thread_live = 0;
    }
    mtx_lock(&g_state_lock);
    clear_runtime_queues_locked();
    g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
    mtx_unlock(&g_state_lock);

    if (g_cb_guest_mem) {
        psp_sysmem_release(g_cb_guest_mem);
        g_cb_guest_mem = 0;
    }
}

/* --------------------------------------------------------------------------
 * PSP HLE Syscall Implementations
 * -------------------------------------------------------------------------- */

static void hle_wlan_on(void) { psp_ret(1u); }
static void hle_net_ok(void)  { psp_ret(0u); }

static void hle_net_init(void) {
    ensure_connected();
    if (!g_cb_guest_mem)
        g_cb_guest_mem = psp_sysmem_alloc(CB_GUEST_TOTAL_BYTES, 1);
    psp_ret(0u);
}

static void hle_net_term(void) {
    /* Keep the persistent TCP session and assigned MAC alive across lobby
     * re-entries: modlabo caches sceWlanGetEtherAddr at startup and rejects
     * subsequent sessions if sceNetGetLocalEtherAddr changes after sceNetTerm! */
    if (g_online && g_adhocctl_state != PSP_ADHOCCTL_STATE_DISCONNECTED)
        (void)net_send(SETO_OP_ADHOCCTL_LEAVE, NULL, 0);
    mtx_lock(&g_state_lock);
    clear_runtime_queues_locked();
    g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
    g_netconf_status = PSP_UTILITY_STATUS_NONE;
    memset(g_adhoc_group, 0, sizeof(g_adhoc_group));
    memset(g_ctl_handlers, 0, sizeof(g_ctl_handlers));
    mtx_unlock(&g_state_lock);
    g_offline_logged = 0;
    psp_ret(0u);
}

static void hle_net_mac(void) {
    ensure_connected();
    uint32_t dst = psp_arg(0);
    if (!dst) {
        psp_ret(PSP_NET_ERROR_INVALID_ARG);
        return;
    }
    for (uint32_t i = 0; i < SETO_MAC_LEN; i++)
        psp_write8(dst + i, g_local_mac[i]);
    psp_ret(0u);
}

static void hle_net_ether_ntostr(void) {
    uint32_t src = psp_arg(0);
    uint32_t dst = psp_arg(1);
    if (!src || !dst) {
        psp_ret(PSP_NET_ERROR_INVALID_ARG);
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             psp_read8(src + 0u), psp_read8(src + 1u), psp_read8(src + 2u),
             psp_read8(src + 3u), psp_read8(src + 4u), psp_read8(src + 5u));
    for (uint32_t i = 0; buf[i]; i++) psp_write8(dst + i, (uint8_t)buf[i]);
    psp_write8(dst + 17u, 0);
    psp_ret(0u);
}

static void hle_net_ether_strton(void) {
    uint32_t src = psp_arg(0);
    uint32_t dst = psp_arg(1);
    if (!src || !dst) {
        psp_ret(PSP_NET_ERROR_INVALID_ARG);
        return;
    }
    char s[24];
    psp_str(src, s, sizeof(s));
    unsigned int b[6] = { 0 };
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (uint32_t i = 0; i < 6u; i++) psp_write8(dst + i, (uint8_t)b[i]);
    }
    psp_ret(0u);
}

/* ---- sceUtilityNetconf* -------------------------------------------------- */

static void join_adhoc_group_common(const char group[SETO_GROUP_NAME_LEN], uint8_t action) {
    ensure_connected();
    mtx_lock(&g_state_lock);
    memcpy(g_adhoc_group, group, SETO_GROUP_NAME_LEN);
    memcpy(g_group_bssid, g_local_mac, SETO_MAC_LEN);
    g_join_confirmed = 0;
    mtx_unlock(&g_state_lock);

    if (g_online) {
        seto_adhocctl_join_t req;
        memset(&req, 0, sizeof(req));
        memcpy(req.group_name, group, SETO_GROUP_NAME_LEN);
        req.action = action;
        if (net_send(SETO_OP_ADHOCCTL_JOIN, &req, sizeof(req)) == 0)
            return;
    }

    /* Offline fallback: mark connected locally */
    mtx_lock(&g_state_lock);
    g_adhocctl_state = PSP_ADHOCCTL_STATE_CONNECTED;
    g_join_confirmed = 1;
    enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_CONNECT, 0);
    mtx_unlock(&g_state_lock);
}

static void hle_netconf_init_start(void) {
    ensure_locks();
    uint32_t param = psp_arg(0);
    g_netconf_param = param;
    if (param) {
        psp_write32(param + 0x1Cu, 0u);
        uint32_t action = psp_read32(param + 0x30u);
        if (action >= 2u && action <= 5u) {
            char grp[SETO_GROUP_NAME_LEN] = { 0 };
            uint32_t adhocp = psp_read32(param + 0x34u);
            if (adhocp) {
                for (uint32_t i = 0; i < SETO_GROUP_NAME_LEN; i++)
                    grp[i] = (char)psp_read8(adhocp + i);
            }
            join_adhoc_group_common(grp, (uint8_t)action);
        }
    }
    g_netconf_status = PSP_UTILITY_STATUS_INIT;
    psp_ret(0u);
}

static void hle_netconf_get_status(void) {
    seto_net_pump_callbacks();
    int cur = g_netconf_status;
    if (cur == PSP_UTILITY_STATUS_INIT) g_netconf_status = PSP_UTILITY_STATUS_RUNNING;
    else if (cur == PSP_UTILITY_STATUS_SHUTDOWN) g_netconf_status = PSP_UTILITY_STATUS_NONE;
    psp_ret((uint32_t)cur);
}

static void hle_netconf_update(void) {
    if (g_netconf_status == PSP_UTILITY_STATUS_RUNNING) {
        if (g_online && !g_join_confirmed) {
            /* Wait briefly across frames for Seto-Server's ADHOCCTL_JOINED */
            uint64_t t0 = host_ms();
            while (g_online && !g_join_confirmed && (host_ms() - t0) < 50ULL)
                psp_threadman_host_sleep_ms(4u);
        }
        if (g_netconf_param) psp_write32(g_netconf_param + 0x1Cu, 0u);
        if (!g_online && g_netconf_param) {
            uint32_t action = psp_read32(g_netconf_param + 0x30u);
            if (action >= 2u && action <= 5u)
                g_adhocctl_state = PSP_ADHOCCTL_STATE_CONNECTED;
        }
        g_netconf_status = PSP_UTILITY_STATUS_QUIT;
    }
    seto_net_pump_callbacks();
    psp_ret(0u);
}

static void hle_netconf_shutdown_start(void) {
    g_netconf_status = PSP_UTILITY_STATUS_SHUTDOWN;
    psp_ret(0u);
}

/* ---- sceNetAdhocctl* ----------------------------------------------------- */

static void hle_adhocctl_init(void) {
    ensure_locks();
    uint32_t prod_ptr = psp_arg(2);
    if (prod_ptr) {
        char prod[10] = { 0 };
        for (uint32_t i = 0; i < 9u; i++) {
            char c = (char)psp_read8(prod_ptr + 4u + i);
            if (!c) break;
            prod[i] = c;
        }
        if (prod[0] &&
            strcmp(prod, "ULJM05151") != 0 &&
            strcmp(prod, "ULES00600") != 0) {
            snprintf(g_product, sizeof(g_product), "%s", prod);
        }
    }
    ensure_connected();
    psp_ret(0u);
}

static void hle_adhocctl_term(void) {
    if (g_online && g_adhocctl_state != PSP_ADHOCCTL_STATE_DISCONNECTED)
        (void)net_send(SETO_OP_ADHOCCTL_LEAVE, NULL, 0);
    mtx_lock(&g_state_lock);
    g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
    memset(g_adhoc_group, 0, sizeof(g_adhoc_group));
    memset(g_ctl_handlers, 0, sizeof(g_ctl_handlers));
    g_peer_count = 0;
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void hle_adhocctl_connect_common(uint8_t action) {
    uint32_t name_ptr = psp_arg(0);
    char grp[SETO_GROUP_NAME_LEN] = { 0 };
    if (name_ptr) {
        for (uint32_t i = 0; i < SETO_GROUP_NAME_LEN; i++) {
            char c = (char)psp_read8(name_ptr + i);
            if (!c) break;
            grp[i] = c;
        }
    }
    join_adhoc_group_common(grp, action);
    psp_ret(0u);
}

static void hle_adhocctl_connect(void) { hle_adhocctl_connect_common(PSP_NETCONF_ACTION_CONNECT_ADHOC); }
static void hle_adhocctl_create(void)  { hle_adhocctl_connect_common(PSP_NETCONF_ACTION_CREATE_ADHOC); }

static void hle_adhocctl_join(void) {
    uint32_t scan_ptr = psp_arg(0);
    char grp[SETO_GROUP_NAME_LEN] = { 0 };
    if (scan_ptr) {
        /* SceNetAdhocctlScanInfo: next(4) + channel(4) + name(8) */
        for (uint32_t i = 0; i < SETO_GROUP_NAME_LEN; i++)
            grp[i] = (char)psp_read8(scan_ptr + 8u + i);
    }
    join_adhoc_group_common(grp, PSP_NETCONF_ACTION_JOIN_ADHOC);
    psp_ret(0u);
}

static void hle_adhocctl_disconnect(void) {
    if (g_online) {
        (void)net_send(SETO_OP_ADHOCCTL_LEAVE, NULL, 0);
    } else {
        mtx_lock(&g_state_lock);
        g_adhocctl_state = PSP_ADHOCCTL_STATE_DISCONNECTED;
        memset(g_adhoc_group, 0, sizeof(g_adhoc_group));
        g_peer_count = 0;
        enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_DISCONNECT, 0);
        mtx_unlock(&g_state_lock);
    }
    seto_net_pump_callbacks();
    psp_ret(0u);
}

static void hle_adhocctl_add_handler(void) {
    ensure_locks();
    uint32_t fn  = psp_arg(0);
    uint32_t arg = psp_arg(1);
    int id = -1;

    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_CTL_HANDLERS; i++) {
        if (!g_ctl_handlers[i].used) {
            id = g_next_ctl_id++;
            g_ctl_handlers[i].used = 1;
            g_ctl_handlers[i].id   = id;
            g_ctl_handlers[i].fn   = fn;
            g_ctl_handlers[i].arg  = arg;
            g_ctl_handlers[i].gp   = psp_cpu.r[PSP_REG_GP];
            g_ctl_handlers[i].k0   = psp_cpu.r[PSP_REG_K0];
            break;
        }
    }
    mtx_unlock(&g_state_lock);
    psp_ret(id > 0 ? (uint32_t)id : PSP_ADHOC_ERROR_INVALID_ARG);
}

static void hle_adhocctl_del_handler(void) {
    ensure_locks();
    int id = (int)psp_arg(0);
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_CTL_HANDLERS; i++) {
        if (g_ctl_handlers[i].used && g_ctl_handlers[i].id == id) {
            memset(&g_ctl_handlers[i], 0, sizeof(g_ctl_handlers[i]));
            break;
        }
    }
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void hle_adhocctl_get_state(void) {
    seto_net_pump_callbacks();
    uint32_t out = psp_arg(0);
    if (!out) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    mtx_lock(&g_state_lock);
    int st = g_adhocctl_state;
    mtx_unlock(&g_state_lock);
    psp_write32(out, (uint32_t)st);
    psp_ret(0u);
}

static void hle_adhocctl_get_parameter(void) {
    uint32_t out = psp_arg(0);
    if (!out) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    mtx_lock(&g_state_lock);
    for (uint32_t i = 0; i < 148u; i++) psp_write8(out + i, 0);
    psp_write32(out + 0u, g_group_channel);
    for (uint32_t i = 0; i < 8u; i++) psp_write8(out + 4u + i, (uint8_t)g_adhoc_group[i]);
    for (uint32_t i = 0; i < 127u && g_nickname[i]; i++)
        psp_write8(out + 12u + i, (uint8_t)g_nickname[i]);
    const uint8_t *bssid = seto_mac_is_zero(g_group_bssid) ? g_local_mac : g_group_bssid;
    for (uint32_t i = 0; i < 6u; i++) psp_write8(out + 140u + i, bssid[i]);
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void hle_adhocctl_scan(void) {
    ensure_connected();
    if (g_online) {
        mtx_lock(&g_state_lock);
        if (g_adhocctl_state == PSP_ADHOCCTL_STATE_DISCONNECTED)
            g_adhocctl_state = PSP_ADHOCCTL_STATE_SCANNING;
        mtx_unlock(&g_state_lock);
        (void)net_send(SETO_OP_SCAN_REQ, NULL, 0);
    } else {
        mtx_lock(&g_state_lock);
        g_scan_count = 0;
        enqueue_ctl_event_locked(PSP_ADHOCCTL_EVENT_SCAN, 0);
        mtx_unlock(&g_state_lock);
    }
    psp_ret(0u);
}

static void hle_adhocctl_get_scan_info(void) {
    seto_net_pump_callbacks();
    uint32_t len_ptr = psp_arg(0);
    uint32_t buf_ptr = psp_arg(1);
    if (!len_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    /* SceNetAdhocctlScanInfo: next(4) + channel(4) + name(8) + bssid(6) + pad(2) + unk(4) = 28 bytes */
    const uint32_t entry_sz = 28u;
    mtx_lock(&g_state_lock);
    uint32_t needed = (uint32_t)g_scan_count * entry_sz;
    if (!buf_ptr) {
        psp_write32(len_ptr, needed);
        mtx_unlock(&g_state_lock);
        psp_ret(0u);
        return;
    }
    uint32_t max_bytes = psp_read32(len_ptr);
    uint32_t count = max_bytes / entry_sz;
    if (count > (uint32_t)g_scan_count) count = (uint32_t)g_scan_count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = buf_ptr + i * entry_sz;
        for (uint32_t b = 0; b < entry_sz; b++) psp_write8(p + b, 0);
        psp_write32(p + 0u, (i + 1u < count) ? (p + entry_sz) : 0u);
        psp_write32(p + 4u, g_scan_groups[i].channel);
        for (uint32_t b = 0; b < 8u; b++)
            psp_write8(p + 8u + b, (uint8_t)g_scan_groups[i].group_name[b]);
        for (uint32_t b = 0; b < 6u; b++)
            psp_write8(p + 16u + b, g_scan_groups[i].bssid[b]);
    }
    psp_write32(len_ptr, count * entry_sz);
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void write_peer_info_guest(uint32_t dst, uint32_t max_sz,
                                  uint32_t next_ptr,
                                  const uint8_t mac[SETO_MAC_LEN],
                                  const char *nick) {
    /* SceNetAdhocctlPeerInfoEmu (152 bytes):
     * +0: next(4), +4: nickname(128), +132: mac(6), +138: pad(2),
     * +140: flags(4), +144: last_recv_us(8) */
    uint8_t raw[152];
    memset(raw, 0, sizeof(raw));
    raw[0] = (uint8_t)(next_ptr & 0xFFu);
    raw[1] = (uint8_t)((next_ptr >> 8) & 0xFFu);
    raw[2] = (uint8_t)((next_ptr >> 16) & 0xFFu);
    raw[3] = (uint8_t)((next_ptr >> 24) & 0xFFu);
    if (nick) {
        for (size_t i = 0; i < 127u && nick[i]; i++)
            raw[4u + i] = (uint8_t)nick[i];
    }
    memcpy(raw + 132u, mac, SETO_MAC_LEN);
    uint32_t flags = 0x0400u;
    memcpy(raw + 140u, &flags, 4u);
    uint64_t ts = psp_threadman_vtime_us();
    memcpy(raw + 144u, &ts, 8u);

    uint32_t n = max_sz < 152u ? max_sz : 152u;
    for (uint32_t i = 0; i < n; i++) psp_write8(dst + i, raw[i]);
}

static void hle_adhocctl_get_peer_list(void) {
    uint32_t len_ptr = psp_arg(0);
    uint32_t buf_ptr = psp_arg(1);
    if (!len_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    const uint32_t entry_sz = 152u;
    mtx_lock(&g_state_lock);
    /* Exclude self from peer list */
    int others = 0;
    for (int i = 0; i < g_peer_count; i++) {
        if (!seto_mac_equal(g_peers[i].mac, g_local_mac)) others++;
    }
    if (!buf_ptr) {
        psp_write32(len_ptr, (uint32_t)others * entry_sz);
        mtx_unlock(&g_state_lock);
        psp_ret(0u);
        return;
    }
    uint32_t cap = psp_read32(len_ptr) / entry_sz;
    uint32_t written = 0;
    for (int i = 0; i < g_peer_count && written < cap; i++) {
        if (seto_mac_equal(g_peers[i].mac, g_local_mac)) continue;
        uint32_t p = buf_ptr + written * entry_sz;
        uint32_t next = ((int)(written + 1u) < others && (written + 1u) < cap) ? (p + entry_sz) : 0u;
        write_peer_info_guest(p, entry_sz, next, g_peers[i].mac, g_peers[i].nickname);
        written++;
    }
    psp_write32(len_ptr, written * entry_sz);
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void hle_adhocctl_get_peer_info(void) {
    uint32_t mac_ptr  = psp_arg(0);
    uint32_t size     = psp_arg(1);
    uint32_t info_ptr = psp_arg(2);
    if (!mac_ptr || !info_ptr || size == 0) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    uint8_t mac[SETO_MAC_LEN];
    for (uint32_t i = 0; i < SETO_MAC_LEN; i++) mac[i] = psp_read8(mac_ptr + i);

    if (seto_mac_equal(mac, g_local_mac)) {
        write_peer_info_guest(info_ptr, size, 0u, g_local_mac, g_nickname);
        psp_ret(0u);
        return;
    }

    int found = 0;
    mtx_lock(&g_state_lock);
    for (int i = 0; i < g_peer_count; i++) {
        if (seto_mac_equal(g_peers[i].mac, mac)) {
            write_peer_info_guest(info_ptr, size, 0u, g_peers[i].mac, g_peers[i].nickname);
            found = 1;
            break;
        }
    }
    mtx_unlock(&g_state_lock);
    psp_ret(found ? 0u : PSP_ADHOC_ERROR_INVALID_ARG);
}

static void hle_adhocctl_get_name_by_addr(void) {
    uint32_t mac_ptr  = psp_arg(0);
    uint32_t nick_ptr = psp_arg(1);
    if (!mac_ptr || !nick_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    uint8_t mac[SETO_MAC_LEN];
    for (uint32_t i = 0; i < SETO_MAC_LEN; i++) mac[i] = psp_read8(mac_ptr + i);

    const char *found_nick = NULL;
    char nick_copy[SETO_NICKNAME_LEN + 1] = { 0 };
    if (seto_mac_equal(mac, g_local_mac)) {
        snprintf(nick_copy, sizeof(nick_copy), "%s", g_nickname);
        found_nick = nick_copy;
    } else {
        mtx_lock(&g_state_lock);
        for (int i = 0; i < g_peer_count; i++) {
            if (seto_mac_equal(g_peers[i].mac, mac)) {
                memcpy(nick_copy, g_peers[i].nickname, SETO_NICKNAME_LEN);
                found_nick = nick_copy;
                break;
            }
        }
        mtx_unlock(&g_state_lock);
    }

    if (!found_nick) {
        psp_write8(nick_ptr, 0);
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    for (uint32_t i = 0; i < 127u && found_nick[i]; i++) {
        psp_write8(nick_ptr + i, (uint8_t)found_nick[i]);
        psp_write8(nick_ptr + i + 1u, 0);
    }
    if (!found_nick[0]) psp_write8(nick_ptr, 0);
    psp_ret(0u);
}

/* ---- sceNetAdhoc (PDP & PTP) --------------------------------------------- */

static void hle_adhoc_init(void) {
    ensure_locks();
    psp_ret(0u);
}

static void hle_adhoc_term(void) {
    if (!g_locks_inited) { psp_ret(0u); return; }
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
        while (g_pdp[i].q_count > 0) {
            free(g_pdp[i].q[g_pdp[i].q_head].data);
            g_pdp[i].q_head = (g_pdp[i].q_head + 1) % NET_MAX_PDP_QUEUE;
            g_pdp[i].q_count--;
        }
        memset(&g_pdp[i], 0, sizeof(g_pdp[i]));
    }
    for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
        free(g_ptp[i].rx_buf);
        memset(&g_ptp[i], 0, sizeof(g_ptp[i]));
    }
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static pdp_sock_t *find_pdp_locked(int id) {
    for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
        if (g_pdp[i].used && g_pdp[i].id == id) return &g_pdp[i];
    }
    return NULL;
}

static void hle_adhoc_pdp_create(void) {
    ensure_locks();
    uint32_t mac_ptr = psp_arg(0);
    uint16_t port    = (uint16_t)psp_arg(1);
    uint32_t bufsize = psp_arg(2);
    if (!mac_ptr || port == 0) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    int id = -1;
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
        if (!g_pdp[i].used) {
            id = g_next_pdp_id++;
            memset(&g_pdp[i], 0, sizeof(g_pdp[i]));
            g_pdp[i].used    = 1;
            g_pdp[i].id      = id;
            for (uint32_t b = 0; b < SETO_MAC_LEN; b++)
                g_pdp[i].mac[b] = psp_read8(mac_ptr + b);
            g_pdp[i].port    = port;
            g_pdp[i].bufsize = bufsize ? bufsize : 4096u;
            break;
        }
    }
    mtx_unlock(&g_state_lock);

    if (id < 0) {
        psp_ret(PSP_ADHOC_ERROR_PORT_IN_USE);
        return;
    }
    if (g_online) {
        seto_pdp_port_t req = { .client_pdp_id = (uint32_t)id, .port = port, .reserved = 0 };
        (void)net_send(SETO_OP_PDP_BIND, &req, sizeof(req));
    }
    psp_ret((uint32_t)id);
}

static void hle_adhoc_pdp_delete(void) {
    int id = (int)psp_arg(0);
    uint16_t port = 0;
    mtx_lock(&g_state_lock);
    pdp_sock_t *s = find_pdp_locked(id);
    if (s) {
        port = s->port;
        while (s->q_count > 0) {
            free(s->q[s->q_head].data);
            s->q_head = (s->q_head + 1) % NET_MAX_PDP_QUEUE;
            s->q_count--;
        }
        memset(s, 0, sizeof(*s));
    }
    mtx_unlock(&g_state_lock);
    if (port && g_online) {
        seto_pdp_port_t req = { .client_pdp_id = (uint32_t)id, .port = port, .reserved = 0 };
        (void)net_send(SETO_OP_PDP_UNBIND, &req, sizeof(req));
    }
    psp_ret(0u);
}

static void hle_adhoc_pdp_send(void) {
    int      id       = (int)psp_arg(0);
    uint32_t dmac_ptr = psp_arg(1);
    uint16_t dport    = (uint16_t)psp_arg(2);
    uint32_t data_ptr = psp_arg(3);
    uint32_t len      = psp_arg(4);

    if (!dmac_ptr || dport == 0 || len > SETO_MAX_PDP_LEN || (len > 0 && !data_ptr)) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    uint16_t sport = 0;
    mtx_lock(&g_state_lock);
    pdp_sock_t *s = find_pdp_locked(id);
    if (s) sport = s->port;
    mtx_unlock(&g_state_lock);
    if (!sport) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    uint8_t dmac[SETO_MAC_LEN];
    for (uint32_t i = 0; i < SETO_MAC_LEN; i++) dmac[i] = psp_read8(dmac_ptr + i);

    uint8_t *buf = len ? (uint8_t *)malloc(len) : NULL;
    if (len && !buf) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    for (uint32_t i = 0; i < len; i++) buf[i] = psp_read8(data_ptr + i);

    /* Self-targeted datagram delivers locally without leaving console */
    if (seto_mac_equal(dmac, g_local_mac)) {
        mtx_lock(&g_state_lock);
        for (int i = 0; i < NET_MAX_PDP_SOCKS; i++) {
            pdp_sock_t *dst = &g_pdp[i];
            if (dst->used && dst->port == dport && dst->q_count < NET_MAX_PDP_QUEUE) {
                uint8_t *copy = len ? (uint8_t *)malloc(len) : NULL;
                if (len && !copy) break;
                if (len) memcpy(copy, buf, len);
                int idx = (dst->q_head + dst->q_count) % NET_MAX_PDP_QUEUE;
                memcpy(dst->q[idx].src_mac, g_local_mac, SETO_MAC_LEN);
                dst->q[idx].src_port = sport;
                dst->q[idx].len      = (uint16_t)len;
                dst->q[idx].data     = copy;
                dst->q_count++;
                dst->rcvd_bytes += len;
                break;
            }
        }
        mtx_unlock(&g_state_lock);
        free(buf);
        psp_ret(0u);
        return;
    }

    if (g_online) {
        seto_pdp_send_t hdr = {
            .src_port = sport,
            .dst_port = dport,
            .data_len = (uint16_t)len
        };
        memcpy(hdr.dst_mac, dmac, SETO_MAC_LEN);
        (void)net_send2(SETO_OP_PDP_SEND, &hdr, sizeof(hdr), buf, len);
    }
    free(buf);
    psp_ret(0u);
}

static void hle_adhoc_pdp_recv(void) {
    int      id       = (int)psp_arg(0);
    uint32_t smac_ptr = psp_arg(1);
    uint32_t port_ptr = psp_arg(2);
    uint32_t data_ptr = psp_arg(3);
    uint32_t len_ptr  = psp_arg(4);
    uint32_t timeout  = psp_arg(5);
    int      nonblock = (int)psp_arg(6);

    if (!data_ptr || !len_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    uint64_t wait_limit_ms = (timeout == 0) ? 5000ULL : ((uint64_t)timeout + 999ULL) / 1000ULL;
    uint64_t t0 = host_ms();

    for (;;) {
        mtx_lock(&g_state_lock);
        pdp_sock_t *s = find_pdp_locked(id);
        if (!s) {
            mtx_unlock(&g_state_lock);
            psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
            return;
        }
        if (s->q_count > 0) {
            pdp_pkt_t *pkt = &s->q[s->q_head];
            uint32_t cap = psp_read32(len_ptr);
            if (smac_ptr) {
                for (uint32_t b = 0; b < SETO_MAC_LEN; b++)
                    psp_write8(smac_ptr + b, pkt->src_mac[b]);
            }
            if (port_ptr) psp_write16(port_ptr, pkt->src_port);

            if (pkt->len > cap) {
                psp_write32(len_ptr, pkt->len);
                mtx_unlock(&g_state_lock);
                psp_ret(PSP_ADHOC_ERROR_NOT_ENOUGH_SPACE);
                return;
            }
            for (uint16_t b = 0; b < pkt->len; b++)
                psp_write8(data_ptr + b, pkt->data[b]);
            psp_write32(len_ptr, pkt->len);

            free(pkt->data);
            memset(pkt, 0, sizeof(*pkt));
            s->q_head = (s->q_head + 1) % NET_MAX_PDP_QUEUE;
            s->q_count--;
            mtx_unlock(&g_state_lock);
            seto_net_pump_callbacks();
            psp_ret(0u);
            return;
        }
        mtx_unlock(&g_state_lock);

        if (nonblock) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_NO_DATA);
            return;
        }
        if ((host_ms() - t0) >= wait_limit_ms) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_TIMED_OUT);
            return;
        }
        psp_threadman_host_sleep_ms(2u);
        seto_net_pump_callbacks();
    }
}

/* ---- PTP (Reliable Stream) ----------------------------------------------- */

static ptp_sock_t *find_ptp_locked(int id) {
    for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
        if (g_ptp[i].kind != PTP_KIND_UNUSED && g_ptp[i].id == id) return &g_ptp[i];
    }
    return NULL;
}

static void hle_adhoc_ptp_listen(void) {
    ensure_locks();
    uint32_t smac_ptr = psp_arg(0);
    uint16_t sport    = (uint16_t)psp_arg(1);
    int      queue    = (int)psp_arg(5);
    if (!smac_ptr || sport == 0) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    int id = -1;
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
        if (g_ptp[i].kind == PTP_KIND_UNUSED) {
            id = g_next_ptp_id++;
            memset(&g_ptp[i], 0, sizeof(g_ptp[i]));
            g_ptp[i].kind       = PTP_KIND_LISTENER;
            g_ptp[i].id         = id;
            g_ptp[i].local_port = sport;
            break;
        }
    }
    mtx_unlock(&g_state_lock);
    if (id < 0) {
        psp_ret(PSP_ADHOC_ERROR_PORT_IN_USE);
        return;
    }

    if (g_online) {
        seto_ptp_listen_t req = {
            .client_listen_id = (uint32_t)id,
            .src_port         = sport,
            .backlog          = (uint16_t)(queue > 0 ? queue : 4)
        };
        (void)net_send(SETO_OP_PTP_LISTEN, &req, sizeof(req));
    }
    psp_ret((uint32_t)id);
}

static void hle_adhoc_ptp_open(void) {
    ensure_locks();
    uint32_t smac_ptr = psp_arg(0);
    uint16_t sport    = (uint16_t)psp_arg(1);
    uint32_t dmac_ptr = psp_arg(2);
    uint16_t dport    = (uint16_t)psp_arg(3);
    if (!smac_ptr || !dmac_ptr || dport == 0) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    uint8_t dmac[SETO_MAC_LEN];
    for (uint32_t b = 0; b < SETO_MAC_LEN; b++) dmac[b] = psp_read8(dmac_ptr + b);

    int id = -1;
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
        if (g_ptp[i].kind == PTP_KIND_UNUSED) {
            id = g_next_ptp_id++;
            memset(&g_ptp[i], 0, sizeof(g_ptp[i]));
            g_ptp[i].kind       = PTP_KIND_STREAM;
            g_ptp[i].id         = id;
            g_ptp[i].local_port = sport ? sport : (uint16_t)(40000u + (id & 0x3FFFu));
            memcpy(g_ptp[i].peer_mac, dmac, SETO_MAC_LEN);
            g_ptp[i].peer_port  = dport;
            sport               = g_ptp[i].local_port;
            break;
        }
    }
    mtx_unlock(&g_state_lock);
    if (id < 0) {
        psp_ret(PSP_ADHOC_ERROR_PORT_IN_USE);
        return;
    }

    if (g_online) {
        seto_ptp_open_t req = {
            .client_sock_id = (uint32_t)id,
            .src_port       = sport,
            .dst_port       = dport,
            .reserved       = 0
        };
        memcpy(req.dst_mac, dmac, SETO_MAC_LEN);
        (void)net_send(SETO_OP_PTP_OPEN, &req, sizeof(req));
    } else {
        mtx_lock(&g_state_lock);
        ptp_sock_t *s = find_ptp_locked(id);
        if (s) {
            s->peer_closed  = 1;
            s->close_reason = PSP_ADHOC_ERROR_TIMED_OUT;
        }
        mtx_unlock(&g_state_lock);
    }
    psp_ret((uint32_t)id);
}

static void hle_adhoc_ptp_connect(void) {
    int      id       = (int)psp_arg(0);
    uint32_t timeout  = psp_arg(1);
    int      nonblock = (int)psp_arg(2);

    uint64_t wait_limit_ms = (timeout == 0) ? 5000ULL : ((uint64_t)timeout + 999ULL) / 1000ULL;
    uint64_t t0 = host_ms();

    for (;;) {
        mtx_lock(&g_state_lock);
        ptp_sock_t *s = find_ptp_locked(id);
        if (!s || s->kind != PTP_KIND_STREAM) {
            mtx_unlock(&g_state_lock);
            psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
            return;
        }
        if (s->connected) {
            mtx_unlock(&g_state_lock);
            seto_net_pump_callbacks();
            psp_ret(0u);
            return;
        }
        if (s->peer_closed) {
            uint32_t r = s->close_reason ? s->close_reason : PSP_ADHOC_ERROR_CONNECTION_REFUSED;
            mtx_unlock(&g_state_lock);
            psp_ret(r);
            return;
        }
        mtx_unlock(&g_state_lock);

        if (nonblock) {
            psp_ret(PSP_ADHOC_ERROR_NO_DATA);
            return;
        }
        if ((host_ms() - t0) >= wait_limit_ms) {
            psp_ret(PSP_ADHOC_ERROR_TIMED_OUT);
            return;
        }
        psp_threadman_host_sleep_ms(2u);
        seto_net_pump_callbacks();
    }
}

static void hle_adhoc_ptp_accept(void) {
    int      id       = (int)psp_arg(0);
    uint32_t mac_ptr  = psp_arg(1);
    uint32_t port_ptr = psp_arg(2);
    uint32_t timeout  = psp_arg(3);
    int      nonblock = (int)psp_arg(4);

    uint64_t wait_limit_ms = (timeout == 0) ? 5000ULL : ((uint64_t)timeout + 999ULL) / 1000ULL;
    uint64_t t0 = host_ms();

    for (;;) {
        mtx_lock(&g_state_lock);
        ptp_sock_t *lst = find_ptp_locked(id);
        if (!lst || lst->kind != PTP_KIND_LISTENER) {
            mtx_unlock(&g_state_lock);
            psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
            return;
        }
        if (lst->inc_count > 0) {
            ptp_pending_inc_t inc = lst->inc[lst->inc_head];
            int new_id = -1;
            for (int i = 0; i < NET_MAX_PTP_SOCKS; i++) {
                if (g_ptp[i].kind == PTP_KIND_UNUSED) {
                    new_id = g_next_ptp_id++;
                    memset(&g_ptp[i], 0, sizeof(g_ptp[i]));
                    g_ptp[i].kind       = PTP_KIND_STREAM;
                    g_ptp[i].id         = new_id;
                    g_ptp[i].local_port = lst->local_port;
                    memcpy(g_ptp[i].peer_mac, inc.peer_mac, SETO_MAC_LEN);
                    g_ptp[i].peer_port  = inc.peer_port;
                    g_ptp[i].stream_id  = inc.stream_id;
                    g_ptp[i].connected  = 1;
                    break;
                }
            }
            if (new_id < 0) {
                mtx_unlock(&g_state_lock);
                psp_ret(PSP_ADHOC_ERROR_PORT_IN_USE);
                return;
            }
            lst->inc_head = (lst->inc_head + 1) % NET_MAX_PTP_BACKLOG;
            lst->inc_count--;
            mtx_unlock(&g_state_lock);

            if (mac_ptr) {
                for (uint32_t b = 0; b < SETO_MAC_LEN; b++)
                    psp_write8(mac_ptr + b, inc.peer_mac[b]);
            }
            if (port_ptr) psp_write16(port_ptr, inc.peer_port);

            if (g_online) {
                seto_ptp_accept_t ack = {
                    .stream_id      = inc.stream_id,
                    .client_sock_id = (uint32_t)new_id
                };
                (void)net_send(SETO_OP_PTP_ACCEPT, &ack, sizeof(ack));
            }
            seto_net_pump_callbacks();
            psp_ret((uint32_t)new_id);
            return;
        }
        mtx_unlock(&g_state_lock);

        if (nonblock) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_NO_DATA);
            return;
        }
        if ((host_ms() - t0) >= wait_limit_ms) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_TIMED_OUT);
            return;
        }
        psp_threadman_host_sleep_ms(2u);
        seto_net_pump_callbacks();
    }
}

static void hle_adhoc_ptp_send(void) {
    int      id       = (int)psp_arg(0);
    uint32_t data_ptr = psp_arg(1);
    uint32_t size_ptr = psp_arg(2);
    if (!size_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    int32_t total = (int32_t)psp_read32(size_ptr);
    if (total == 0) {
        psp_ret(0u);
        return;
    }
    if (total < 0 || !data_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    mtx_lock(&g_state_lock);
    ptp_sock_t *s = find_ptp_locked(id);
    if (!s || s->kind != PTP_KIND_STREAM || s->peer_closed) {
        uint32_t err = (!s || s->kind != PTP_KIND_STREAM) ? PSP_ADHOC_ERROR_INVALID_ARG
                                                          : PSP_ADHOC_ERROR_DISCONNECTED;
        mtx_unlock(&g_state_lock);
        psp_ret(err);
        return;
    }
    s->sent_total += (uint32_t)total;
    mtx_unlock(&g_state_lock);

    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    for (int32_t i = 0; i < total; i++) buf[i] = psp_read8(data_ptr + (uint32_t)i);

    size_t off = 0;
    while (off < (size_t)total) {
        size_t chunk = (size_t)total - off;
        if (chunk > SETO_MAX_PTP_CHUNK) chunk = SETO_MAX_PTP_CHUNK;
        seto_ptp_data_t dh = {
            .client_sock_id = (uint32_t)id,
            .data_len       = (uint16_t)chunk,
            .reserved       = 0
        };
        if (net_send2(SETO_OP_PTP_DATA, &dh, sizeof(dh), buf + off, chunk) < 0) {
            free(buf);
            psp_write32(size_ptr, (uint32_t)off);
            psp_ret(PSP_ADHOC_ERROR_DISCONNECTED);
            return;
        }
        off += chunk;
    }

    free(buf);
    psp_write32(size_ptr, (uint32_t)total);
    psp_ret(0u);
}

static void hle_adhoc_ptp_recv(void) {
    int      id       = (int)psp_arg(0);
    uint32_t data_ptr = psp_arg(1);
    uint32_t size_ptr = psp_arg(2);
    uint32_t timeout  = psp_arg(3);
    int      nonblock = (int)psp_arg(4);

    if (!data_ptr || !size_ptr) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }
    int32_t req = (int32_t)psp_read32(size_ptr);
    if (req <= 0) {
        psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
        return;
    }

    uint64_t wait_limit_ms = (timeout == 0) ? 5000ULL : ((uint64_t)timeout + 999ULL) / 1000ULL;
    uint64_t t0 = host_ms();

    for (;;) {
        mtx_lock(&g_state_lock);
        ptp_sock_t *s = find_ptp_locked(id);
        if (!s || s->kind != PTP_KIND_STREAM) {
            mtx_unlock(&g_state_lock);
            psp_ret(PSP_ADHOC_ERROR_INVALID_ARG);
            return;
        }
        if (s->rx_len > 0) {
            size_t take = (size_t)req < s->rx_len ? (size_t)req : s->rx_len;
            for (size_t i = 0; i < take; i++)
                psp_write8(data_ptr + (uint32_t)i, s->rx_buf[i]);
            if (take < s->rx_len)
                memmove(s->rx_buf, s->rx_buf + take, s->rx_len - take);
            s->rx_len -= take;
            psp_write32(size_ptr, (uint32_t)take);
            mtx_unlock(&g_state_lock);
            seto_net_pump_callbacks();
            psp_ret(0u);
            return;
        }
        if (s->peer_closed) {
            uint32_t r = s->close_reason ? s->close_reason : PSP_ADHOC_ERROR_DISCONNECTED;
            mtx_unlock(&g_state_lock);
            psp_write32(size_ptr, 0u);
            psp_ret(r);
            return;
        }
        mtx_unlock(&g_state_lock);

        if (nonblock) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_NO_DATA);
            return;
        }
        if ((host_ms() - t0) >= wait_limit_ms) {
            seto_net_pump_callbacks();
            psp_ret(PSP_ADHOC_ERROR_TIMED_OUT);
            return;
        }
        psp_threadman_host_sleep_ms(2u);
        seto_net_pump_callbacks();
    }
}

static void hle_adhoc_ptp_close(void) {
    int id = (int)psp_arg(0);
    int was_open = 0;

    mtx_lock(&g_state_lock);
    ptp_sock_t *s = find_ptp_locked(id);
    if (s) {
        was_open = 1;
        free(s->rx_buf);
        memset(s, 0, sizeof(*s));
    }
    mtx_unlock(&g_state_lock);

    if (was_open && g_online) {
        seto_ptp_close_t req = { .client_sock_id = (uint32_t)id };
        (void)net_send(SETO_OP_PTP_CLOSE, &req, sizeof(req));
    }
    psp_ret(0u);
}

/* ---- sceNetAdhocMatching* ------------------------------------------------ */

static match_slot_t *find_match_locked(int id) {
    for (int i = 0; i < NET_MAX_MATCH_SLOTS; i++) {
        if (g_match[i].used && g_match[i].id == id) return &g_match[i];
    }
    return NULL;
}

static void hle_adhoc_matching_init(void) {
    ensure_locks();
    if (!g_cb_guest_mem)
        g_cb_guest_mem = psp_sysmem_alloc(CB_GUEST_TOTAL_BYTES, 1);
    psp_ret(0u);
}

static void hle_adhoc_matching_term(void) {
    if (!g_locks_inited) { psp_ret(0u); return; }
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_MATCH_SLOTS; i++) {
        if (g_match[i].used && g_match[i].running && g_online) {
            seto_match_stop_t req = {
                .client_matching_id = (uint32_t)g_match[i].id,
                .port               = g_match[i].port,
                .reserved           = 0
            };
            mtx_unlock(&g_state_lock);
            (void)net_send(SETO_OP_MATCH_STOP, &req, sizeof(req));
            mtx_lock(&g_state_lock);
        }
    }
    memset(g_match, 0, sizeof(g_match));
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void hle_adhoc_matching_create(void) {
    ensure_locks();
    int      mode        = (int)psp_arg(0);
    int      maxpeers    = (int)psp_arg(1);
    uint16_t port        = (uint16_t)psp_arg(2);
    uint32_t hello_delay = psp_arg(4);
    /* Allegrex EABI passes args 0..7 in $a0..$a3 + $t0..$t3, and the 9th
     * argument (index 8: callback) at $sp + 0 (no 32-byte register home area). */
    uint32_t cb_fn       = psp_read32(psp_cpu.r[PSP_REG_SP]);

    if (mode < 1 || mode > 3 || port == 0) {
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ARG);
        return;
    }

    int id = -1;
    mtx_lock(&g_state_lock);
    for (int i = 0; i < NET_MAX_MATCH_SLOTS; i++) {
        if (!g_match[i].used) {
            id = g_next_match_id++;
            memset(&g_match[i], 0, sizeof(g_match[i]));
            g_match[i].used           = 1;
            g_match[i].id             = id;
            g_match[i].mode           = mode;
            g_match[i].max_peers      = maxpeers > 0 ? maxpeers : 2;
            g_match[i].port           = port;
            g_match[i].hello_delay_us = hello_delay ? hello_delay : 1000000u;
            g_match[i].cb_fn          = cb_fn;
            g_match[i].cb_gp          = psp_cpu.r[PSP_REG_GP];
            g_match[i].cb_k0          = psp_cpu.r[PSP_REG_K0];
            break;
        }
    }
    mtx_unlock(&g_state_lock);

    if (trace_enabled()) {
        fprintf(stderr, "seto_net: MatchingCreate(mode=%d, max=%d, port=%u, cb=0x%08X) -> id=%d\n",
                mode, maxpeers, port, cb_fn, id);
    }
    psp_ret(id > 0 ? (uint32_t)id : PSP_ADHOC_MATCHING_ERROR_MAXNUM);
}

static void hle_adhoc_matching_start(void) {
    int      id      = (int)psp_arg(0);
    int32_t  opt_len = (int32_t)psp_arg(5);
    uint32_t opt_ptr = psp_arg(6);

    if (opt_len < 0 || (uint32_t)opt_len > SETO_MAX_OPT_LEN || (opt_len > 0 && !opt_ptr)) {
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ARG);
        return;
    }

    seto_match_start_t req;
    uint8_t opt_buf[SETO_MAX_OPT_LEN];
    for (int32_t i = 0; i < opt_len; i++)
        opt_buf[i] = psp_read8(opt_ptr + (uint32_t)i);

    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (!m) {
        mtx_unlock(&g_state_lock);
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ID);
        return;
    }
    m->running   = 1;
    m->hello_len = (uint16_t)opt_len;
    if (opt_len > 0) memcpy(m->hello_opt, opt_buf, (size_t)opt_len);

    memset(&req, 0, sizeof(req));
    req.client_matching_id = (uint32_t)m->id;
    req.port               = m->port;
    req.mode               = (uint8_t)m->mode;
    req.max_peers          = (uint8_t)m->max_peers;
    req.hello_delay_us     = m->hello_delay_us;
    req.opt_len            = (uint16_t)opt_len;
    mtx_unlock(&g_state_lock);

    if (g_online)
        (void)net_send2(SETO_OP_MATCH_START, &req, sizeof(req), opt_buf, (size_t)opt_len);

    seto_net_pump_callbacks();
    psp_ret(0u);
}

static void hle_adhoc_matching_stop(void) {
    int id = (int)psp_arg(0);
    seto_match_stop_t req;
    int send_it = 0;

    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (!m) {
        mtx_unlock(&g_state_lock);
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ID);
        return;
    }
    if (m->running) {
        m->running = 0;
        req.client_matching_id = (uint32_t)m->id;
        req.port               = m->port;
        req.reserved           = 0;
        send_it                = 1;
    }
    mtx_unlock(&g_state_lock);

    if (send_it && g_online)
        (void)net_send(SETO_OP_MATCH_STOP, &req, sizeof(req));
    psp_ret(0u);
}

static void hle_adhoc_matching_delete(void) {
    int id = (int)psp_arg(0);
    seto_match_stop_t req;
    int send_it = 0;

    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (m) {
        if (m->running) {
            req.client_matching_id = (uint32_t)m->id;
            req.port               = m->port;
            req.reserved           = 0;
            send_it                = 1;
        }
        memset(m, 0, sizeof(*m));
    }
    mtx_unlock(&g_state_lock);

    if (send_it && g_online)
        (void)net_send(SETO_OP_MATCH_STOP, &req, sizeof(req));
    psp_ret(0u);
}

static void hle_adhoc_matching_set_hello_opt(void) {
    int      id      = (int)psp_arg(0);
    int32_t  opt_len = (int32_t)psp_arg(1);
    uint32_t opt_ptr = psp_arg(2);
    if (opt_len < 0 || (uint32_t)opt_len > SETO_MAX_OPT_LEN || (opt_len > 0 && !opt_ptr)) {
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ARG);
        return;
    }

    uint8_t opt_buf[SETO_MAX_OPT_LEN];
    for (int32_t i = 0; i < opt_len; i++)
        opt_buf[i] = psp_read8(opt_ptr + (uint32_t)i);

    seto_match_hello_t req;
    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (!m) {
        mtx_unlock(&g_state_lock);
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ID);
        return;
    }
    m->hello_len = (uint16_t)opt_len;
    if (opt_len > 0) memcpy(m->hello_opt, opt_buf, (size_t)opt_len);
    req.client_matching_id = (uint32_t)m->id;
    req.port               = m->port;
    req.opt_len            = (uint16_t)opt_len;
    int running = m->running;
    mtx_unlock(&g_state_lock);

    if (running && g_online)
        (void)net_send2(SETO_OP_MATCH_SET_HELLO, &req, sizeof(req), opt_buf, (size_t)opt_len);
    psp_ret(0u);
}

static void hle_adhoc_matching_get_hello_opt(void) {
    int      id      = (int)psp_arg(0);
    uint32_t len_ptr = psp_arg(1);
    uint32_t opt_ptr = psp_arg(2);
    if (!len_ptr) {
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ARG);
        return;
    }
    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (!m) {
        mtx_unlock(&g_state_lock);
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ID);
        return;
    }
    uint32_t cap = psp_read32(len_ptr);
    psp_write32(len_ptr, m->hello_len);
    if (opt_ptr && cap > 0) {
        uint32_t n = cap < m->hello_len ? cap : m->hello_len;
        for (uint32_t i = 0; i < n; i++) psp_write8(opt_ptr + i, m->hello_opt[i]);
    }
    mtx_unlock(&g_state_lock);
    psp_ret(0u);
}

static void match_target_op_common(uint8_t opcode, int has_opt) {
    int      id      = (int)psp_arg(0);
    uint32_t mac_ptr = psp_arg(1);
    int32_t  opt_len = has_opt ? (int32_t)psp_arg(2) : 0;
    uint32_t opt_ptr = has_opt ? psp_arg(3) : 0;

    if (!mac_ptr || opt_len < 0 || (uint32_t)opt_len > SETO_MAX_OPT_LEN ||
        (opt_len > 0 && !opt_ptr)) {
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ARG);
        return;
    }

    uint8_t opt_buf[SETO_MAX_OPT_LEN];
    for (int32_t i = 0; i < opt_len; i++)
        opt_buf[i] = psp_read8(opt_ptr + (uint32_t)i);

    seto_match_target_t req;
    memset(&req, 0, sizeof(req));
    for (uint32_t b = 0; b < SETO_MAC_LEN; b++)
        req.target_mac[b] = psp_read8(mac_ptr + b);
    req.opt_len = (uint16_t)opt_len;

    mtx_lock(&g_state_lock);
    match_slot_t *m = find_match_locked(id);
    if (!m || !m->running) {
        mtx_unlock(&g_state_lock);
        psp_ret(PSP_ADHOC_MATCHING_ERROR_INVALID_ID);
        return;
    }
    req.client_matching_id = (uint32_t)m->id;
    req.port               = m->port;
    mtx_unlock(&g_state_lock);

    if (g_online)
        (void)net_send2(opcode, &req, sizeof(req), opt_buf, (size_t)opt_len);
    psp_ret(0u);
}

static void hle_adhoc_matching_select_target(void) {
    match_target_op_common(SETO_OP_MATCH_SELECT, 1);
}

static void hle_adhoc_matching_cancel_target(void) {
    match_target_op_common(SETO_OP_MATCH_CANCEL, 0);
}

static void hle_adhoc_matching_cancel_target_opt(void) {
    match_target_op_common(SETO_OP_MATCH_CANCEL, 1);
}

static void hle_adhoc_matching_send_data(void) {
    match_target_op_common(SETO_OP_MATCH_SEND_DATA, 1);
}

/* --------------------------------------------------------------------------
 * Registration Entry Point
 * -------------------------------------------------------------------------- */

void seto_net_register_hle(void) {
    ensure_locks();

    static const struct {
        uint32_t    nid;
        const char *lib;
        const char *name;
        psp_hle_fn  fn;
    } entries[] = {
        /* sceWlanDrv */
        { 0x93440B11u, "sceWlanDrv",     "sceWlanDevIsPowerOn",     hle_wlan_on },
        { 0xD7763699u, "sceWlanDrv",     "sceWlanGetSwitchState",   hle_wlan_on },
        { 0x0C622081u, "sceWlanDrv",     "sceWlanGetEtherAddr",     hle_net_mac },

        /* sceNet */
        { 0x39AF39A6u, "sceNet",         "sceNetInit",              hle_net_init },
        { 0x281928A9u, "sceNet",         "sceNetTerm",              hle_net_term },
        { 0x0BF0A3AEu, "sceNet",         "sceNetGetLocalEtherAddr", hle_net_mac },
        { 0x89360950u, "sceNet",         "sceNetEtherNtostr",       hle_net_ether_ntostr },
        { 0xD27961C9u, "sceNet",         "sceNetEtherStrton",       hle_net_ether_strton },

        /* sceUtilityNetconf */
        { 0x1579A159u, "sceUtility",     "sceUtilityLoadNetModule",        hle_net_ok },
        { 0x64D50C56u, "sceUtility",     "sceUtilityUnloadNetModule",      hle_net_ok },
        { 0x4DB1E739u, "sceUtility",     "sceUtilityNetconfInitStart",     hle_netconf_init_start },
        { 0x6332AA39u, "sceUtility",     "sceUtilityNetconfGetStatus",     hle_netconf_get_status },
        { 0x91E70E35u, "sceUtility",     "sceUtilityNetconfUpdate",        hle_netconf_update },
        { 0xF88155F6u, "sceUtility",     "sceUtilityNetconfShutdownStart", hle_netconf_shutdown_start },

        /* sceNetAdhoc (PDP & PTP) */
        { 0xE1D621D7u, "sceNetAdhoc",    "sceNetAdhocInit",         hle_adhoc_init },
        { 0xA62C6F57u, "sceNetAdhoc",    "sceNetAdhocTerm",         hle_adhoc_term },
        { 0x6F92741Bu, "sceNetAdhoc",    "sceNetAdhocPdpCreate",    hle_adhoc_pdp_create },
        { 0x7F27BB5Eu, "sceNetAdhoc",    "sceNetAdhocPdpDelete",    hle_adhoc_pdp_delete },
        { 0xABED3790u, "sceNetAdhoc",    "sceNetAdhocPdpSend",      hle_adhoc_pdp_send },
        { 0xDFE53E03u, "sceNetAdhoc",    "sceNetAdhocPdpRecv",      hle_adhoc_pdp_recv },
        { 0x877F6D66u, "sceNetAdhoc",    "sceNetAdhocPtpOpen",      hle_adhoc_ptp_open },
        { 0xFC6FC07Bu, "sceNetAdhoc",    "sceNetAdhocPtpConnect",   hle_adhoc_ptp_connect },
        { 0xE08BDAC1u, "sceNetAdhoc",    "sceNetAdhocPtpListen",    hle_adhoc_ptp_listen },
        { 0x9DF81198u, "sceNetAdhoc",    "sceNetAdhocPtpAccept",    hle_adhoc_ptp_accept },
        { 0x4DA4C788u, "sceNetAdhoc",    "sceNetAdhocPtpSend",      hle_adhoc_ptp_send },
        { 0x8BEA2B3Eu, "sceNetAdhoc",    "sceNetAdhocPtpRecv",      hle_adhoc_ptp_recv },
        { 0x9AC2EEACu, "sceNetAdhoc",    "sceNetAdhocPtpFlush",     hle_net_ok },
        { 0x157E6225u, "sceNetAdhoc",    "sceNetAdhocPtpClose",     hle_adhoc_ptp_close },

        /* sceNetAdhocctl */
        { 0xE26F226Eu, "sceNetAdhocctl", "sceNetAdhocctlInit",          hle_adhocctl_init },
        { 0x9D689E13u, "sceNetAdhocctl", "sceNetAdhocctlTerm",          hle_adhocctl_term },
        { 0x0AD043EDu, "sceNetAdhocctl", "sceNetAdhocctlConnect",       hle_adhocctl_connect },
        { 0xEC0635C1u, "sceNetAdhocctl", "sceNetAdhocctlCreate",        hle_adhocctl_create },
        { 0x5E7F79C9u, "sceNetAdhocctl", "sceNetAdhocctlJoin",          hle_adhocctl_join },
        { 0x34401D65u, "sceNetAdhocctl", "sceNetAdhocctlDisconnect",    hle_adhocctl_disconnect },
        { 0x75ECD386u, "sceNetAdhocctl", "sceNetAdhocctlGetState",      hle_adhocctl_get_state },
        { 0x20B317A0u, "sceNetAdhocctl", "sceNetAdhocctlAddHandler",    hle_adhocctl_add_handler },
        { 0x6402490Bu, "sceNetAdhocctl", "sceNetAdhocctlDelHandler",    hle_adhocctl_del_handler },
        { 0x08FFF7A0u, "sceNetAdhocctl", "sceNetAdhocctlScan",          hle_adhocctl_scan },
        { 0x81AEE1BEu, "sceNetAdhocctl", "sceNetAdhocctlGetScanInfo",   hle_adhocctl_get_scan_info },
        { 0xE162CB14u, "sceNetAdhocctl", "sceNetAdhocctlGetPeerList",   hle_adhocctl_get_peer_list },
        { 0x8DB83FDCu, "sceNetAdhocctl", "sceNetAdhocctlGetPeerInfo",   hle_adhocctl_get_peer_info },
        { 0x8916C003u, "sceNetAdhocctl", "sceNetAdhocctlGetNameByAddr", hle_adhocctl_get_name_by_addr },
        { 0xDED9D28Eu, "sceNetAdhocctl", "sceNetAdhocctlGetParameter",  hle_adhocctl_get_parameter },

        /* sceNetAdhocMatching */
        { 0x2A2A1E07u, "sceNetAdhocMatching", "sceNetAdhocMatchingInit",                hle_adhoc_matching_init },
        { 0x7945ECDAu, "sceNetAdhocMatching", "sceNetAdhocMatchingTerm",                hle_adhoc_matching_term },
        { 0xCA5EDA6Fu, "sceNetAdhocMatching", "sceNetAdhocMatchingCreate",              hle_adhoc_matching_create },
        { 0xF16EAF4Fu, "sceNetAdhocMatching", "sceNetAdhocMatchingDelete",              hle_adhoc_matching_delete },
        { 0x93EF3843u, "sceNetAdhocMatching", "sceNetAdhocMatchingStart",               hle_adhoc_matching_start },
        { 0x32B156B3u, "sceNetAdhocMatching", "sceNetAdhocMatchingStop",                hle_adhoc_matching_stop },
        { 0x5E3D4B79u, "sceNetAdhocMatching", "sceNetAdhocMatchingSelectTarget",        hle_adhoc_matching_select_target },
        { 0xEA3C6108u, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTarget",        hle_adhoc_matching_cancel_target },
        { 0x8F58BEDFu, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTargetWithOpt", hle_adhoc_matching_cancel_target_opt },
        { 0xF79472D7u, "sceNetAdhocMatching", "sceNetAdhocMatchingSendData",            hle_adhoc_matching_send_data },
        { 0xEC19337Du, "sceNetAdhocMatching", "sceNetAdhocMatchingAbortSendData",       hle_net_ok },
        { 0xB58E61B7u, "sceNetAdhocMatching", "sceNetAdhocMatchingSetHelloOpt",         hle_adhoc_matching_set_hello_opt },
        { 0xB5D96C2Au, "sceNetAdhocMatching", "sceNetAdhocMatchingGetHelloOpt",         hle_adhoc_matching_get_hello_opt }
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        psp_hle_register(entries[i].nid, entries[i].lib, entries[i].name, entries[i].fn);
    }
}
