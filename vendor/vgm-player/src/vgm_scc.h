#ifndef VGM_SCC_H
#define VGM_SCC_H

#include <stddef.h>

#include "emu2212.h"

/* MSX master oscillator; emu2212 expects this rate. */
#define VGM_SCC_DEFAULT_CLOCK_HZ 3579545u

/** Map VGM header K051649 clock (~1.79 MHz) to emu2212 master clock (~3.58 MHz). */
uint32_t vgm_scc_emu_clock_hz(uint32_t vgm_header_hz);

void vgm_scc_activate(SCC *scc);

/** Apply VGM 0xD2 write (openMSX port map: 0 wave, 1 freq, 2 vol, 3 mask, 4 SCC+ wave). */
void vgm_scc_write_d2(SCC *scc, int scc_plus, uint8_t port, uint8_t aa, uint8_t dd);

void vgm_scc_apply_mute_mask(SCC *scc, uint8_t mute_mask);

/** SCC channel level 0–15 for UI (inverted volume: 0 = loudest). */
uint8_t vgm_scc_channel_level(const SCC *scc, int ch);

/** True when a 0xD2 write targets a fully muted SCC channel (skip on export). */
int vgm_scc_d2_skip_for_export(uint8_t port, uint8_t aa, uint8_t mute_mask, int scc_plus);

/** Apply export mute mask to SCC channel-enable byte (port 3). */
uint8_t vgm_scc_apply_mask_mute(uint8_t mask, uint8_t mute_mask);

typedef struct {
    uint8_t *ports;
    uint8_t *aas;
    uint8_t *dds;
    size_t count;
} VgmSccSnapshot;

void vgm_scc_snapshot_free(VgmSccSnapshot *snap);

/* Capture SCC register/wave state for VGM export (caller frees with vgm_scc_snapshot_free). */
int vgm_scc_capture_snapshot(const SCC *scc, int scc_plus, uint8_t mute_mask, VgmSccSnapshot *out);

#endif
