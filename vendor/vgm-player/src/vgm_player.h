#ifndef VGM_PLAYER_H
#define VGM_PLAYER_H

#include "vgm_file.h"
#include "emu2149.h"
#include "emu2212.h"
#include "vgm_period_smooth.h"

struct VgmPlayer {
    const VgmFile *file;
    PSG *psg;
    SCC *scc;
    int scc_plus;
    uint8_t mute_mask;
    uint8_t scc_mute_mask;
    uint8_t game_r7;
    uint8_t game_r7_valid;
    size_t loop_pos;

    int reduce_clicks;
    int exclusive_solo;
    uint8_t tone_hit_bleed[3];
    VgmPeriodSmoothState period_smooth;
    void *active_rec_opts;

    /* libvgm-style timeline: commands advance file_tick (44.1 kHz), audio follows. */
    int stream_active;
    size_t stream_pos;
    size_t stream_cmd_end;
    uint32_t file_tick;
    uint32_t play_tick;
    uint64_t out_pos;
    uint64_t range_start;
    uint64_t range_end;
    int stream_ended;
    uint32_t cur_loop;
    uint32_t last_loop_out_pos;
};

#endif
