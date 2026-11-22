/*
 * seto_net.h — Seto-static HLE networking client for Seto-Server.
 *
 * Implements PSP WLAN, Net, UtilityNetconf, Adhoc, Adhocctl, and AdhocMatching
 * syscalls over a single TCP connection to Seto-Server (with automatic offline
 * fallback when SETO_SERVER / launcher.ini `server=` is not configured).
 */
#ifndef SETO_NET_H
#define SETO_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register all PSP network/adhoc/netconf HLE syscalls with psprecomp. */
void seto_net_register_hle(void);

/* Drain queued Adhocctl and AdhocMatching guest callbacks on the CPU-baton
 * holder. Safe to call from host.c's main loop and HLE polling points. */
void seto_net_pump_callbacks(void);

/* Close server socket, stop RX thread, and release host resources on exit. */
void seto_net_shutdown(void);

/* Returns 1 while connected to Seto-Server, 0 otherwise. */
int  seto_net_is_online(void);

#ifdef __cplusplus
}
#endif

#endif /* SETO_NET_H */
