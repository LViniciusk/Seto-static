/* SETO-static S3: the minimum sceUmdUser surface Tag Force 1 calls.
 *
 * The drive is only ever "present" when a real disc image opened successfully;
 * WaitDriveStat checks the requested bit and reports a timeout when it is not
 * set, rather than pretending every wait succeeded.
 */
#include <psprecomp/hle.h>

#include <stdio.h>
#include <string.h>

/* pspumd.h state bits: 0x20 is the one Tag Force waits on after Activate. */
#define PSP_UMD_NOT_PRESENT 0x01
#define PSP_UMD_PRESENT     0x02
#define PSP_UMD_INITED      0x10
#define PSP_UMD_READY       0x20

#define UMD_ERROR_TIMEOUT   0x800201A8u   /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */

static int g_disc;      /* the ISO path opened at least once */
static int g_active;    /* sceUmdActivate succeeded on this drive */

void psp_umd_set_iso(const char *path) {
    g_disc = 0;
    psp_io_set_umd("");

    FILE *f = (path && path[0]) ? fopen(path, "rb") : NULL;
    if (f) {
        /* An ISO9660 primary volume descriptor lives at sector 16. Refuse a
         * file that is not a disc image rather than presenting a dead drive. */
        uint8_t pvd[7];
        if (fseek(f, 16 * 2048, SEEK_SET) == 0 &&
            fread(pvd, 1, sizeof pvd, f) == sizeof pvd &&
            pvd[0] == 0x01 && memcmp(pvd + 1, "CD001", 5) == 0) {
            g_disc = 1;
            psp_io_set_umd(path);
        }
        fclose(f);
    }
}

static uint32_t drive_stat(void) {
    if (!g_disc) return PSP_UMD_NOT_PRESENT;
    return PSP_UMD_PRESENT | PSP_UMD_INITED | (g_active ? PSP_UMD_READY : 0);
}

static void hle_Activate(void) {
    /* (unit, drive-prefix) */
    g_active = psp_arg(0) == 1 && g_disc;
    psp_ret(g_active ? 0 : 0x80010002u);
}

static void hle_Deactivate(void) {
    g_active = 0;
    psp_ret(0);
}

static void hle_GetDriveStat(void) {
    psp_ret(drive_stat());
}

static void hle_WaitDriveStat(void) {
    uint32_t want = psp_arg(0);
    psp_ret((drive_stat() & want) ? 0 : UMD_ERROR_TIMEOUT);
}

/* No callback/event machinery exists here, so the CB variant is the plain
 * check; the state is already settled by the time the guest asks. */
static void hle_WaitDriveStatCB(void) {
    hle_WaitDriveStat();
}

/* WaitDriveStat never parks in this static-disc host, so there are no pending
 * waits to cancel. In particular, cancellation must not deactivate the drive. */
static void hle_CancelWaitDriveStat(void) {
    psp_ret(0);
}

void psp_umd_register(void) {
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",       hle_Activate);
    psp_hle_register(0xE83742BA, "sceUmdUser", "sceUmdDeactivate",     hle_Deactivate);
    psp_hle_register(0x6AF9B50A, "sceUmdUser", "sceUmdCancelWaitDriveStat", hle_CancelWaitDriveStat);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",   hle_GetDriveStat);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",  hle_WaitDriveStat);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB", hle_WaitDriveStatCB);
}
