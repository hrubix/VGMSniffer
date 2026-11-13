#include "vgm_scc.h"

#include <string.h>
#include <stdlib.h>

uint32_t vgm_scc_emu_clock_hz(uint32_t vgm_header_hz)
{
    uint32_t hz = vgm_header_hz & 0x7fffffffu;

    if (hz == 0) {
        return (uint32_t)VGM_SCC_DEFAULT_CLOCK_HZ;
    }
    /* MSX VGMs log PSG-bus rate (~1.79 MHz); SCC runs at MSX master (~3.58 MHz). */
    if (hz < 2500000u) {
        return hz * 2u;
    }
    return hz;
}

void vgm_scc_activate(SCC *scc)
{
    if (!scc) {
        return;
    }
    scc->active = 1;
    scc->ch_enable = 0xff;
    scc->ch_enable_next = 0xff;
}

void vgm_scc_write_d2(SCC *scc, int scc_plus, uint8_t port, uint8_t aa, uint8_t dd)
{
    if (!scc) {
        return;
    }

    port &= 0x7fu;
    aa &= 0xffu;
    dd &= 0xffu;

    if (port == 0x00) {
        if (aa < 0x80u) {
            SCC_writeReg(scc, aa, dd);
        } else if (!scc_plus) {
            SCC_writeReg(scc, aa, dd);
        }
        return;
    }
    if (port == 0x01 && aa < 10u) {
        SCC_writeReg(scc, 0xC0u + aa, dd);
        return;
    }
    if (port == 0x02 && aa < 5u) {
        SCC_writeReg(scc, 0xD0u + aa, dd);
        return;
    }
    if (port == 0x03 && aa == 0u) {
        SCC_writeReg(scc, 0xE1u, dd);
        return;
    }
    if (port == 0x04 && scc_plus && aa < 0xA0u) {
        /* SCC+ waveform RAM at 0xB800 (openMSX port 4): aa 0..0x9F → enhanced waves ch 1..5. */
        SCC_writeReg(scc, aa, dd);
    }
}

void vgm_scc_apply_mute_mask(SCC *scc, uint8_t mute_mask)
{
    uint32_t mask = 0;
    int ch;

    if (!scc) {
        return;
    }

    for (ch = 0; ch < 5; ch++) {
        if (mute_mask & (1u << ch)) {
            mask |= SCC_MASK_CH(ch);
        }
    }
    SCC_setMask(scc, mask);
}

uint8_t vgm_scc_channel_level(const SCC *scc, int ch)
{
    uint32_t vol;
    uint32_t enable;

    if (!scc || ch < 0 || ch > 4) {
        return 0;
    }

    enable = (uint32_t)(scc->ch_enable & scc->ch_enable_next) & (1u << ch);
    if (!enable) {
        return 0;
    }

    vol = scc->volume[ch] & 0x0fu;
    if (vol >= 0x0fu) {
        return 0;
    }
    return (uint8_t)(0x0fu - vol);
}

static int scc_d2_channel(uint8_t port, uint8_t aa, int scc_plus)
{
    port &= 0x7fu;
    if (port == 0x01 && aa < 10u) {
        return (int)(aa / 2u);
    }
    if (port == 0x02 && aa < 5u) {
        return (int)aa;
    }
    if (port == 0x04 && scc_plus && aa < 0xA0u) {
        return (int)(aa >> 5);
    }
    if (port == 0x00 && aa < 0x80u) {
        if (aa < 32u) {
            return 0;
        }
        if (aa < 64u) {
            return 1;
        }
        if (aa < 96u) {
            return 2;
        }
        return 3;
    }
    return -1;
}

int vgm_scc_d2_skip_for_export(uint8_t port, uint8_t aa, uint8_t mute_mask, int scc_plus)
{
    int ch = scc_d2_channel(port, aa, scc_plus);
    if (ch < 0) {
        return 0;
    }
    return (mute_mask & (1u << ch)) != 0;
}

uint8_t vgm_scc_apply_mask_mute(uint8_t mask, uint8_t mute_mask)
{
    return (uint8_t)(mask & (uint8_t)(~mute_mask & 0x1fu));
}

static int snap_push(VgmSccSnapshot *snap, uint8_t port, uint8_t aa, uint8_t dd)
{
    size_t n = snap->count;
    uint8_t *ports = (uint8_t *)realloc(snap->ports, (n + 1u) * sizeof(uint8_t));
    uint8_t *aas = (uint8_t *)realloc(snap->aas, (n + 1u) * sizeof(uint8_t));
    uint8_t *dds = (uint8_t *)realloc(snap->dds, (n + 1u) * sizeof(uint8_t));
    if (!ports || !aas || !dds) {
        return -1;
    }
    snap->ports = ports;
    snap->aas = aas;
    snap->dds = dds;
    snap->ports[n] = port;
    snap->aas[n] = aa;
    snap->dds[n] = dd;
    snap->count = n + 1u;
    return 0;
}

void vgm_scc_snapshot_free(VgmSccSnapshot *snap)
{
    if (!snap) {
        return;
    }
    free(snap->ports);
    free(snap->aas);
    free(snap->dds);
    snap->ports = NULL;
    snap->aas = NULL;
    snap->dds = NULL;
    snap->count = 0;
}

int vgm_scc_capture_snapshot(const SCC *scc, int scc_plus, uint8_t mute_mask, VgmSccSnapshot *out)
{
    if (!scc || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    for (int ch = 0; ch < 5; ch++) {
        if (mute_mask & (1u << ch)) {
            continue;
        }
        if (ch == 4 && !scc_plus) {
            continue;
        }
        if (ch == 4 && scc_plus) {
            for (int i = 0; i < 32; i++) {
                if (snap_push(out, 0x04, (uint8_t)i, (uint8_t)scc->wave[ch][i]) != 0) {
                    vgm_scc_snapshot_free(out);
                    return -1;
                }
            }
            continue;
        }
        int base = ch < 3 ? ch * 32 : 96;
        for (int i = 0; i < 32; i++) {
            if (snap_push(out, 0x00, (uint8_t)(base + i), (uint8_t)scc->wave[ch][i]) != 0) {
                vgm_scc_snapshot_free(out);
                return -1;
            }
        }
    }

    for (int ch = 0; ch < 5; ch++) {
        if (mute_mask & (1u << ch)) {
            continue;
        }
        uint16_t freq = (uint16_t)(scc->freq[ch] & 0x0fffu);
        if (snap_push(out, 0x01, (uint8_t)(ch * 2), (uint8_t)(freq & 0xffu)) != 0 ||
            snap_push(out, 0x01, (uint8_t)(ch * 2 + 1), (uint8_t)((freq >> 8) & 0x0fu)) != 0) {
            vgm_scc_snapshot_free(out);
            return -1;
        }
    }

    for (int ch = 0; ch < 5; ch++) {
        if (mute_mask & (1u << ch)) {
            continue;
        }
        if (snap_push(out, 0x02, (uint8_t)ch, (uint8_t)(scc->volume[ch] & 0x0fu)) != 0) {
            vgm_scc_snapshot_free(out);
            return -1;
        }
    }

    {
        uint8_t mask =
            vgm_scc_apply_mask_mute((uint8_t)(scc->ch_enable & scc->ch_enable_next & 0x1fu), mute_mask);
        if (snap_push(out, 0x03, 0, mask) != 0) {
            vgm_scc_snapshot_free(out);
            return -1;
        }
    }

    return 0;
}
