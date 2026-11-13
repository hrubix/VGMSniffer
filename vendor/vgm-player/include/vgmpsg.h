#ifndef VGMPSG_H
#define VGMPSG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R7 mixer mute bits: set = force channel tone/noise inactive */
#define VGMPSG_MUTE_TONE_A  (1u << 0)
#define VGMPSG_MUTE_TONE_B  (1u << 1)
#define VGMPSG_MUTE_TONE_C  (1u << 2)
#define VGMPSG_MUTE_NOISE_A (1u << 3)
#define VGMPSG_MUTE_NOISE_B (1u << 4)
#define VGMPSG_MUTE_NOISE_C (1u << 5)
#define VGMPSG_MUTE_ALL     0x3Fu

typedef struct VgmFile VgmFile;
typedef struct VgmPlayer VgmPlayer;

typedef struct VgmFileInfo {
    uint32_t version;
    uint32_t ay8910_clock_hz;
    uint32_t total_samples_44100;
    uint32_t loop_samples_44100;
    uint32_t loop_offset; /* bytes from end of header (0x34) */
    int has_loop;
} VgmFileInfo;

/* Time range in seconds (output timeline @ VGMPSG_OUTPUT_RATE). end <= start = full track. */
typedef struct VgmTimeRange {
    double start_sec;
    double end_sec;
} VgmTimeRange;

#define VGM_LABEL_NAME_MAX 128
#define VGM_LABELS_MAX 256

typedef struct VgmLabel {
    char name[VGM_LABEL_NAME_MAX];
    double start_sec;
    double end_sec;
} VgmLabel;

typedef struct VgmLabels {
    char source_vgm[512];
    VgmLabel labels[VGM_LABELS_MAX];
    size_t count;
} VgmLabels;

VgmFile *vgm_file_load(const char *path);
VgmFile *vgm_file_load_bytes(const uint8_t *data, size_t size, int is_vgz);
void vgm_file_free(VgmFile *file);
const uint8_t *vgm_file_data(const VgmFile *file);
size_t vgm_file_size(const VgmFile *file);
int vgm_file_info(const VgmFile *file, VgmFileInfo *out);
double vgm_file_duration_sec(const VgmFile *file);
/* True if a 0x66 appears inside command operands (naive scan would end early). */
int vgm_file_has_false_eof(const VgmFile *file);

/* Non-PSG chips present in VGM header (playback uses AY/PSG 0xA0 only). */
#define VGMPSG_CHIP_SN76489 (1u << 0)
#define VGMPSG_CHIP_YM2413  (1u << 1)
#define VGMPSG_CHIP_YM2612  (1u << 2)
#define VGMPSG_CHIP_YM2151  (1u << 3)
#define VGMPSG_CHIP_YM2203  (1u << 4)
#define VGMPSG_CHIP_YM2608  (1u << 5)
#define VGMPSG_CHIP_SCC     (1u << 6)
#define VGMPSG_CHIP_OTHER   (1u << 7)

uint32_t vgm_file_extra_chip_flags(const VgmFile *file);
uint32_t vgm_file_ay_write_count(const VgmFile *file);
uint32_t vgm_file_fm_write_count(const VgmFile *file);
uint32_t vgm_file_scc_write_count(const VgmFile *file);
uint32_t vgm_file_scc_clock_hz(const VgmFile *file);
int vgm_file_scc_plus(const VgmFile *file);
/* True when the command stream contains AY-3-8910 (0xA0) register writes. */
int vgm_file_has_psg_data(const VgmFile *file);
/* True when the command stream contains SCC (0xD2) writes or SCC clock in header. */
int vgm_file_has_scc_data(const VgmFile *file);

VgmPlayer *vgm_player_create(const VgmFile *file);
void vgm_player_free(VgmPlayer *player);

void vgm_player_set_mute_mask(VgmPlayer *player, uint8_t mute_mask);
uint8_t vgm_player_get_mute_mask(const VgmPlayer *player);

void vgm_player_set_scc_mute_mask(VgmPlayer *player, uint8_t mute_mask);
uint8_t vgm_player_get_scc_mute_mask(const VgmPlayer *player);

void vgm_player_set_reduce_clicks(VgmPlayer *player, int enabled);
int vgm_player_get_reduce_clicks(const VgmPlayer *player);

void vgm_player_set_exclusive_solo(VgmPlayer *player, int enabled);
int vgm_player_get_exclusive_solo(const VgmPlayer *player);

void vgm_time_range_full(VgmTimeRange *range, double duration_sec);
int vgm_time_range_valid(const VgmTimeRange *range, double duration_sec);

/* Render full song once (no loop). */
int vgm_player_render(VgmPlayer *player, int16_t **out_samples, size_t *out_count);

/* Render [start_sec, end_sec); fast-forwards chip state before start. */
int vgm_player_render_range(VgmPlayer *player, const VgmTimeRange *range, int16_t **out_samples,
                            size_t *out_count);

/* Streaming playback: begin once, read chunks; mute updates apply without rewind. */
void vgm_player_stream_begin(VgmPlayer *player, const VgmTimeRange *range);
int vgm_player_stream_read(VgmPlayer *player, int16_t *buf, size_t max_samples, size_t *out_count);
int vgm_player_stream_done(const VgmPlayer *player);
void vgm_player_stream_end(VgmPlayer *player);

/* Seek chip+timeline to output sample index, then arm streaming through range_end. */
int vgm_player_seek_output(VgmPlayer *player, uint64_t target_out);
void vgm_player_arm_stream(VgmPlayer *player, uint64_t range_end);
uint64_t vgm_player_out_pos(const VgmPlayer *player);

/* Selected SCC channel snapshot for UI / note tracking (period 0..4095). */
int vgm_player_scc_channel_state(const VgmPlayer *player, int ch, uint16_t *period_out,
                                 uint8_t *volume_out, int *enabled_out);
/* State after the sample just rendered. enabled = key bit and not muted.
   volume is the raw nibble (0 = silent, 15 = loudest). */
int vgm_player_scc_channel_tap(const VgmPlayer *player, int ch, uint16_t *period_out,
                               uint8_t *volume_out, int *enabled_out);
uint32_t vgm_player_scc_clock_hz(const VgmPlayer *player);

/* PSG voice after the sample just rendered. tone/noise follow the song mixer.
   volume_out is the raw 0..15 nibble (envelope mode is separate).
   gain_out is linear DAC level 0..1 (envelope or fixed), 0 when silent. */
int vgm_player_psg_channel_tap(const VgmPlayer *player, int ch, uint16_t *period_out,
                               int *tone_enabled_out, int *noise_enabled_out,
                               uint8_t *noise_period_out, uint8_t *volume_out, int *envelope_out,
                               float *gain_out);
/* Tone and noise PCM for one channel after the sample just rendered.
   Each already includes that channel's level. Mute bits still apply. */
int vgm_player_psg_stem_pcm(const VgmPlayer *player, int ch, int16_t *tone_pcm, int16_t *noise_pcm);
uint32_t vgm_player_ay_clock_hz(const VgmPlayer *player);

/* Song mixer and envelope after the sample just rendered.
   mixer6 is R7 bits 0..5 (I/O bits 6 and 7 cleared). Solo mute is not applied.
   env_period is R11 + (R12 << 8). env_shape is R13 bits 0..3.
   Returns -1 when the player has no PSG. */
int vgm_player_psg_global_regs(const VgmPlayer *player, uint8_t *mixer6, uint16_t *env_period,
                               uint8_t *env_shape);

/* out[0..5] levels; out[6..8] env mode; out[9] reg13; out[10] reg6;
   out[11..13] tone A period LE + on; out[14..16] tone B; out[17..19] tone C. */
int vgm_player_channel_levels_at(VgmPlayer *player, double time_sec, uint8_t out[20]);

/* Single forward pass: sample channel levels every step_sec up to end_sec.
   out_buf receives max_samples rows of sample_stride bytes (20 for web). */
int vgm_player_scan_channel_levels(VgmPlayer *player, double end_sec, double step_sec,
                                   uint8_t *out_buf, size_t sample_stride, size_t max_samples,
                                   size_t *out_count);

/* Mono int16 PCM @ VGMPSG_OUTPUT_RATE (WAV export writes identical L/R stereo frames). */
int vgm_export_wav(const char *path, const int16_t *samples, size_t count);
int vgm_export_wav_alloc(const int16_t *samples, size_t count, uint8_t **out_data, size_t *out_size);
void vgmpsg_free_buffer(void *p);

#define VGM_REC_AY  0u
#define VGM_REC_SCC 1u

/* One timed export command: AY (b0=reg, b1=val) or SCC 0xD2 (b0=port, b1=aa, b2=dd). */
typedef struct {
    uint32_t wait_44100;
    uint8_t kind;
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
} VgmRecCmd;

/* Record commands in range for VGM slice export (caller frees *cmds). */
int vgm_player_record_range(VgmPlayer *player, const VgmTimeRange *range, VgmRecCmd **cmds,
                            size_t *count);

/* Seek to output-sample time and capture AY R0-R13 (R7 includes export mute_mask). */
int vgm_player_snapshot_ay_at_output(VgmPlayer *player, uint64_t target_out, uint8_t out[14]);

/* One-shot VGM slice with adjusted header (kss2vgm 0xA0 reg val style). */
int vgm_export_vgm_slice(const VgmFile *file, uint8_t mute_mask, const VgmTimeRange *range,
                         const char *out_path);
int vgm_export_vgm_slice_alloc(const VgmFile *file, const VgmTimeRange *range, uint8_t **out_data,
                               size_t *out_size);
/* Re-emit 0xA0 commands in range with player mute_mask applied to R7 (matches WAV export). */
int vgm_export_vgm_slice_recorded(VgmPlayer *player, const VgmTimeRange *range,
                                  const char *out_path);
int vgm_export_vgm_slice_recorded_alloc(VgmPlayer *player, const VgmTimeRange *range,
                                        uint8_t **out_data, size_t *out_size);

/* Minimal JSON labels sidecar (see README). */
int vgm_labels_load(const char *path, VgmLabels *out);
int vgm_labels_load_string(const char *json, size_t json_len, VgmLabels *out);
void vgm_labels_default_path(const char *vgm_path, char *out_path, size_t out_cap);
int vgm_labels_find(const VgmLabels *labels, const char *name, VgmTimeRange *out);

#ifdef __cplusplus
}
#endif

#endif
