#include "vgm_period_smooth.h"

#include <stddef.h>

static int period_from_shadow(const VgmPeriodSmoothState *st, int ch)
{
    uint8_t fine = st->shadow[(size_t)(ch * 2)];
    uint8_t coarse = st->shadow[(size_t)(ch * 2 + 1)];
    return (int)(fine | ((coarse & 0x0fu) << 8));
}

static uint16_t effective_period(const VgmPeriodSmoothState *st, int ch, int raw)
{
    if (raw > 0) {
        return (uint16_t)raw;
    }
    /* Coarse-first partial update: fine still 0 until the paired write arrives. */
    if (st->dirty_coarse[ch] && !st->dirty_fine[ch]) {
        return st->committed_period[(size_t)ch];
    }
    return 0;
}

void vgm_period_smooth_reset(VgmPeriodSmoothState *st)
{
    if (!st) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        st->shadow[i] = 0;
    }
    for (int i = 0; i < 3; i++) {
        st->committed_period[i] = 0;
        st->dirty_fine[i] = 0;
        st->dirty_coarse[i] = 0;
    }
}

void vgm_period_smooth_sync_from_psg(VgmPeriodSmoothState *st, const PSG *psg)
{
    int ch;

    if (!st || !psg) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        st->shadow[i] = psg->reg[i];
    }
    for (ch = 0; ch < 3; ch++) {
        int p = period_from_shadow(st, ch);
        st->committed_period[ch] = p > 0 ? (uint16_t)p : 0;
        st->dirty_fine[ch] = 0;
        st->dirty_coarse[ch] = 0;
    }
}

int vgm_period_smooth_write(VgmPeriodSmoothState *st, uint8_t reg, uint8_t val, int *ch_out)
{
    int ch;

    if (!st || reg > 5) {
        return 0;
    }

    ch = (int)(reg >> 1);
    st->shadow[reg] = val;
    if ((reg & 1u) == 0) {
        st->dirty_fine[ch] = 1;
    } else {
        st->dirty_coarse[ch] = 1;
    }

    if (ch_out) {
        *ch_out = ch;
    }

    if (st->dirty_fine[ch] && st->dirty_coarse[ch]) {
        return 1; /* commit now */
    }

    if ((reg & 1u) == 0 && st->dirty_fine[ch] && !st->dirty_coarse[ch]) {
        if (val == 0) {
            return 0; /* fine=0 may prelude a coarse write (percussion hit). */
        }
        /* Melody glides touch fine only while coarse stays 0. After a hit coarse can
         * still be 1 in shadow; hold until reg1 catches up (fe, wait 1, reg1=0). */
        if (st->shadow[(size_t)(ch * 2 + 1)] != 0) {
            return 0;
        }
        return 1;
    }

    /* Coarse-only (coarse-first pair): hold until the matching fine write. */
    return 0;
}

uint16_t vgm_period_smooth_apply_channel(VgmPeriodSmoothState *st, PSG *psg, int ch)
{
    int raw;
    uint16_t period;
    uint8_t fine;
    uint8_t coarse;

    if (!st || !psg || ch < 0 || ch > 2) {
        return 0;
    }

    raw = period_from_shadow(st, ch);
    period = effective_period(st, ch, raw);
    fine = (uint8_t)(period & 0xffu);
    coarse = (uint8_t)((period >> 8) & 0x0fu);

    PSG_writeReg(psg, (uint32_t)(ch * 2), fine);
    PSG_writeReg(psg, (uint32_t)(ch * 2 + 1), coarse);

    st->shadow[(size_t)(ch * 2)] = fine;
    st->shadow[(size_t)(ch * 2 + 1)] = coarse;
    st->committed_period[(size_t)ch] = period;
    st->dirty_fine[ch] = 0;
    st->dirty_coarse[ch] = 0;

    return period;
}

int vgm_period_smooth_on_wait(const VgmPeriodSmoothState *st)
{
    int mask = 0;
    int ch;

    if (!st) {
        return 0;
    }

    for (ch = 0; ch < 3; ch++) {
        int fine = st->dirty_fine[ch];
        int coarse = st->dirty_coarse[ch];
        if (!fine && !coarse) {
            continue;
        }
        if (fine && coarse) {
            mask |= 1 << ch;
        }
        /* Fine-only and coarse-only partial updates are held until paired or flush. */
    }
    return mask;
}

int vgm_period_smooth_pending_mask(const VgmPeriodSmoothState *st)
{
    int mask = 0;
    int ch;

    if (!st) {
        return 0;
    }
    for (ch = 0; ch < 3; ch++) {
        if (st->dirty_fine[ch] || st->dirty_coarse[ch]) {
            mask |= 1 << ch;
        }
    }
    return mask;
}
