#ifndef SETO_TF3_BRIDGE_H
#define SETO_TF3_BRIDGE_H

#include <stdint.h>

/* Exposed by duel_prx.c for tf3_bridge.c */
uint32_t duel_eboot_export_addr(uint32_t nid, uint32_t *gp_out);
uint32_t psp_duel_export(uint32_t nid, uint32_t *gp_out);

/* Map and populate the 4 MB GX extension window (0x06400000..0x067FFFFF):
 *   0x06400000..0x065FFFFF: relocated cardinfo_eng.ehp buffer (2 MB)
 *   0x06600000..0x0660FFFF: extended trunk / owned table for card_idx 2480..3479
 *   0x06610000..0x0661FFFF: modcardalbum extended list buffer
 *   0x06620000..0x0663FFFF: TF3 DLG_Text_E.bin & empty wstr for duel dialogs
 */
void gx_runtime_init(void);
uint32_t gx_cip_tbl_addr(uint32_t s0);
uint32_t gx_cip_prepare_tbl_read(uint32_t s0);
void gx_cip_setup_cache(uint32_t s0);

/* Initialize/zero scratch PSP RAM reserved at the tail of TF3 window (0x03850000..0x0385FFFF). */
void duel_tf3_on_duel_load(void);

extern int g_tf3_post_deck_trace;

/* Intercept TF1 modduel_{mgr,draw,tut} -> libduel_eng calls before dispatching to TF3 tf3eng.
 * Returns 1 if handled, 0 to fall through to standard psp_duel_export(nid). */
int duel_tf3_handle_export(uint32_t nid);

/* Import dispatcher for TF3's recompiled rel_duel_eng.prx (libehsys_rel). */
void tf3eng_import_forward(uint32_t nid);

#endif /* SETO_TF3_BRIDGE_H */
