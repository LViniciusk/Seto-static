/*
 * seto_proto.h — Shared wire protocol & PSP Ad-Hoc constants between
 * Seto-Server (Linux VPS) and Seto-static (Win32/POSIX HLE client).
 *
 * Pure C11, little-endian framing over TCP (with TCP_NODELAY recommended).
 */
#ifndef SETO_PROTO_H
#define SETO_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Wire Framing Constants
 * -------------------------------------------------------------------------- */
#define SETO_PROTO_MAGIC        0x5453u   /* 'S','T' in little-endian */
#define SETO_PROTO_VERSION      1u
#define SETO_DEFAULT_PORT       27315u
#define SETO_MAX_PAYLOAD        65536u
#define SETO_MAX_OPT_LEN        2048u
#define SETO_MAX_PDP_LEN        16384u
#define SETO_MAX_PTP_CHUNK      16384u

#define SETO_MAC_LEN            6u
#define SETO_GROUP_NAME_LEN     8u
#define SETO_PRODUCT_LEN        12u
#define SETO_NICKNAME_LEN       128u

/* --------------------------------------------------------------------------
 * Canonical PSP Firmware Constants (PSPSDK / PPSSPP verified)
 * -------------------------------------------------------------------------- */

/* sceUtilityNetconf actions */
enum seto_psp_netconf_action {
    PSP_NETCONF_ACTION_CONNECT_AP     = 0,
    PSP_NETCONF_ACTION_DISPLAY_STATUS = 1,
    PSP_NETCONF_ACTION_CONNECT_ADHOC  = 2,
    PSP_NETCONF_ACTION_CONNECT_ADHOC_EX = 3,
    PSP_NETCONF_ACTION_CREATE_ADHOC   = 4,
    PSP_NETCONF_ACTION_JOIN_ADHOC     = 5
};

/* sceNetAdhocctl states */
enum seto_psp_adhocctl_state {
    PSP_ADHOCCTL_STATE_DISCONNECTED = 0,
    PSP_ADHOCCTL_STATE_CONNECTED    = 1,
    PSP_ADHOCCTL_STATE_SCANNING     = 2,
    PSP_ADHOCCTL_STATE_GAMEMODE     = 3
};

/* sceNetAdhocctl handler events */
enum seto_psp_adhocctl_event {
    PSP_ADHOCCTL_EVENT_ERROR      = 0,
    PSP_ADHOCCTL_EVENT_CONNECT    = 1,
    PSP_ADHOCCTL_EVENT_DISCONNECT = 2,
    PSP_ADHOCCTL_EVENT_SCAN       = 3,
    PSP_ADHOCCTL_EVENT_GAME       = 4
};

/* sceNetAdhocMatching modes */
enum seto_psp_matching_mode {
    PSP_ADHOC_MATCHING_MODE_HOST   = 1,
    PSP_ADHOC_MATCHING_MODE_CLIENT = 2,
    PSP_ADHOC_MATCHING_MODE_PTP    = 3
};

/* sceNetAdhocMatching callback events */
enum seto_psp_matching_event {
    PSP_ADHOC_MATCHING_EVENT_HELLO        = 1,
    PSP_ADHOC_MATCHING_EVENT_JOIN         = 2,
    PSP_ADHOC_MATCHING_EVENT_LEFT         = 3,
    PSP_ADHOC_MATCHING_EVENT_REJECT       = 4,
    PSP_ADHOC_MATCHING_EVENT_CANCEL       = 5,
    PSP_ADHOC_MATCHING_EVENT_ACCEPT       = 6,
    PSP_ADHOC_MATCHING_EVENT_COMPLETE     = 7,
    PSP_ADHOC_MATCHING_EVENT_TIMEOUT      = 8,
    PSP_ADHOC_MATCHING_EVENT_ERROR        = 9,
    PSP_ADHOC_MATCHING_EVENT_DISCONNECT   = 10,
    PSP_ADHOC_MATCHING_EVENT_DATA         = 11,
    PSP_ADHOC_MATCHING_EVENT_DATA_CONFIRM = 12,
    PSP_ADHOC_MATCHING_EVENT_DATA_TIMEOUT = 13
};

/* Common PSP Ad-Hoc error codes */
#define PSP_NET_ERROR_INVALID_ARG             0x80410001u
#define PSP_ADHOC_ERROR_INVALID_ARG           0x80410701u
#define PSP_ADHOC_ERROR_NOT_INITIALIZED       0x80410712u
#define PSP_ADHOC_ERROR_PORT_IN_USE           0x80410706u
#define PSP_ADHOC_ERROR_CONNECTED             0x80410707u
#define PSP_ADHOC_ERROR_DISCONNECTED          0x80410708u
#define PSP_ADHOC_ERROR_NO_DATA               0x80410709u /* WOULD_BLOCK */
#define PSP_ADHOC_ERROR_TIMED_OUT             0x80410715u
#define PSP_ADHOC_ERROR_CONNECTION_REFUSED    0x80410718u
#define PSP_ADHOC_MATCHING_ERROR_INVALID_ID   0x80410801u
#define PSP_ADHOC_MATCHING_ERROR_INVALID_ARG  0x80410802u
#define PSP_ADHOC_MATCHING_ERROR_UNKNOWN_TARGET 0x80410806u
#define PSP_ADHOC_MATCHING_ERROR_MAXNUM       0x80410809u
#define PSP_ADHOC_MATCHING_ERROR_IN_PROGRESS  0x8041080Cu
#define PSP_ADHOC_MATCHING_ERROR_ESTABLISHED  0x8041080Du

/* --------------------------------------------------------------------------
 * Seto Wire Opcodes
 * -------------------------------------------------------------------------- */
enum seto_opcode {
    /* Session & Keepalive */
    SETO_OP_HELLO            = 0x01, /* C -> S: seto_hello_t */
    SETO_OP_WELCOME          = 0x02, /* S -> C: seto_welcome_t */
    SETO_OP_PING             = 0x03, /* C <-> S: seto_ping_t */
    SETO_OP_PONG             = 0x04, /* C <-> S: seto_ping_t */
    SETO_OP_ERROR            = 0x0F, /* S -> C: seto_error_t */

    /* Adhocctl & Netconf (Group / Room Management) */
    SETO_OP_ADHOCCTL_JOIN    = 0x10, /* C -> S: seto_adhocctl_join_t */
    SETO_OP_ADHOCCTL_JOINED  = 0x11, /* S -> C: seto_adhocctl_joined_t */
    SETO_OP_ADHOCCTL_LEAVE   = 0x12, /* C -> S: empty */
    SETO_OP_ADHOCCTL_LEFT    = 0x13, /* S -> C: empty */
    SETO_OP_PEER_SNAPSHOT    = 0x14, /* S -> C: seto_peer_snapshot_t + N * seto_peer_entry_t */
    SETO_OP_SCAN_REQ         = 0x15, /* C -> S: empty */
    SETO_OP_SCAN_RSP         = 0x16, /* S -> C: seto_scan_rsp_t + N * seto_scan_entry_t */

    /* AdhocMatching (Laboratory Lobby Discovery & Handshake) */
    SETO_OP_MATCH_START      = 0x20, /* C -> S: seto_match_start_t + opt[opt_len] */
    SETO_OP_MATCH_STOP       = 0x21, /* C -> S: seto_match_stop_t */
    SETO_OP_MATCH_SET_HELLO  = 0x22, /* C -> S: seto_match_hello_t + opt[opt_len] */
    SETO_OP_MATCH_SELECT     = 0x23, /* C -> S: seto_match_target_t + opt[opt_len] */
    SETO_OP_MATCH_CANCEL     = 0x24, /* C -> S: seto_match_target_t + opt[opt_len] */
    SETO_OP_MATCH_SEND_DATA  = 0x25, /* C -> S: seto_match_target_t + data[opt_len] */
    SETO_OP_MATCH_EVENT      = 0x28, /* S -> C: seto_match_event_t + opt[opt_len] */

    /* Adhoc PDP (Datagram Relay) */
    SETO_OP_PDP_BIND         = 0x30, /* C -> S: seto_pdp_port_t */
    SETO_OP_PDP_UNBIND       = 0x31, /* C -> S: seto_pdp_port_t */
    SETO_OP_PDP_SEND         = 0x32, /* C -> S: seto_pdp_send_t + data[data_len] */
    SETO_OP_PDP_RECV         = 0x33, /* S -> C: seto_pdp_recv_t + data[data_len] */

    /* Adhoc PTP (Reliable Stream Relay) */
    SETO_OP_PTP_LISTEN       = 0x40, /* C -> S: seto_ptp_listen_t */
    SETO_OP_PTP_OPEN         = 0x41, /* C -> S: seto_ptp_open_t */
    SETO_OP_PTP_INCOMING     = 0x42, /* S -> C: seto_ptp_incoming_t */
    SETO_OP_PTP_ACCEPT       = 0x43, /* C -> S: seto_ptp_accept_t */
    SETO_OP_PTP_CONNECTED    = 0x44, /* S -> C: seto_ptp_connected_t */
    SETO_OP_PTP_DATA         = 0x45, /* C <-> S: seto_ptp_data_t + data[data_len] */
    SETO_OP_PTP_CLOSE        = 0x46, /* C -> S: seto_ptp_close_t */
    SETO_OP_PTP_CLOSED       = 0x47  /* S -> C: seto_ptp_closed_t */
};

/* --------------------------------------------------------------------------
 * Wire Packet Structures (packed, little-endian)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct seto_hdr {
    uint16_t magic;      /* SETO_PROTO_MAGIC (0x5453) */
    uint8_t  version;    /* SETO_PROTO_VERSION (1) */
    uint8_t  opcode;     /* enum seto_opcode */
    uint32_t length;     /* Payload bytes following this 8-byte header */
} seto_hdr_t;

/* 0x01: SETO_OP_HELLO (C -> S) */
typedef struct seto_hello {
    char     product[SETO_PRODUCT_LEN];   /* e.g. "ULUS10136" */
    char     nickname[SETO_NICKNAME_LEN]; /* PSP SystemParam nickname */
    uint8_t  preferred_mac[SETO_MAC_LEN]; /* 00:00:00:00:00:00 for auto-assign */
    uint16_t flags;
} seto_hello_t;

/* 0x02: SETO_OP_WELCOME (S -> C) */
typedef struct seto_welcome {
    uint32_t session_id;
    uint8_t  assigned_mac[SETO_MAC_LEN];
    uint16_t flags;
} seto_welcome_t;

/* 0x03 / 0x04: SETO_OP_PING / SETO_OP_PONG */
typedef struct seto_ping {
    uint64_t timestamp_us;
} seto_ping_t;

/* 0x0F: SETO_OP_ERROR (S -> C) */
typedef struct seto_error {
    uint32_t psp_error;
    uint8_t  ref_opcode;
    char     message[59];
} seto_error_t;

/* 0x10: SETO_OP_ADHOCCTL_JOIN (C -> S) */
typedef struct seto_adhocctl_join {
    char     group_name[SETO_GROUP_NAME_LEN]; /* 8-byte PSP group name */
    uint8_t  action;                          /* enum seto_psp_netconf_action */
    uint8_t  reserved[3];
} seto_adhocctl_join_t;

/* 0x11: SETO_OP_ADHOCCTL_JOINED (S -> C) */
typedef struct seto_adhocctl_joined {
    char     group_name[SETO_GROUP_NAME_LEN];
    uint8_t  bssid[SETO_MAC_LEN];
    uint16_t channel;
} seto_adhocctl_joined_t;

/* 0x14: SETO_OP_PEER_SNAPSHOT (S -> C) */
typedef struct seto_peer_entry {
    uint8_t  mac[SETO_MAC_LEN];
    uint16_t flags;
    char     nickname[SETO_NICKNAME_LEN];
} seto_peer_entry_t;

typedef struct seto_peer_snapshot {
    char     group_name[SETO_GROUP_NAME_LEN];
    uint16_t peer_count;
    uint16_t reserved;
    /* Followed by peer_count * seto_peer_entry_t */
} seto_peer_snapshot_t;

/* 0x16: SETO_OP_SCAN_RSP (S -> C) */
typedef struct seto_scan_entry {
    char     group_name[SETO_GROUP_NAME_LEN];
    uint8_t  bssid[SETO_MAC_LEN];
    uint16_t channel;
    uint16_t peer_count;
    uint16_t reserved;
} seto_scan_entry_t;

typedef struct seto_scan_rsp {
    uint16_t group_count;
    uint16_t reserved;
    /* Followed by group_count * seto_scan_entry_t */
} seto_scan_rsp_t;

/* 0x20: SETO_OP_MATCH_START (C -> S) */
typedef struct seto_match_start {
    uint32_t client_matching_id;
    uint16_t port;
    uint8_t  mode;             /* enum seto_psp_matching_mode (1..3) */
    uint8_t  max_peers;
    uint32_t hello_delay_us;
    uint16_t opt_len;
    uint16_t reserved;
    /* Followed by uint8_t opt_data[opt_len] */
} seto_match_start_t;

/* 0x21: SETO_OP_MATCH_STOP (C -> S) */
typedef struct seto_match_stop {
    uint32_t client_matching_id;
    uint16_t port;
    uint16_t reserved;
} seto_match_stop_t;

/* 0x22: SETO_OP_MATCH_SET_HELLO (C -> S) */
typedef struct seto_match_hello {
    uint32_t client_matching_id;
    uint16_t port;
    uint16_t opt_len;
    /* Followed by uint8_t opt_data[opt_len] */
} seto_match_hello_t;

/* 0x23 / 0x24 / 0x25: SELECT, CANCEL, SEND_DATA (C -> S) */
typedef struct seto_match_target {
    uint32_t client_matching_id;
    uint16_t port;
    uint8_t  target_mac[SETO_MAC_LEN];
    uint16_t opt_len;
    uint16_t reserved;
    /* Followed by uint8_t opt_data[opt_len] */
} seto_match_target_t;

/* 0x28: SETO_OP_MATCH_EVENT (S -> C) */
typedef struct seto_match_event {
    uint32_t client_matching_id;
    uint16_t port;
    uint16_t event;            /* enum seto_psp_matching_event (1..13) */
    uint8_t  peer_mac[SETO_MAC_LEN];
    uint16_t opt_len;
    /* Followed by uint8_t opt_data[opt_len] */
} seto_match_event_t;

/* 0x30 / 0x31: SETO_OP_PDP_BIND / UNBIND (C -> S) */
typedef struct seto_pdp_port {
    uint32_t client_pdp_id;
    uint16_t port;
    uint16_t reserved;
} seto_pdp_port_t;

/* 0x32: SETO_OP_PDP_SEND (C -> S) */
typedef struct seto_pdp_send {
    uint16_t src_port;
    uint8_t  dst_mac[SETO_MAC_LEN];
    uint16_t dst_port;
    uint16_t data_len;
    /* Followed by uint8_t data[data_len] */
} seto_pdp_send_t;

/* 0x33: SETO_OP_PDP_RECV (S -> C) */
typedef struct seto_pdp_recv {
    uint8_t  src_mac[SETO_MAC_LEN];
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t data_len;
    /* Followed by uint8_t data[data_len] */
} seto_pdp_recv_t;

/* 0x40: SETO_OP_PTP_LISTEN (C -> S) */
typedef struct seto_ptp_listen {
    uint32_t client_listen_id;
    uint16_t src_port;
    uint16_t backlog;
} seto_ptp_listen_t;

/* 0x41: SETO_OP_PTP_OPEN (C -> S) */
typedef struct seto_ptp_open {
    uint32_t client_sock_id;
    uint16_t src_port;
    uint8_t  dst_mac[SETO_MAC_LEN];
    uint16_t dst_port;
    uint16_t reserved;
} seto_ptp_open_t;

/* 0x42: SETO_OP_PTP_INCOMING (S -> C, delivered to listener) */
typedef struct seto_ptp_incoming {
    uint32_t stream_id;
    uint32_t client_listen_id;
    uint8_t  peer_mac[SETO_MAC_LEN];
    uint16_t peer_port;
    uint16_t local_port;
    uint16_t reserved;
} seto_ptp_incoming_t;

/* 0x43: SETO_OP_PTP_ACCEPT (C -> S, binds guest accepted socket to stream_id) */
typedef struct seto_ptp_accept {
    uint32_t stream_id;
    uint32_t client_sock_id;
} seto_ptp_accept_t;

/* 0x44: SETO_OP_PTP_CONNECTED (S -> C, confirms active bidirectional stream) */
typedef struct seto_ptp_connected {
    uint32_t stream_id;
    uint32_t client_sock_id;
    uint8_t  peer_mac[SETO_MAC_LEN];
    uint16_t peer_port;
} seto_ptp_connected_t;

/* 0x45: SETO_OP_PTP_DATA (C -> S: sock_id = sender sock; S -> C: sock_id = receiver sock) */
typedef struct seto_ptp_data {
    uint32_t client_sock_id;
    uint16_t data_len;
    uint16_t reserved;
    /* Followed by uint8_t data[data_len] */
} seto_ptp_data_t;

/* 0x46: SETO_OP_PTP_CLOSE (C -> S) */
typedef struct seto_ptp_close {
    uint32_t client_sock_id;
} seto_ptp_close_t;

/* 0x47: SETO_OP_PTP_CLOSED (S -> C) */
typedef struct seto_ptp_closed {
    uint32_t client_sock_id;
    uint32_t reason; /* 0 = clean EOF/close, or PSP_ADHOC_ERROR_* */
} seto_ptp_closed_t;

#pragma pack(pop)

/* Static size sanity checks */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(seto_hdr_t) == 8, "seto_hdr_t must be 8 bytes");
_Static_assert(sizeof(seto_hello_t) == 148, "seto_hello_t must be 148 bytes");
_Static_assert(sizeof(seto_welcome_t) == 12, "seto_welcome_t must be 12 bytes");
_Static_assert(sizeof(seto_peer_entry_t) == 136, "seto_peer_entry_t must be 136 bytes");
#endif

static inline int seto_mac_is_broadcast(const uint8_t mac[SETO_MAC_LEN]) {
    return mac[0] == 0xFFu && mac[1] == 0xFFu && mac[2] == 0xFFu &&
           mac[3] == 0xFFu && mac[4] == 0xFFu && mac[5] == 0xFFu;
}

static inline int seto_mac_is_zero(const uint8_t mac[SETO_MAC_LEN]) {
    return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0u;
}

static inline int seto_mac_equal(const uint8_t a[SETO_MAC_LEN], const uint8_t b[SETO_MAC_LEN]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
           a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

#ifdef __cplusplus
}
#endif

#endif /* SETO_PROTO_H */
