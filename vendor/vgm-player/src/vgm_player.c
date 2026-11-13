#include "vgm_player.h"
#include "vgm_cmd.h"
#include "vgm_period_smooth.h"
#include "vgm_scc.h"
#include "vgmpsg.h"

#include <stdlib.h>
#include <string.h>

#define AY_REG_MIXER 7u

/* emu2149 ch_out is unipolar; emit signed square waves via tone/noise phase (edge). */
#define PSG_CH_OUT_MAX ((int32_t)(0xFF << 4))
#define PSG_PCM_MIX_CH 10923 /* ~32767/3 headroom for 3-channel sum */
/* emu2212 SCC_calc is much quieter than our scaled PSG bipolar path; boost to match. */
#define SCC_PCM_MIX_NUM 4
#define SCC_PCM_MIX_DEN 1

static int16_t clamp_pcm(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static int32_t scc_pcm_sample(VgmPlayer *player)
{
    int32_t raw;

    vgm_scc_apply_mute_mask(player->scc, player->scc_mute_mask);
    raw = (int32_t)SCC_calc(player->scc);
    return clamp_pcm((raw * SCC_PCM_MIX_NUM) / SCC_PCM_MIX_DEN);
}

static int channel_output_active(const PSG *psg, int ch)
{
    if (psg->mask & PSG_MASK_CH(ch)) {
        return 0;
    }
    if (0 < psg->freq_limit && psg->freq[ch] <= psg->freq_limit && psg->nmask[ch]) {
        return 0;
    }
    return 1;
}

static int32_t channel_mag_pcm(const PSG *psg, int ch, int32_t scale_max)
{
    uint8_t vol_idx;

    if (!(psg->volume[ch] & 32)) {
        vol_idx = (uint8_t)(psg->volume[ch] & 31u);
    } else {
        vol_idx = psg->env_ptr;
    }

    int32_t mag = (int32_t)(psg->voltbl[vol_idx] << 4);
    if (mag <= 0) {
        return 0;
    }
    return (int32_t)(((int64_t)mag * scale_max) / PSG_CH_OUT_MAX);
}

static int32_t channel_bipolar_pcm(const PSG *psg, int ch, uint8_t noise_bit, int32_t scale_max)
{
    int32_t mag;

    if (!channel_output_active(psg, ch)) {
        return 0;
    }

    mag = channel_mag_pcm(psg, ch, scale_max);
    if (mag == 0) {
        return 0;
    }

    if ((psg->tmask[ch] || psg->edge[ch]) && (psg->nmask[ch] || noise_bit)) {
        return mag;
    }
    return -mag;
}

static int32_t channel_tone_bipolar_pcm(const PSG *psg, int ch, int32_t scale_max)
{
    int32_t mag;

    if (!channel_output_active(psg, ch)) {
        return 0;
    }

    mag = channel_mag_pcm(psg, ch, scale_max);
    if (mag == 0) {
        return 0;
    }

    if (psg->tmask[ch]) {
        return 0;
    }

    if (psg->edge[ch]) {
        return mag;
    }
    return -mag;
}

static int32_t channel_noise_bipolar_pcm(const PSG *psg, int ch, uint8_t noise_bit, int32_t scale_max)
{
    int32_t mag;

    if (!channel_output_active(psg, ch)) {
        return 0;
    }

    mag = channel_mag_pcm(psg, ch, scale_max);
    if (mag == 0) {
        return 0;
    }

    if (psg->nmask[ch]) {
        return 0;
    }

    if (noise_bit) {
        return mag;
    }
    return -mag;
}


/* libvgm VGMPlayer::Tick2Sample / Sample2Tick (_tsMult = outRate, _tsDiv = 44100). */
static uint32_t tick_to_output_samples(uint32_t ticks_44100)
{
    return (uint32_t)(((uint64_t)ticks_44100 * (uint64_t)VGMPSG_OUTPUT_RATE +
                       (uint64_t)VGMPSG_VGM_WAIT_RATE / 2) /
                      (uint64_t)VGMPSG_VGM_WAIT_RATE);
}

static uint32_t output_samples_to_vgm_tick(uint64_t out_samples)
{
    return (uint32_t)((out_samples * (uint64_t)VGMPSG_VGM_WAIT_RATE +
                       (uint64_t)VGMPSG_OUTPUT_RATE / 2) /
                      (uint64_t)VGMPSG_OUTPUT_RATE);
}

static uint32_t output_samples_to_vgm_wait(uint32_t out_samples)
{
    return (uint32_t)(((uint64_t)out_samples * (uint64_t)VGMPSG_VGM_WAIT_RATE +
                       (uint64_t)VGMPSG_OUTPUT_RATE / 2) /
                      (uint64_t)VGMPSG_OUTPUT_RATE);
}

static uint8_t apply_mixer_mute(uint8_t reg7, uint8_t mute_mask)
{
    return (uint8_t)(reg7 | (mute_mask & 0x3fu));
}

/* Timeline stem masks: mute_mask matches web/app.js STEM_MASK. */
#define STEM_TONE_A_MASK                                                                    \
    (VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_A | VGMPSG_MUTE_NOISE_B | \
     VGMPSG_MUTE_NOISE_C)
#define STEM_TONE_B_MASK                                                                    \
    (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_A | VGMPSG_MUTE_NOISE_B | \
     VGMPSG_MUTE_NOISE_C)
#define STEM_TONE_C_MASK                                                                    \
    (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_NOISE_A | VGMPSG_MUTE_NOISE_B | \
     VGMPSG_MUTE_NOISE_C)
#define STEM_NOISE_A_MASK                                                                 \
    (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_B | \
     VGMPSG_MUTE_NOISE_C)
#define STEM_NOISE_B_MASK                                                                 \
    (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_A | \
     VGMPSG_MUTE_NOISE_C)
#define STEM_NOISE_C_MASK                                                                 \
    (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_A | \
     VGMPSG_MUTE_NOISE_B)

#define AY_R7_TONE_A_OFF 0x01u
#define AY_R7_TONE_B_OFF 0x02u
#define AY_R7_TONE_C_OFF 0x04u
#define AY_R7_NOISE_A_OFF 0x08u
#define AY_R7_NOISE_B_OFF 0x10u
#define AY_R7_NOISE_C_OFF 0x20u

static int game_noise_enabled(uint8_t game_r7, int ch)
{
    static const uint8_t noise_off[] = {AY_R7_NOISE_A_OFF, AY_R7_NOISE_B_OFF, AY_R7_NOISE_C_OFF};
    return ch >= 0 && ch <= 2 && (game_r7 & noise_off[ch]) == 0;
}

static int game_tone_enabled(uint8_t game_r7, int ch)
{
    static const uint8_t tone_off[] = {AY_R7_TONE_A_OFF, AY_R7_TONE_B_OFF, AY_R7_TONE_C_OFF};
    return ch >= 0 && ch <= 2 && (game_r7 & tone_off[ch]) == 0;
}

/**
 * Mixer used for stem isolation. If the stream never wrote R7, assume a
 * music-oriented default: tones on, noises off (0x38). AY power-on is often
 * 0 (noise+tone), which falsely fills Noise timeline rows for BASIC/tracker
 * dumps that never program the mixer.
 */
static uint8_t effective_game_r7(const VgmPlayer *player)
{
    return player->game_r7_valid ? player->game_r7 : (uint8_t)(AY_R7_NOISE_A_OFF | AY_R7_NOISE_B_OFF | AY_R7_NOISE_C_OFF);
}

static int is_tone_stem_solo_mask(uint8_t mm)
{
    return mm == STEM_TONE_A_MASK || mm == STEM_TONE_B_MASK || mm == STEM_TONE_C_MASK;
}

static uint16_t channel_tone_period(const PSG *psg, int ch)
{
    if (!psg || ch < 0 || ch > 2) {
        return 0;
    }
    return (uint16_t)(psg->reg[(size_t)(ch * 2)] |
                      ((uint16_t)(psg->reg[(size_t)(ch * 2 + 1)] & 0x0fu) << 8));
}

/* Percussion hit: short period, then period 0, then tone-off — not coarse/fine bookkeeping. */
#define MELODY_HIT_PERIOD_MAX 512u
#define MELODY_HIT_GESTURE_MAX_TICKS 2500u

static int scan_ahead_hit_gesture_within_ticks(const VgmPlayer *player, size_t start_pos, int ch,
                                               uint32_t max_ticks)
{
    const uint8_t *data;
    size_t end;
    size_t pos;
    uint32_t ticks = 0;
    uint8_t fine;
    uint8_t coarse;
    const uint8_t fine_reg = (uint8_t)(ch * 2);
    const uint8_t coarse_reg = (uint8_t)(ch * 2 + 1);
    static const uint8_t tone_off[] = {AY_R7_TONE_A_OFF, AY_R7_TONE_B_OFF, AY_R7_TONE_C_OFF};
    uint16_t cur_period;
    int state;

    if (!player || !player->file || !player->file->data || ch < 0 || ch > 2) {
        return 0;
    }

    data = player->file->data;
    end = player->stream_cmd_end;
    pos = start_pos;
    fine = (uint8_t)player->psg->reg[fine_reg];
    coarse = (uint8_t)(player->psg->reg[coarse_reg] & 0x0fu);
    cur_period = (uint16_t)fine | ((uint16_t)(coarse & 0x0fu) << 8);
    if (cur_period > 0 && cur_period <= MELODY_HIT_PERIOD_MAX) {
        state = 1;
    } else {
        state = 0;
    }

    while (pos < end && ticks < max_ticks) {
        uint8_t cmd = data[pos];

        if (cmd >= 0x70 && cmd <= 0x7f) {
            ticks += (uint32_t)(cmd - 0x70 + 1);
            pos += 1u;
            continue;
        }
        if (cmd == 0x61) {
            if (pos + 3 > end) {
                break;
            }
            ticks += (uint32_t)data[pos + 1] | ((uint32_t)data[pos + 2] << 8);
            pos += 3u;
            continue;
        }
        if (cmd == 0x62) {
            ticks += 735u;
            pos += 1u;
            continue;
        }
        if (cmd == 0x63) {
            ticks += 882u;
            pos += 1u;
            continue;
        }
        if (cmd == 0x66) {
            break;
        }
        if (cmd == 0xA0) {
            if (pos + 3 > end) {
                break;
            }
            if ((data[pos + 1] & 0x80u) == 0) {
                uint8_t reg = (uint8_t)(data[pos + 1] & 0x7fu);
                uint8_t val = data[pos + 2];

                if (reg == AY_REG_MIXER) {
                    if (state == 2 && (val & tone_off[ch])) {
                        return 1;
                    }
                } else if (reg == fine_reg) {
                    fine = val;
                } else if (reg == coarse_reg) {
                    coarse = val;
                }

                if (reg == fine_reg || reg == coarse_reg) {
                    uint16_t period = (uint16_t)fine | ((uint16_t)(coarse & 0x0fu) << 8);

                    if (state == 2) {
                        if (period > 0) {
                            return 0;
                        }
                    } else if (state == 1) {
                        if (period == 0) {
                            state = 2;
                        } else if (period > 0 && period <= MELODY_HIT_PERIOD_MAX) {
                            return 0;
                        } else if (period > MELODY_HIT_PERIOD_MAX) {
                            return 0;
                        }
                    } else if (period > 0 && period <= MELODY_HIT_PERIOD_MAX) {
                        state = 1;
                    }
                }
            }
            pos += 3u;
            continue;
        }
        if (cmd == 0x50) {
            pos += 3u;
            continue;
        }
        if (cmd >= 0x30 && cmd <= 0x4e) {
            pos += 2u;
            continue;
        }
        if (cmd >= 0x51 && cmd <= 0x5f) {
            pos += 3u;
            continue;
        }
        pos += 1u;
    }
    return state == 2 ? 1 : 0;
}

static int tone_melody_only_allowed(VgmPlayer *player, int ch)
{
    uint16_t period;
    uint8_t r7 = effective_game_r7(player);

    if (!game_tone_enabled(r7, ch)) {
        player->tone_hit_bleed[ch] = 0;
        return 0;
    }
    if (game_noise_enabled(r7, ch)) {
        player->tone_hit_bleed[ch] = 0;
        return 0;
    }

    if (scan_ahead_hit_gesture_within_ticks(player, player->stream_pos, ch,
                                            MELODY_HIT_GESTURE_MAX_TICKS)) {
        player->tone_hit_bleed[ch] = 1;
        return 0;
    }

    period = channel_tone_period(player->psg, ch);
    if (period > MELODY_HIT_PERIOD_MAX) {
        player->tone_hit_bleed[ch] = 0;
    }

    if (player->tone_hit_bleed[ch]) {
        return 0;
    }
    return 1;
}

static int channel_hw_unmuted_in_mix(uint8_t mute_mask, int ch)
{
    static const uint8_t tone_mute[] = {VGMPSG_MUTE_TONE_A, VGMPSG_MUTE_TONE_B, VGMPSG_MUTE_TONE_C};
    static const uint8_t noise_mute[] = {VGMPSG_MUTE_NOISE_A, VGMPSG_MUTE_NOISE_B,
                                         VGMPSG_MUTE_NOISE_C};

    return !(mute_mask & tone_mute[ch]) && !(mute_mask & noise_mute[ch]);
}

static int tone_stem_output_allowed(VgmPlayer *player, int ch)
{
    uint8_t mm = player->mute_mask;
    uint8_t r7 = effective_game_r7(player);
    int hw_channel_solo = mm != 0 && channel_hw_unmuted_in_mix(mm, ch);

    if (is_tone_stem_solo_mask(mm) || hw_channel_solo) {
        if (player->exclusive_solo) {
            return tone_melody_only_allowed(player, ch);
        }
        return game_tone_enabled(r7, ch);
    }
    if (!game_tone_enabled(r7, ch)) {
        return 0;
    }
    if (player->exclusive_solo && game_noise_enabled(r7, ch)) {
        return 0;
    }
    return 1;
}

static int channel_tone_only_in_mix(uint8_t mute_mask, int ch)
{
    static const uint8_t tone_mute[] = {VGMPSG_MUTE_TONE_A, VGMPSG_MUTE_TONE_B, VGMPSG_MUTE_TONE_C};
    static const uint8_t noise_mute[] = {VGMPSG_MUTE_NOISE_A, VGMPSG_MUTE_NOISE_B,
                                         VGMPSG_MUTE_NOISE_C};

    if (mute_mask == 0) {
        return 0;
    }
    return !(mute_mask & tone_mute[ch]) && (mute_mask & noise_mute[ch]);
}

static int noise_stem_output_allowed(const VgmPlayer *player, int ch)
{
    return game_noise_enabled(effective_game_r7(player), ch);
}

/* Per-channel timeline stems: one hardware channel output, gated by R7 tone/noise enables. */
static int16_t stem_isolate_sample(VgmPlayer *player, int16_t mixed);

static int is_stem_mute_mask(uint8_t mm)
{
    return mm == STEM_TONE_A_MASK || mm == STEM_TONE_B_MASK || mm == STEM_TONE_C_MASK ||
           mm == STEM_NOISE_A_MASK || mm == STEM_NOISE_B_MASK || mm == STEM_NOISE_C_MASK;
}

static int channel_silent_in_mix(uint8_t mute_mask, int ch)
{
    static const uint8_t tone_mute[] = {VGMPSG_MUTE_TONE_A, VGMPSG_MUTE_TONE_B, VGMPSG_MUTE_TONE_C};
    static const uint8_t noise_mute[] = {VGMPSG_MUTE_NOISE_A, VGMPSG_MUTE_NOISE_B,
                                         VGMPSG_MUTE_NOISE_C};

    return (mute_mask & tone_mute[ch]) && (mute_mask & noise_mute[ch]);
}

static int16_t calc_playback_sample(VgmPlayer *player)
{
    uint8_t mm = player->mute_mask;
    PSG *psg = player->psg;
    uint8_t noise_bit;
    int32_t scale_max;
    int32_t sum;
    int i;

    (void)PSG_calc(psg);

    if (mm == VGMPSG_MUTE_ALL) {
        if (player->scc) {
            return (int16_t)scc_pcm_sample(player);
        }
        return 0;
    }
    if (is_stem_mute_mask(mm)) {
        return stem_isolate_sample(player, 0);
    }

    noise_bit = (uint8_t)(psg->noise_seed & 1u);
    scale_max = PSG_PCM_MIX_CH;
    sum = 0;
    for (i = 0; i < 3; i++) {
        if (mm != 0 && channel_silent_in_mix(mm, i)) {
            continue;
        }
        if (channel_tone_only_in_mix(mm, i)) {
            if (tone_stem_output_allowed(player, i)) {
                sum += channel_tone_bipolar_pcm(psg, i, scale_max);
            }
            continue;
        }
        if (mm != 0 && channel_hw_unmuted_in_mix(mm, i)) {
            /* Chip mix: tone and noise are ANDed, not added. */
            const int tone = tone_stem_output_allowed(player, i);
            const int noise = noise_stem_output_allowed(player, i);
            if (tone && noise) {
                sum += channel_bipolar_pcm(psg, i, noise_bit, scale_max);
            } else if (tone) {
                sum += channel_tone_bipolar_pcm(psg, i, scale_max);
            } else if (noise) {
                sum += channel_noise_bipolar_pcm(psg, i, noise_bit, scale_max);
            }
            continue;
        }
        sum += channel_bipolar_pcm(psg, i, noise_bit, scale_max);
    }
    {
        int32_t mixed = clamp_pcm(sum);
        if (player->scc && !is_stem_mute_mask(mm)) {
            mixed = clamp_pcm(mixed + scc_pcm_sample(player));
        }
        return (int16_t)mixed;
    }
}

static int16_t stem_isolate_sample(VgmPlayer *player, int16_t mixed)
{
    uint8_t mm = player->mute_mask;
    PSG *psg = player->psg;
    uint8_t noise_bit = (uint8_t)(psg->noise_seed & 1u);

    if (mm == STEM_TONE_A_MASK) {
        return tone_stem_output_allowed(player, 0)
                   ? clamp_pcm(channel_tone_bipolar_pcm(psg, 0, PSG_PCM_MIX_CH))
                   : 0;
    }
    if (mm == STEM_TONE_B_MASK) {
        return tone_stem_output_allowed(player, 1)
                   ? clamp_pcm(channel_tone_bipolar_pcm(psg, 1, PSG_PCM_MIX_CH))
                   : 0;
    }
    if (mm == STEM_TONE_C_MASK) {
        return tone_stem_output_allowed(player, 2)
                   ? clamp_pcm(channel_tone_bipolar_pcm(psg, 2, PSG_PCM_MIX_CH))
                   : 0;
    }
    if (mm == STEM_NOISE_A_MASK) {
        return noise_stem_output_allowed(player, 0)
                   ? clamp_pcm(channel_noise_bipolar_pcm(psg, 0, noise_bit, PSG_PCM_MIX_CH))
                   : 0;
    }
    if (mm == STEM_NOISE_B_MASK) {
        return noise_stem_output_allowed(player, 1)
                   ? clamp_pcm(channel_noise_bipolar_pcm(psg, 1, noise_bit, PSG_PCM_MIX_CH))
                   : 0;
    }
    if (mm == STEM_NOISE_C_MASK) {
        return noise_stem_output_allowed(player, 2)
                   ? clamp_pcm(channel_noise_bipolar_pcm(psg, 2, noise_bit, PSG_PCM_MIX_CH))
                   : 0;
    }
    return mixed;
}

static void sync_psg_mixer_mute(VgmPlayer *player)
{
    uint8_t r7;

    if (!player || !player->psg || player->mute_mask == 0) {
        return;
    }
    r7 = PSG_readReg(player->psg, AY_REG_MIXER);
    PSG_writeReg(player->psg, AY_REG_MIXER, apply_mixer_mute(r7, player->mute_mask));
}

static int parse_wait(const uint8_t *stream, size_t len, size_t *pos, uint32_t *wait_out)
{
    if (*pos >= len) {
        return -1;
    }

    uint8_t cmd = stream[*pos];
    if (cmd >= 0x70 && cmd <= 0x7f) {
        *wait_out = (uint32_t)(cmd - 0x70 + 1);
        *pos += 1;
        return 0;
    }
    if (cmd == 0x61) {
        if (*pos + 3 > len) {
            return -1;
        }
        *wait_out = (uint32_t)stream[*pos + 1] | ((uint32_t)stream[*pos + 2] << 8);
        *pos += 3;
        return 0;
    }
    if (cmd == 0x62) {
        *wait_out = 735;
        *pos += 1;
        return 0;
    }
    if (cmd == 0x63) {
        *wait_out = 882;
        *pos += 1;
        return 0;
    }
    return -1;
}

static int write_ay_reg(VgmPlayer *player, uint8_t reg, uint8_t val);

static int parse_command(VgmPlayer *player, const uint8_t *stream, size_t len, size_t *pos,
                         int *ended, uint8_t *reg_out, uint8_t *val_out, int *is_ay, int *rec_out,
                         int *is_scc_out, uint8_t *scc_port, uint8_t *scc_aa, uint8_t *scc_dd)
{
    *is_ay = 0;
    if (is_scc_out) {
        *is_scc_out = 0;
    }
    if (rec_out) {
        *rec_out = 0;
    }
    if (*pos >= len) {
        *ended = 1;
        return 0;
    }

    uint8_t cmd = stream[*pos];
    switch (cmd) {
    case 0x66:
        *pos += 1;
        *ended = 1;
        return 0;
    case 0xA0:
        if (*pos + 3 > len) {
            return -1;
        }
        /* libvgm Cmd_DReg8_Data8: reg in low 7 bits, bit 7 = second chip */
        *reg_out = (uint8_t)(stream[*pos + 1] & 0x7fu);
        *val_out = stream[*pos + 2];
        if (*reg_out <= 15 && (stream[*pos + 1] & 0x80u) == 0) {
            int wr = write_ay_reg(player, *reg_out, *val_out);
            if (wr < 0) {
                return -1;
            }
            *is_ay = 1;
            if (rec_out) {
                *rec_out = wr;
            }
        }
        *pos += 3;
        return 0;
    case 0xD2:
        if (*pos + 4 > len) {
            return -1;
        }
        if (is_scc_out) {
            *is_scc_out = player->scc ? 1 : 0;
        }
        if (scc_port) {
            *scc_port = stream[*pos + 1];
        }
        if (scc_aa) {
            *scc_aa = stream[*pos + 2];
        }
        if (scc_dd) {
            *scc_dd = stream[*pos + 3];
        }
        if (player->scc) {
            vgm_scc_write_d2(player->scc, player->scc_plus, stream[*pos + 1], stream[*pos + 2],
                             stream[*pos + 3]);
        }
        *pos += 4;
        return 0;
    case 0x67: {
        if (*pos + 8 > len || stream[*pos + 1] != 0x66) {
            return -1;
        }
        uint32_t block_size = (uint32_t)stream[*pos + 3] | ((uint32_t)stream[*pos + 4] << 8) |
                              ((uint32_t)stream[*pos + 5] << 16) | ((uint32_t)stream[*pos + 6] << 24);
        *pos += 7 + block_size;
        return 0;
    }
    default: {
        size_t skip_pos = *pos;
        int cmd_end = vgm_cmd_advance(stream, len, &skip_pos);
        if (cmd_end < 0) {
            return -1;
        }
        *pos = skip_pos;
        if (cmd_end > 0) {
            *ended = 1;
        }
        return 0;
    }
    }
}

typedef struct StreamRunOpts {
    uint64_t range_start;
    uint64_t range_end;
    int16_t *out_buf;
    size_t out_cap;
    size_t *out_count;
    int record;
    uint32_t rec_pending_wait;
    VgmRecCmd **rec_cmds;
    size_t *rec_count;
    size_t *rec_cap;
} StreamRunOpts;

static int rec_grow(VgmRecCmd **cmds, size_t *cap, size_t count)
{
    if (count < *cap) {
        return 0;
    }
    size_t new_cap = (*cap == 0) ? 256u : (*cap * 2u);
    VgmRecCmd *next = (VgmRecCmd *)realloc(*cmds, new_cap * sizeof(VgmRecCmd));
    if (!next) {
        return -1;
    }
    *cmds = next;
    *cap = new_cap;
    return 0;
}

static int rec_emit(StreamRunOpts *opts, uint32_t pending, uint8_t kind, uint8_t b0, uint8_t b1,
                    uint8_t b2)
{
    if (rec_grow(opts->rec_cmds, opts->rec_cap, *opts->rec_count) != 0) {
        return -1;
    }
    VgmRecCmd *entry = &(*opts->rec_cmds)[*opts->rec_count];
    entry->wait_44100 = pending;
    entry->kind = kind;
    entry->b0 = b0;
    entry->b1 = b1;
    entry->b2 = b2;
    (*opts->rec_count)++;
    return 0;
}

static int rec_emit_ay(StreamRunOpts *opts, uint32_t pending, uint8_t reg, uint8_t val)
{
    return rec_emit(opts, pending, VGM_REC_AY, reg, val, 0);
}

/* Omit register writes for export when that channel path is fully muted. */
static int reg_skip_for_export(uint8_t reg, uint8_t mute_mask)
{
    switch (reg) {
    case 0:
    case 1:
        return (mute_mask & VGMPSG_MUTE_TONE_A) != 0;
    case 2:
    case 3:
        return (mute_mask & VGMPSG_MUTE_TONE_B) != 0;
    case 4:
    case 5:
        return (mute_mask & VGMPSG_MUTE_TONE_C) != 0;
    case 6:
        return (mute_mask & (VGMPSG_MUTE_NOISE_A | VGMPSG_MUTE_NOISE_B | VGMPSG_MUTE_NOISE_C)) ==
               (VGMPSG_MUTE_NOISE_A | VGMPSG_MUTE_NOISE_B | VGMPSG_MUTE_NOISE_C);
    case 8:
        return (mute_mask & (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_NOISE_A)) ==
               (VGMPSG_MUTE_TONE_A | VGMPSG_MUTE_NOISE_A);
    case 9:
        return (mute_mask & (VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_NOISE_B)) ==
               (VGMPSG_MUTE_TONE_B | VGMPSG_MUTE_NOISE_B);
    case 10:
        return (mute_mask & (VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_C)) ==
               (VGMPSG_MUTE_TONE_C | VGMPSG_MUTE_NOISE_C);
    case 7:
        return 0;
    case 11:
    case 12:
    case 13:
        return mute_mask == VGMPSG_MUTE_ALL;
    default:
        return 0;
    }
}

static int period_smooth_commit_channel(VgmPlayer *player, int ch, StreamRunOpts *opts)
{
    uint8_t fine = (uint8_t)(ch * 2);
    uint8_t coarse = (uint8_t)(ch * 2 + 1);

    vgm_period_smooth_apply_channel(&player->period_smooth, player->psg, ch);

    if (!opts || !opts->record) {
        return 0;
    }
    if (player->out_pos < opts->range_start || player->out_pos >= opts->range_end) {
        return 0;
    }

    if (!reg_skip_for_export(fine, player->mute_mask)) {
        if (rec_emit_ay(opts, opts->rec_pending_wait, fine, player->period_smooth.shadow[fine]) != 0) {
            return -1;
        }
        opts->rec_pending_wait = 0;
    }
    if (!reg_skip_for_export(coarse, player->mute_mask)) {
        if (rec_emit_ay(opts, 0, coarse, player->period_smooth.shadow[coarse]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int period_smooth_flush_mask(VgmPlayer *player, int mask, StreamRunOpts *opts)
{
    for (int ch = 0; ch < 3; ch++) {
        if (mask & (1 << ch)) {
            if (period_smooth_commit_channel(player, ch, opts) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int period_smooth_on_wait_boundary(VgmPlayer *player, StreamRunOpts *opts)
{
    if (!player->reduce_clicks) {
        return 0;
    }
    int mask = vgm_period_smooth_on_wait(&player->period_smooth);
    return period_smooth_flush_mask(player, mask, opts);
}

static int period_smooth_flush_all(VgmPlayer *player, StreamRunOpts *opts)
{
    if (!player->reduce_clicks) {
        return 0;
    }
    int mask = vgm_period_smooth_pending_mask(&player->period_smooth);
    return period_smooth_flush_mask(player, mask, opts);
}

static int write_ay_reg(VgmPlayer *player, uint8_t reg, uint8_t val)
{
    StreamRunOpts *opts = (StreamRunOpts *)player->active_rec_opts;

    if (reg == AY_REG_MIXER) {
        if (player->reduce_clicks) {
            if (period_smooth_flush_all(player, opts) != 0) {
                return -1;
            }
        }
        player->game_r7 = val;
        player->game_r7_valid = 1;
        val = apply_mixer_mute(val, player->mute_mask);
        PSG_writeReg(player->psg, reg, val);
        return 1;
    }

    if (player->reduce_clicks && reg <= 5) {
        int ch = 0;
        int commit_now = vgm_period_smooth_write(&player->period_smooth, reg, val, &ch);
        if (commit_now) {
            if (period_smooth_commit_channel(player, ch, opts) != 0) {
                return -1;
            }
            return 0;
        }
        return 0;
    }

    if (player->reduce_clicks) {
        if (period_smooth_flush_all(player, opts) != 0) {
            return -1;
        }
    }

    PSG_writeReg(player->psg, reg, val);
    return 1;
}

static void rec_add_wait_in_range(StreamRunOpts *opts, uint64_t seg_start, uint64_t seg_end,
                                  uint32_t *pending)
{
    if (!opts->record || seg_end <= seg_start) {
        return;
    }
    if (seg_end <= opts->range_start || seg_start >= opts->range_end) {
        return;
    }
    uint64_t clip_a = seg_start < opts->range_start ? opts->range_start : seg_start;
    uint64_t clip_b = seg_end > opts->range_end ? opts->range_end : seg_end;
    if (clip_b > clip_a) {
        *pending += output_samples_to_vgm_wait((uint32_t)(clip_b - clip_a));
    }
}

static int resolve_range(const VgmFile *file, const VgmTimeRange *range, uint64_t *start_out,
                         uint64_t *end_out);

static void stream_reset(VgmPlayer *player)
{
    const uint8_t *data = player->file->data;
    size_t pos = player->file->cmd_start;

    while (pos < player->file->cmd_end && data[pos] == 0) {
        pos++;
    }

    PSG_reset(player->psg);
    sync_psg_mixer_mute(player);
    if (player->scc) {
        SCC_reset(player->scc);
        vgm_scc_activate(player->scc);
        vgm_scc_apply_mute_mask(player->scc, player->scc_mute_mask);
    }
    vgm_period_smooth_reset(&player->period_smooth);
    vgm_period_smooth_sync_from_psg(&player->period_smooth, player->psg);
    player->game_r7 = 0;
    player->game_r7_valid = 0;
    for (int i = 0; i < 3; i++) {
        player->tone_hit_bleed[i] = 0;
    }
    player->stream_pos = pos;
    player->stream_cmd_end = player->file->cmd_end;
    player->file_tick = 0;
    player->play_tick = 0;
    player->out_pos = 0;
    player->stream_ended = 0;
    player->cur_loop = 0;
    player->last_loop_out_pos = 0;
}

static int stream_try_loop(VgmPlayer *player)
{
    const VgmFile *file = player->file;

    if (!file->info.has_loop || file->info.loop_offset == 0) {
        return 0;
    }
    if (player->out_pos == player->last_loop_out_pos) {
        return 0;
    }
    if (player->loop_pos < file->cmd_start || player->loop_pos >= player->stream_cmd_end) {
        return 0;
    }

    player->stream_pos = player->loop_pos;
    player->cur_loop++;
    player->last_loop_out_pos = (uint32_t)player->out_pos;
    return 1;
}

/* libvgm VGMPlayer::ParseFile — commands only, no audio until next Render step. */
static int process_file_until_tick(VgmPlayer *player, uint32_t target_play_tick, StreamRunOpts *opts)
{
    const uint8_t *data = player->file->data;
    size_t end = player->stream_cmd_end;
    size_t *pos = &player->stream_pos;

    player->active_rec_opts = opts;

    while (*pos < end && player->file_tick <= target_play_tick && !player->stream_ended) {
        uint8_t cmd = data[*pos];

        if (cmd >= 0x70 && cmd <= 0x7f) {
            player->file_tick += (uint32_t)(cmd - 0x70 + 1);
            (*pos)++;
            if (period_smooth_on_wait_boundary(player, opts) != 0) {
                return -1;
            }
            continue;
        }
        if (cmd == 0x61 || cmd == 0x62 || cmd == 0x63) {
            uint32_t w = 0;
            if (parse_wait(data, end, pos, &w) != 0) {
                return -1;
            }
            player->file_tick += w;
            if (period_smooth_on_wait_boundary(player, opts) != 0) {
                return -1;
            }
            continue;
        }
        if (cmd == 0x66) {
            (*pos)++;
            if (stream_try_loop(player)) {
                continue;
            }
            player->stream_ended = 1;
            return 0;
        }

        uint8_t reg = 0, val = 0;
        int is_ay = 0, is_scc = 0, ended = 0, rec = 0;
        uint8_t scc_port = 0, scc_aa = 0, scc_dd = 0;
        if (parse_command(player, data, end, pos, &ended, &reg, &val, &is_ay, &rec, &is_scc, &scc_port,
                          &scc_aa, &scc_dd) != 0) {
            return -1;
        }
        if (ended) {
            if (stream_try_loop(player)) {
                continue;
            }
            player->stream_ended = 1;
            return 0;
        }
        if (opts && opts->record && player->out_pos >= opts->range_start &&
            player->out_pos < opts->range_end) {
            if (is_ay && rec == 1) {
                if (!reg_skip_for_export(reg, player->mute_mask)) {
                    uint8_t rec_val = val;
                    if (reg == AY_REG_MIXER) {
                        rec_val = apply_mixer_mute(val, player->mute_mask);
                    }
                    if (rec_emit_ay(opts, opts->rec_pending_wait, reg, rec_val) != 0) {
                        return -1;
                    }
                    opts->rec_pending_wait = 0;
                }
            } else if (is_scc &&
                       !vgm_scc_d2_skip_for_export(scc_port, scc_aa, player->scc_mute_mask,
                                                     player->scc_plus)) {
                uint8_t rec_dd = scc_dd;
                if ((scc_port & 0x7fu) == 0x03u && scc_aa == 0) {
                    rec_dd = vgm_scc_apply_mask_mute(scc_dd, player->scc_mute_mask);
                }
                if (rec_emit(opts, opts->rec_pending_wait, VGM_REC_SCC, scc_port, scc_aa, rec_dd) !=
                    0) {
                    return -1;
                }
                opts->rec_pending_wait = 0;
            }
        }
    }
    return 0;
}

/* libvgm VGMPlayer::Render — advance timeline, batch commands, then emit smplStep samples. */
static int stream_render_chunk(VgmPlayer *player, int16_t *buf, size_t max_samples, size_t *out_count,
                               StreamRunOpts *opts)
{
    player->active_rec_opts = opts;
    *out_count = 0;

    while (*out_count < max_samples) {
        if (player->stream_ended || player->out_pos >= player->range_end) {
            if (period_smooth_flush_all(player, opts) != 0) {
                return -1;
            }
            return 0;
        }

        uint32_t max_out_smpl = tick_to_output_samples(player->file_tick);
        int64_t smpl_step = (int64_t)max_out_smpl - (int64_t)player->out_pos;
        if (smpl_step < 1) {
            smpl_step = 1;
        }
        if ((size_t)smpl_step > max_samples - *out_count) {
            smpl_step = (int64_t)(max_samples - *out_count);
        }
        if ((uint64_t)smpl_step > player->range_end - player->out_pos) {
            smpl_step = (int64_t)(player->range_end - player->out_pos);
        }

        uint64_t seg_start = player->out_pos;
        for (int64_t i = 0; i < smpl_step; i++) {
            uint32_t sample_tick = output_samples_to_vgm_tick(player->out_pos);

            if (sample_tick > player->play_tick) {
                player->play_tick = sample_tick;
                if (process_file_until_tick(player, player->play_tick, opts) != 0) {
                    return -1;
                }
                if (player->stream_ended) {
                    break;
                }
            }

            int in_range =
                (player->out_pos >= player->range_start && player->out_pos < player->range_end);
            int16_t sample = calc_playback_sample(player);
            if (in_range && buf) {
                buf[(*out_count)++] = sample;
            }
            player->out_pos++;
        }

        if (opts && opts->record) {
            rec_add_wait_in_range(opts, seg_start, player->out_pos, &opts->rec_pending_wait);
        }
    }

    return 0;
}

void vgm_player_stream_begin(VgmPlayer *player, const VgmTimeRange *range)
{
    if (!player || !range) {
        return;
    }

    uint64_t start_out = 0, end_out = 0;
    if (resolve_range(player->file, range, &start_out, &end_out) != 0) {
        player->stream_active = 0;
        return;
    }

    stream_reset(player);
    player->range_start = start_out;
    player->range_end = end_out;
    player->stream_active = 1;
}

int vgm_player_stream_read(VgmPlayer *player, int16_t *buf, size_t max_samples, size_t *out_count)
{
    if (!player || !buf || !out_count || !player->stream_active) {
        return -1;
    }

    if (stream_render_chunk(player, buf, max_samples, out_count, NULL) != 0) {
        player->stream_active = 0;
        return -1;
    }

    if (player->stream_ended || player->out_pos >= player->range_end) {
        player->stream_active = 0;
    }

    return 0;
}

int vgm_player_stream_done(const VgmPlayer *player)
{
    return !player || !player->stream_active;
}

void vgm_player_stream_end(VgmPlayer *player)
{
    if (player) {
        player->stream_active = 0;
        player->stream_ended = 1;
    }
}

static int run_stream(VgmPlayer *player, StreamRunOpts *opts)
{
    stream_reset(player);
    player->range_start = opts->range_start;
    player->range_end = opts->range_end;
    opts->rec_pending_wait = 0;

    while (player->out_pos < opts->range_end && !player->stream_ended) {
        size_t chunk = 0;
        size_t cap_left = opts->out_buf ? opts->out_cap - *opts->out_count : 4096u;
        int16_t *dst = opts->out_buf ? opts->out_buf + *opts->out_count : NULL;
        if (stream_render_chunk(player, dst, cap_left, &chunk, opts) != 0) {
            player->active_rec_opts = NULL;
            return -1;
        }
        *opts->out_count += chunk;
        if (chunk == 0) {
            break;
        }
    }

    if (period_smooth_flush_all(player, opts) != 0) {
        player->active_rec_opts = NULL;
        return -1;
    }
    player->active_rec_opts = NULL;

    return 0;
}

static int resolve_range(const VgmFile *file, const VgmTimeRange *range, uint64_t *start_out,
                         uint64_t *end_out)
{
    double duration = vgm_file_duration_sec(file);
    double start = range->start_sec;
    double end = range->end_sec;

    if (end <= start || end <= 0.0) {
        end = duration;
    }
    if (start < 0.0) {
        start = 0.0;
    }
    if (start >= duration) {
        return -1;
    }
    if (end > duration) {
        end = duration;
    }
    if (end <= start) {
        return -1;
    }

    *start_out = (uint64_t)(start * (double)VGMPSG_OUTPUT_RATE + 0.5);
    *end_out = (uint64_t)(end * (double)VGMPSG_OUTPUT_RATE + 0.5);
    if (*end_out <= *start_out) {
        return -1;
    }
    return 0;
}

static void fill_channel_levels_from_psg(const VgmPlayer *player, PSG *psg, uint8_t out[20])
{
    static const uint8_t tone_off[] = {0x01u, 0x02u, 0x04u};
    static const uint8_t noise_off[] = {0x08u, 0x10u, 0x20u};
    uint8_t r7 = psg->reg[7];

    (void)player;

    for (int ch = 0; ch < 3; ch++) {
        uint8_t rvol = psg->reg[(size_t)(8 + ch)];
        uint8_t level;
        uint16_t tone_period;
        if (rvol & 0x10u) {
            level = (uint8_t)(psg->env_ptr >> 1);
        } else {
            level = (uint8_t)(rvol & 0x0fu);
        }
        out[(size_t)(ch * 2)] = (r7 & tone_off[ch]) ? 0 : level;
        out[(size_t)(ch * 2 + 1)] = (r7 & noise_off[ch]) ? 0 : level;
        out[6u + (size_t)ch] = (rvol & 0x10u) ? 1u : 0u;
        tone_period = (uint16_t)(psg->reg[(size_t)(ch * 2)] | ((psg->reg[(size_t)(ch * 2 + 1)] & 0x0fu) << 8));
        out[11u + (size_t)(ch * 3)] = (uint8_t)(tone_period & 0xffu);
        out[12u + (size_t)(ch * 3)] = (uint8_t)((tone_period >> 8) & 0x0fu);
        out[13u + (size_t)(ch * 3)] = (r7 & tone_off[ch]) ? 0u : 1u;
    }
    out[9] = (uint8_t)(psg->reg[13] & 0x0fu);
    out[10] = psg->reg[6];
}

static int advance_player_to_output(VgmPlayer *player, uint64_t target_out)
{
    size_t dummy = 0;
    StreamRunOpts opts = {
        .range_start = 0,
        .range_end = target_out > 0 ? target_out : 1,
        .out_buf = NULL,
        .out_cap = 0,
        .out_count = &dummy,
        .record = 0,
    };

    if (target_out == 0) {
        return 0;
    }

    player->range_end = target_out;
    while (player->out_pos < target_out && !player->stream_ended) {
        size_t chunk = 0;
        if (stream_render_chunk(player, NULL, 4096u, &chunk, &opts) != 0) {
            return -1;
        }
        if (chunk == 0) {
            break;
        }
    }
    return 0;
}

static int seek_player_to_time(VgmPlayer *player, uint64_t target_out)
{
    if (target_out == 0) {
        stream_reset(player);
        player->range_start = 0;
        player->range_end = 1;
        return 0;
    }

    if (target_out >= player->out_pos && !player->stream_ended) {
        return advance_player_to_output(player, target_out);
    }

    size_t dummy = 0;
    StreamRunOpts opts = {
        .range_start = 0,
        .range_end = target_out,
        .out_buf = NULL,
        .out_cap = 0,
        .out_count = &dummy,
        .record = 0,
    };

    stream_reset(player);
    player->range_start = 0;
    player->range_end = opts.range_end;

    while (player->out_pos < opts.range_end && !player->stream_ended) {
        size_t chunk = 0;
        if (stream_render_chunk(player, NULL, 4096u, &chunk, &opts) != 0) {
            return -1;
        }
        if (chunk == 0) {
            break;
        }
    }
    return 0;
}

int vgm_player_snapshot_ay_at_output(VgmPlayer *player, uint64_t target_out, uint8_t out[14])
{
    if (!player || !player->psg || !out) {
        return -1;
    }

    if (seek_player_to_time(player, target_out) != 0) {
        return -1;
    }

    for (int i = 0; i < 14; i++) {
        out[i] = PSG_readReg(player->psg, (uint8_t)i);
    }
    out[7] = apply_mixer_mute(out[7], player->mute_mask);
    return 0;
}

int vgm_player_channel_levels_at(VgmPlayer *player, double time_sec, uint8_t out[20])
{
    uint64_t target_out = 0;

    if (!player || !player->file || !out) {
        return -1;
    }

    memset(out, 0, 20);
    {
        double duration = vgm_file_duration_sec(player->file);
        if (time_sec < 0.0) {
            time_sec = 0.0;
        }
        if (time_sec > duration) {
            time_sec = duration;
        }
    }

    target_out = (uint64_t)(time_sec * (double)VGMPSG_OUTPUT_RATE + 0.5);
    {
        uint8_t saved_mute = player->mute_mask;
        player->mute_mask = 0;
        if (seek_player_to_time(player, target_out) != 0) {
            player->mute_mask = saved_mute;
            return -1;
        }
        PSG_calc(player->psg);
        fill_channel_levels_from_psg(player, player->psg, out);
        player->mute_mask = saved_mute;
    }
    return 0;
}

int vgm_player_scan_channel_levels(VgmPlayer *player, double end_sec, double step_sec,
                                   uint8_t *out_buf, size_t sample_stride, size_t max_samples,
                                   size_t *out_count)
{
    uint64_t end_out;
    uint64_t step_out;
    uint64_t next_out;
    size_t count = 0;
    uint8_t saved_mute;

    if (!player || !player->file || !out_buf || !out_count || step_sec <= 0.0 || sample_stride < 20u ||
        max_samples == 0) {
        return -1;
    }

    *out_count = 0;
    {
        double duration = vgm_file_duration_sec(player->file);
        if (end_sec < 0.0) {
            end_sec = 0.0;
        }
        if (end_sec > duration) {
            end_sec = duration;
        }
    }

    end_out = (uint64_t)(end_sec * (double)VGMPSG_OUTPUT_RATE + 0.5);
    step_out = (uint64_t)(step_sec * (double)VGMPSG_OUTPUT_RATE + 0.5);
    if (step_out < 1u) {
        step_out = 1u;
    }

    saved_mute = player->mute_mask;
    player->mute_mask = 0;

    stream_reset(player);
    player->range_start = 0;
    player->range_end = end_out > 0 ? end_out : 1u;
    next_out = 0;

    while (next_out <= end_out && count < max_samples) {
        if (next_out > 0u) {
            if (advance_player_to_output(player, next_out) != 0) {
                player->mute_mask = saved_mute;
                return -1;
            }
            if (player->out_pos < next_out) {
                break;
            }
        }

        PSG_calc(player->psg);
        fill_channel_levels_from_psg(player, player->psg, out_buf + count * sample_stride);
        count++;
        next_out += step_out;
    }

    player->mute_mask = saved_mute;
    *out_count = count;
    return 0;
}

VgmPlayer *vgm_player_create(const VgmFile *file)
{
    if (!file) {
        return NULL;
    }

    VgmPlayer *player = (VgmPlayer *)calloc(1, sizeof(VgmPlayer));
    if (!player) {
        return NULL;
    }

    player->file = file;
    uint32_t clk = file->info.ay8910_clock_hz ? file->info.ay8910_clock_hz : VGMPSG_AY_CLOCK_HZ;
    player->psg = PSG_new(clk, VGMPSG_OUTPUT_RATE);
    if (!player->psg) {
        free(player);
        return NULL;
    }

    PSG_setVolumeMode(player->psg, 2);
    PSG_setQuality(player->psg, 1);
    PSG_reset(player->psg);

    if (file->info.loop_offset > 0) {
        player->loop_pos = 0x34u + file->info.loop_offset;
        if (player->loop_pos >= file->size) {
            player->loop_pos = file->cmd_start;
        }
    } else {
        player->loop_pos = file->cmd_start;
    }

    player->scc_plus = file->scc_plus;
    if (file->scc_write_count > 0 || file->scc_clock_hz > 0) {
        uint32_t scc_clk = file->scc_clock_hz
                               ? file->scc_clock_hz
                               : (uint32_t)VGM_SCC_DEFAULT_CLOCK_HZ;
        player->scc = SCC_new(scc_clk, VGMPSG_OUTPUT_RATE);
        if (player->scc) {
            SCC_set_type(player->scc, player->scc_plus ? SCC_ENHANCED : SCC_STANDARD);
            SCC_set_quality(player->scc, 0);
            vgm_scc_activate(player->scc);
        }
    }

    return player;
}

void vgm_player_free(VgmPlayer *player)
{
    if (!player) {
        return;
    }
    PSG_delete(player->psg);
    SCC_delete(player->scc);
    free(player);
}

void vgm_player_set_mute_mask(VgmPlayer *player, uint8_t mute_mask)
{
    uint8_t base;

    if (!player) {
        return;
    }
    player->mute_mask = (uint8_t)(mute_mask & VGMPSG_MUTE_ALL);
    if (!player->psg) {
        return;
    }
    /* Reapply from the song mixer. Reading the live register keeps bits a previous mask forced on. */
    if (player->game_r7_valid) {
        base = player->game_r7;
    } else {
        base = (uint8_t)(AY_R7_NOISE_A_OFF | AY_R7_NOISE_B_OFF | AY_R7_NOISE_C_OFF);
    }
    PSG_writeReg(player->psg, AY_REG_MIXER, apply_mixer_mute(base, player->mute_mask));
}

uint8_t vgm_player_get_mute_mask(const VgmPlayer *player)
{
    return player ? player->mute_mask : 0;
}

void vgm_player_set_scc_mute_mask(VgmPlayer *player, uint8_t mute_mask)
{
    if (player) {
        player->scc_mute_mask = (uint8_t)(mute_mask & 0x1fu);
        vgm_scc_apply_mute_mask(player->scc, player->scc_mute_mask);
    }
}

uint8_t vgm_player_get_scc_mute_mask(const VgmPlayer *player)
{
    return player ? player->scc_mute_mask : 0;
}

void vgm_player_set_reduce_clicks(VgmPlayer *player, int enabled)
{
    if (player) {
        player->reduce_clicks = enabled ? 1 : 0;
        if (player->reduce_clicks) {
            vgm_period_smooth_sync_from_psg(&player->period_smooth, player->psg);
        } else {
            vgm_period_smooth_reset(&player->period_smooth);
        }
    }
}

int vgm_player_get_reduce_clicks(const VgmPlayer *player)
{
    return player ? player->reduce_clicks : 0;
}

void vgm_player_set_exclusive_solo(VgmPlayer *player, int enabled)
{
    if (player) {
        player->exclusive_solo = enabled ? 1 : 0;
    }
}

int vgm_player_get_exclusive_solo(const VgmPlayer *player)
{
    return player ? player->exclusive_solo : 0;
}

void vgm_time_range_full(VgmTimeRange *range, double duration_sec)
{
    if (range) {
        range->start_sec = 0.0;
        range->end_sec = duration_sec;
    }
}

int vgm_time_range_valid(const VgmTimeRange *range, double duration_sec)
{
    if (!range) {
        return 0;
    }
    double end = (range->end_sec <= range->start_sec) ? duration_sec : range->end_sec;
    return range->start_sec >= 0.0 && range->start_sec < duration_sec && end > range->start_sec;
}

int vgm_player_render(VgmPlayer *player, int16_t **out_samples, size_t *out_count)
{
    double dur = vgm_file_duration_sec(player->file);
    VgmTimeRange full = {0.0, dur};
    return vgm_player_render_range(player, &full, out_samples, out_count);
}

int vgm_player_render_range(VgmPlayer *player, const VgmTimeRange *range, int16_t **out_samples,
                            size_t *out_count)
{
    uint64_t start_out = 0, end_out = 0;
    size_t need = 0;
    size_t produced = 0;
    int16_t *buf = NULL;
    size_t chunk = 0;

    if (!player || !range || !out_samples || !out_count) {
        return -1;
    }

    if (resolve_range(player->file, range, &start_out, &end_out) != 0) {
        return -1;
    }

    need = (size_t)(end_out - start_out);
    buf = (int16_t *)calloc(need, sizeof(int16_t));
    if (!buf) {
        return -1;
    }

    vgm_player_stream_begin(player, range);
    while (produced < need && player->stream_active) {
        if (vgm_player_stream_read(player, buf + produced, need - produced, &chunk) != 0) {
            free(buf);
            return -1;
        }
        if (chunk == 0) {
            break;
        }
        produced += chunk;
    }

    *out_samples = buf;
    *out_count = produced;
    return 0;
}

int vgm_player_record_range(VgmPlayer *player, const VgmTimeRange *range, VgmRecCmd **cmds,
                            size_t *rec_count)
{
    if (!player || !range || !cmds || !rec_count) {
        return -1;
    }

    uint64_t start_out = 0, end_out = 0;
    if (resolve_range(player->file, range, &start_out, &end_out) != 0) {
        return -1;
    }

    *cmds = NULL;
    *rec_count = 0;
    size_t cap = 0;

    size_t dummy = 0;
    StreamRunOpts opts = {
        .range_start = start_out,
        .range_end = end_out,
        .out_buf = NULL,
        .out_cap = 0,
        .out_count = &dummy,
        .record = 1,
        .rec_cmds = cmds,
        .rec_count = rec_count,
        .rec_cap = &cap,
    };

    if (run_stream(player, &opts) != 0) {
        free(*cmds);
        *cmds = NULL;
        *rec_count = 0;
        return -1;
    }

    if (*rec_count == 0) {
        free(*cmds);
        *cmds = NULL;
        return -1;
    }

    return 0;
}

int vgm_player_seek_output(VgmPlayer *player, uint64_t target_out)
{
    if (!player) {
        return -1;
    }
    return seek_player_to_time(player, target_out);
}

void vgm_player_arm_stream(VgmPlayer *player, uint64_t range_end)
{
    if (!player) {
        return;
    }
    if (range_end <= player->out_pos) {
        player->stream_active = 0;
        player->stream_ended = 1;
        return;
    }
    player->range_start = player->out_pos;
    player->range_end = range_end;
    player->stream_active = 1;
    player->stream_ended = 0;
}

uint64_t vgm_player_out_pos(const VgmPlayer *player)
{
    return player ? player->out_pos : 0;
}

int vgm_player_scc_channel_state(const VgmPlayer *player, int ch, uint16_t *period_out,
                                 uint8_t *volume_out, int *enabled_out)
{
    const SCC *scc;
    if (!player || !player->scc || ch < 0 || ch > 4) {
        return -1;
    }
    scc = player->scc;
    if (period_out) {
        *period_out = (uint16_t)(scc->freq[ch] & 0x0fffu);
    }
    if (volume_out) {
        *volume_out = (uint8_t)(scc->volume[ch] & 0x0fu);
    }
    if (enabled_out) {
        *enabled_out = ((scc->ch_enable & scc->ch_enable_next) & (1 << ch)) != 0 ? 1 : 0;
    }
    return 0;
}

int vgm_player_scc_channel_tap(const VgmPlayer *player, int ch, uint16_t *period_out,
                               uint8_t *volume_out, int *enabled_out)
{
    const SCC *scc;
    int keyed;
    int masked;

    if (!player || !player->scc || ch < 0 || ch > 4) {
        return -1;
    }
    scc = player->scc;
    if (period_out) {
        *period_out = (uint16_t)(scc->freq[ch] & 0x0fffu);
    }
    if (volume_out) {
        *volume_out = (uint8_t)(scc->volume[ch] & 0x0fu);
    }
    if (enabled_out) {
        keyed = (scc->ch_enable & (1 << ch)) != 0;
        masked = (scc->mask & SCC_MASK_CH(ch)) != 0;
        *enabled_out = (keyed && !masked) ? 1 : 0;
    }
    return 0;
}

uint32_t vgm_player_scc_clock_hz(const VgmPlayer *player)
{
    if (!player || !player->scc) {
        return (uint32_t)VGM_SCC_DEFAULT_CLOCK_HZ;
    }
    return player->scc->clk ? player->scc->clk : (uint32_t)VGM_SCC_DEFAULT_CLOCK_HZ;
}

uint32_t vgm_player_ay_clock_hz(const VgmPlayer *player)
{
    if (!player || !player->file || player->file->info.ay8910_clock_hz == 0) {
        return (uint32_t)VGMPSG_AY_CLOCK_HZ;
    }
    return player->file->info.ay8910_clock_hz;
}

int vgm_player_psg_global_regs(const VgmPlayer *player, uint8_t *mixer6, uint16_t *env_period,
                               uint8_t *env_shape)
{
    const PSG *psg;

    if (!player || !player->psg) {
        return -1;
    }
    psg = player->psg;
    /* game_r7 is the song write. The live mixer register also carries solo mute. */
    if (mixer6) {
        *mixer6 = (uint8_t)(effective_game_r7(player) & 0x3fu);
    }
    if (env_period) {
        *env_period = (uint16_t)(((uint16_t)psg->reg[12] << 8) | psg->reg[11]);
    }
    if (env_shape) {
        *env_shape = (uint8_t)(psg->reg[13] & 0x0fu);
    }
    return 0;
}

int vgm_player_psg_channel_tap(const VgmPlayer *player, int ch, uint16_t *period_out,
                               int *tone_enabled_out, int *noise_enabled_out,
                               uint8_t *noise_period_out, uint8_t *volume_out, int *envelope_out,
                               float *gain_out)
{
    const PSG *psg;
    uint8_t r7;
    uint8_t stored;
    uint8_t vol_idx;
    int env;

    if (!player || !player->psg || ch < 0 || ch > 2) {
        return -1;
    }
    psg = player->psg;
    r7 = effective_game_r7(player);
    stored = psg->volume[ch];
    env = (stored & 32u) != 0;
    if (period_out) {
        *period_out = (uint16_t)(psg->freq[ch] & 0x0fffu);
    }
    if (tone_enabled_out) {
        *tone_enabled_out = game_tone_enabled(r7, ch);
    }
    if (noise_enabled_out) {
        *noise_enabled_out = game_noise_enabled(r7, ch);
    }
    if (noise_period_out) {
        *noise_period_out = (uint8_t)(psg->noise_freq & 31u);
    }
    if (volume_out) {
        *volume_out = (uint8_t)((stored >> 1) & 15u);
    }
    if (envelope_out) {
        *envelope_out = env;
    }
    if (gain_out) {
        if (!psg->voltbl) {
            *gain_out = 0.f;
        } else {
            vol_idx = env ? psg->env_ptr : (uint8_t)(stored & 31u);
            *gain_out = (float)psg->voltbl[vol_idx] / 255.f;
        }
    }
    return 0;
}

int vgm_player_psg_stem_pcm(const VgmPlayer *player, int ch, int16_t *tone_pcm, int16_t *noise_pcm)
{
    PSG *psg;
    uint8_t noise_bit;

    if (!player || !player->psg || ch < 0 || ch > 2) {
        return -1;
    }
    psg = player->psg;
    noise_bit = (uint8_t)(psg->noise_seed & 1u);
    if (tone_pcm) {
        *tone_pcm = clamp_pcm(channel_tone_bipolar_pcm(psg, ch, PSG_PCM_MIX_CH));
    }
    if (noise_pcm) {
        *noise_pcm = clamp_pcm(channel_noise_bipolar_pcm(psg, ch, noise_bit, PSG_PCM_MIX_CH));
    }
    return 0;
}
