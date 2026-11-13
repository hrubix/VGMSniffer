#ifndef VGM_PERIOD_SMOOTH_H
#define VGM_PERIOD_SMOOTH_H

#include "emu2149.h"

#include <stdint.h>

typedef struct VgmPeriodSmoothState {
    uint8_t shadow[6];
    uint16_t committed_period[3];
    uint8_t dirty_fine[3];
    uint8_t dirty_coarse[3];
} VgmPeriodSmoothState;

void vgm_period_smooth_reset(VgmPeriodSmoothState *st);
void vgm_period_smooth_sync_from_psg(VgmPeriodSmoothState *st, const PSG *psg);

/* Write R0-R5 to shadow. Returns 1 if channel ch should commit immediately. */
int vgm_period_smooth_write(VgmPeriodSmoothState *st, uint8_t reg, uint8_t val, int *ch_out);

/* Apply channel period to PSG from shadow. Returns effective 12-bit period. */
uint16_t vgm_period_smooth_apply_channel(VgmPeriodSmoothState *st, PSG *psg, int ch);

/* Returns bitmask (bit ch) of channels that should commit on this wait boundary. */
int vgm_period_smooth_on_wait(const VgmPeriodSmoothState *st);

/* Returns bitmask of channels with any pending dirty flags. */
int vgm_period_smooth_pending_mask(const VgmPeriodSmoothState *st);

#endif
