/*
 * seto_net_test.c — End-to-end HLE test driving seto_net.c through real
 * psprecomp guest RAM, NID dispatch, and guest MIPS callback invocation.
 *
 * Usage:
 *   SETO_SERVER=127.0.0.1:<port> SETO_NICKNAME=Jaden ./test-seto-net host
 *   SETO_SERVER=127.0.0.1:<port> SETO_NICKNAME=Kaiba ./test-seto-net joiner
 */
#include "../src/seto_net.h"
#include "../src/seto_proto.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define GUEST_CTL_CB_ADDR   0x09481000u
#define GUEST_MATCH_CB_ADDR 0x09482000u

static int     g_ctl_events[16];
static int     g_ctl_n;

static int     g_match_events[16];
static uint8_t g_match_peer_mac[6];
static char    g_match_last_opt[128];
static int     g_match_n;

static void guest_ctl_cb(void) {
    if (g_ctl_n < 16)
        g_ctl_events[g_ctl_n++] = (int)psp_cpu.r[PSP_REG_A0];
}

static void guest_match_cb(void) {
    int      ev      = (int)psp_cpu.r[PSP_REG_A1];
    uint32_t mac_ptr = psp_cpu.r[PSP_REG_A2];
    uint32_t opt_len = psp_cpu.r[PSP_REG_A3];
    uint32_t opt_ptr = psp_cpu.r[PSP_REG_T0];

    if (mac_ptr) {
        for (uint32_t i = 0; i < 6u; i++)
            g_match_peer_mac[i] = psp_read8(mac_ptr + i);
    }
    memset(g_match_last_opt, 0, sizeof(g_match_last_opt));
    if (opt_len > 0 && opt_ptr) {
        uint32_t n = opt_len < 127u ? opt_len : 127u;
        for (uint32_t i = 0; i < n; i++)
            g_match_last_opt[i] = (char)psp_read8(opt_ptr + i);
    }
    if (g_match_n < 16)
        g_match_events[g_match_n++] = ev;
}

static uint32_t call_hle(uint32_t nid,
                         uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t t0, uint32_t t1, uint32_t t2, uint32_t t3,
                         uint32_t sp0) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_cpu.r[PSP_REG_T0] = t0;
    psp_cpu.r[PSP_REG_T1] = t1;
    psp_cpu.r[PSP_REG_T2] = t2;
    psp_cpu.r[PSP_REG_T3] = t3;
    psp_write32(psp_cpu.r[PSP_REG_SP], sp0);
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

static int has_match_event(int ev) {
    for (int i = 0; i < g_match_n; i++) {
        if (g_match_events[i] == ev) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const int is_host = (argc < 2 || strcmp(argv[1], "host") == 0);

    CHECK(psp_mem_init() == 0);
    psp_cpu_reset();
    psp_hle_init();
    seto_net_register_hle();

    psp_register(GUEST_CTL_CB_ADDR, guest_ctl_cb);
    psp_register(GUEST_MATCH_CB_ADDR, guest_match_cb);

    /* Set up a clean guest stack in user RAM */
    const uint32_t scratch = 0x08810000u;
    psp_cpu.r[PSP_REG_SP] = 0x0880F000u;

    /* 1. sceNetInit + sceWlanGetEtherAddr */
    CHECK(call_hle(0x39AF39A6u, 0x20000, 0x20, 0x1000, 0x20, 0x1000, 0, 0, 0, 0) == 0);
    CHECK(seto_net_is_online() == 1);
    CHECK(call_hle(0x0C622081u, scratch, 0, 0, 0, 0, 0, 0, 0, 0) == 0);

    uint8_t my_mac[6];
    for (uint32_t i = 0; i < 6u; i++) my_mac[i] = psp_read8(scratch + i);
    CHECK(my_mac[0] == 0x02u && my_mac[1] == 0x53u && my_mac[2] == 0x54u);

    /* 2. sceNetAdhocInit + sceNetAdhocctlInit + AddHandler */
    CHECK(call_hle(0xE1D621D7u, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(call_hle(0xE26F226Eu, 0x2000, 0x30, 0, 0, 0, 0, 0, 0, 0) == 0);
    uint32_t hid = call_hle(0x20B317A0u, GUEST_CTL_CB_ADDR, 0x1234u, 0, 0, 0, 0, 0, 0, 0);
    CHECK((int32_t)hid > 0);

    /* 3. sceUtilityNetconfInitStart(CONNECT_ADHOC_EX=3, group="TF1DUEL1") */
    const uint32_t netconf = scratch + 0x100u;
    const uint32_t adhocp  = scratch + 0x200u;
    for (uint32_t i = 0; i < 0x80u; i++) psp_write8(netconf + i, 0);
    const char *grp = "TF1DUEL1";
    for (uint32_t i = 0; i < 8u; i++) psp_write8(adhocp + i, (uint8_t)grp[i]);
    psp_write32(netconf + 0x30u, 3u);
    psp_write32(netconf + 0x34u, adhocp);

    CHECK(call_hle(0x4DB1E739u, netconf, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(call_hle(0x6332AA39u, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 1u); /* INIT -> RUN */
    CHECK(call_hle(0x91E70E35u, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0u);  /* Update -> QUIT */
    CHECK(call_hle(0x6332AA39u, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 3u); /* QUIT */

    CHECK(call_hle(0x75ECD386u, scratch + 0x300u, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(psp_read32(scratch + 0x300u) == 1u);
    CHECK(g_ctl_n >= 1 && g_ctl_events[0] == PSP_ADHOCCTL_EVENT_CONNECT);

    /* 4. sceNetAdhocMatchingInit + Create (9th arg on $sp+0) + Start */
    CHECK(call_hle(0x2A2A1E07u, 0x20000, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    uint32_t mid = call_hle(0xCA5EDA6Fu,
                            PSP_ADHOC_MATCHING_MODE_PTP, 2, 0x22Bu, 0x800,
                            500000u, 500000u, 3, 500000u,
                            GUEST_MATCH_CB_ADDR);
    CHECK((int32_t)mid > 0);

    const char *hello_str = is_host ? "DECK_HERO" : "DECK_DRAGON";
    const uint32_t hello_ptr = scratch + 0x400u;
    for (size_t i = 0; i <= strlen(hello_str); i++)
        psp_write8(hello_ptr + (uint32_t)i, (uint8_t)hello_str[i]);

    CHECK(call_hle(0x93EF3843u, mid, 0x10, 0x2000, 0x10, 0x2000,
                   (uint32_t)strlen(hello_str), hello_ptr, 0, 0) == 0);

    /* Wait for HELLO (1) from peer */
    for (int i = 0; i < 400 && !has_match_event(PSP_ADHOC_MATCHING_EVENT_HELLO); i++) {
        psp_threadman_host_sleep_ms(10u);
        seto_net_pump_callbacks();
    }
    CHECK(has_match_event(PSP_ADHOC_MATCHING_EVENT_HELLO));
    CHECK(strcmp(g_match_last_opt, is_host ? "DECK_DRAGON" : "DECK_HERO") == 0);

    /* Verify PeerInfo & GetNameByAddr resolved from PEER_SNAPSHOT */
    const uint32_t peer_mac_addr = scratch + 0x480u;
    for (uint32_t i = 0; i < 6u; i++) psp_write8(peer_mac_addr + i, g_match_peer_mac[i]);
    CHECK(call_hle(0x8916C003u, peer_mac_addr, scratch + 0x500u, 0, 0, 0, 0, 0, 0, 0) == 0);
    char peer_nick[32] = { 0 };
    for (uint32_t i = 0; i < 31u; i++) peer_nick[i] = (char)psp_read8(scratch + 0x500u + i);
    CHECK(strcmp(peer_nick, is_host ? "Kaiba" : "Jaden") == 0);

    /* Verify PeerInfo.last_recv matches sceKernelGetSystemTimeWide clock domain */
    CHECK(call_hle(0x8DB83FDCu, peer_mac_addr, 152u, scratch + 0x520u, 0, 0, 0, 0, 0, 0) == 0);
    uint64_t peer_ts = (uint64_t)psp_read32(scratch + 0x520u + 144u) |
                       ((uint64_t)psp_read32(scratch + 0x520u + 148u) << 32);
    psp_hle_call(0x82BC5777u);
    uint64_t sys_ts = (uint64_t)psp_cpu.r[PSP_REG_V0] | ((uint64_t)psp_cpu.r[PSP_REG_V1] << 32);
    CHECK(sys_ts >= peer_ts && (sys_ts - peer_ts) < 1000000ULL);

    /* Joiner challenges Host -> Host accepts -> Both reach COMPLETE (7) */
    if (!is_host) {
        CHECK(call_hle(0x5E3D4B79u, mid, peer_mac_addr, 0, 0, 0, 0, 0, 0, 0) == 0);
    } else {
        for (int i = 0; i < 400 && !has_match_event(PSP_ADHOC_MATCHING_EVENT_JOIN); i++) {
            psp_threadman_host_sleep_ms(10u);
            seto_net_pump_callbacks();
        }
        CHECK(has_match_event(PSP_ADHOC_MATCHING_EVENT_JOIN));
        CHECK(call_hle(0x5E3D4B79u, mid, peer_mac_addr, 0, 0, 0, 0, 0, 0, 0) == 0);
    }

    for (int i = 0; i < 400 && !has_match_event(PSP_ADHOC_MATCHING_EVENT_COMPLETE); i++) {
        psp_threadman_host_sleep_ms(10u);
        seto_net_pump_callbacks();
    }
    CHECK(has_match_event(PSP_ADHOC_MATCHING_EVENT_COMPLETE));

    /* 5. Exchange duel data over PTP + PDP */
    if (is_host) {
        uint32_t lst = call_hle(0xE08BDAC1u, scratch, 0x400u, 0x1000u, 100000u, 3, 2, 0, 0, 0);
        CHECK((int32_t)lst > 0);
        uint32_t cli = call_hle(0x9DF81198u, lst, scratch + 0x600u, scratch + 0x610u, 3000000u, 0, 0, 0, 0, 0);
        CHECK((int32_t)cli > 0);

        psp_write32(scratch + 0x620u, 64u);
        CHECK(call_hle(0x8BEA2B3Eu, cli, scratch + 0x700u, scratch + 0x620u, 3000000u, 0, 0, 0, 0, 0) == 0);
        CHECK(psp_read32(scratch + 0x620u) == 10u);
        char msg[16] = { 0 };
        for (int i = 0; i < 10; i++) msg[i] = (char)psp_read8(scratch + 0x700u + (uint32_t)i);
        CHECK(strcmp(msg, "SUMMON_BEW") == 0);
        CHECK(call_hle(0x157E6225u, cli, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    } else {
        uint32_t s = call_hle(0x877F6D66u, scratch, 0x401u, peer_mac_addr, 0x400u, 0x1000u, 100000u, 3, 0, 0);
        CHECK((int32_t)s > 0);
        const char *m = "SUMMON_BEW";
        for (int i = 0; i < 10; i++) psp_write8(scratch + 0x700u + (uint32_t)i, (uint8_t)m[i]);
        psp_write32(scratch + 0x620u, 10u);
        CHECK(call_hle(0x4DA4C788u, s, scratch + 0x700u, scratch + 0x620u, 3000000u, 0, 0, 0, 0, 0) == 0);
        CHECK(call_hle(0xFC6FC07Bu, s, 3000000u, 0, 0, 0, 0, 0, 0, 0) == 0);
        CHECK(call_hle(0x157E6225u, s, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    }

    seto_net_shutdown();
    psp_mem_free();
    printf("[PASS] seto_net HLE role=%s completed full Netconf + Matching + PTP duel flow!\n",
           is_host ? "host" : "joiner");
    return 0;
}
