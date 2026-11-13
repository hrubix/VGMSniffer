#include "vgm_file.h"
#include "vgm_cmd.h"
#include "vgm_scc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Drop-in zlib subset via miniz (gzip header stripped before inflate). */
#include "miniz.h"

#define VGM_HEADER_MIN 0x40u
#define VGM_EOF_OFFSET 0x04u
#define VGM_VERSION_OFFSET 0x08u
#define VGM_TOTAL_SAMPLES_OFFSET 0x18u
#define VGM_LOOP_OFFSET 0x1cu
#define VGM_LOOP_SAMPLES_OFFSET 0x20u
#define VGM_DATA_OFFSET 0x34u
#define VGM_SN76489_CLK_OFFSET 0x0cu
#define VGM_YM2413_CLK_OFFSET 0x10u
#define VGM_YM2612_CLK_OFFSET 0x2cu
#define VGM_YM2151_CLK_OFFSET 0x30u
#define VGM_YM2203_CLK_OFFSET 0x84u
#define VGM_YM2608_CLK_OFFSET 0x9cu
#define VGM_AY8910_CLK_OFFSET 0x74u
#define VGM_GD3_OFFSET 0x14u
#define VGM_HEADER_END 0x34u
/* MSX AY-3-8910 and close variants; reject bogus values from mis-parsed headers. */
#define VGM_AY_CLOCK_MIN_HZ 100000u
#define VGM_AY_CLOCK_MAX_HZ 10000000u

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int looks_like_vgm(const uint8_t *data, size_t size)
{
    return data && size >= 4 && memcmp(data, "Vgm ", 4) == 0;
}

static int ends_with_ci(const char *path, const char *suffix)
{
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) {
        return 0;
    }
    const char *tail = path + (plen - slen);
#ifdef _WIN32
    return _stricmp(tail, suffix) == 0;
#else
    return strcasecmp(tail, suffix) == 0;
#endif
}

static size_t gzip_payload_offset(const uint8_t *in, size_t in_size)
{
    /* RFC 1952: ID1 ID2 CM FLG MTIME(4) XFL OS — then optional fields. */
    if (in_size < 10u || in[0] != 0x1fu || in[1] != 0x8bu || in[2] != 8u) {
        return 0;
    }
    uint8_t flg = in[3];
    size_t pos = 10u;
    if (flg & 4u) { /* FEXTRA */
        if (pos + 2u > in_size) {
            return 0;
        }
        uint16_t xlen = (uint16_t)(in[pos] | ((uint16_t)in[pos + 1u] << 8));
        pos += 2u + (size_t)xlen;
    }
    if (flg & 8u) { /* FNAME */
        while (pos < in_size && in[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 16u) { /* FCOMMENT */
        while (pos < in_size && in[pos] != 0) {
            pos++;
        }
        pos++;
    }
    if (flg & 2u) { /* FHCRC */
        pos += 2u;
    }
    if (pos >= in_size) {
        return 0;
    }
    return pos;
}

static int decompress_gzip(const uint8_t *in, size_t in_size, uint8_t **out, size_t *out_size)
{
    size_t payload = gzip_payload_offset(in, in_size);
    if (payload == 0) {
        return -1;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)(in + payload);
    strm.avail_in = (uInt)(in_size - payload);

    /* miniz: raw deflate after stripping gzip wrapper */
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return -1;
    }

    size_t cap = in_size * 8u;
    if (cap < 65536u) {
        cap = 65536u;
    }
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        inflateEnd(&strm);
        return -1;
    }

    int rc = 0;
    for (;;) {
        if (strm.total_out >= cap) {
            size_t new_cap = cap * 2u;
            uint8_t *grown = (uint8_t *)realloc(buf, new_cap);
            if (!grown) {
                rc = -1;
                break;
            }
            buf = grown;
            cap = new_cap;
        }

        strm.next_out = buf + strm.total_out;
        strm.avail_out = (uInt)(cap - strm.total_out);
        int err = inflate(&strm, Z_NO_FLUSH);
        if (err == Z_STREAM_END) {
            *out = buf;
            *out_size = strm.total_out;
            break;
        }
        if (err != Z_OK) {
            rc = -1;
            break;
        }
    }

    inflateEnd(&strm);
    if (rc != 0) {
        free(buf);
    }
    return rc;
}

static int load_raw_bytes(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

/* First raw 0x66 in [start, limit) — may lie inside a 0x61 operand. */
static size_t vgm_find_naive_eof(const uint8_t *data, size_t start, size_t limit)
{
    for (size_t i = start; i < limit; i++) {
        if (data[i] == 0x66) {
            return i + 1u;
        }
    }
    return limit;
}

/* Walk the command stream; do not treat 0x66 inside 0x61 wait operands as EOF. */
static size_t vgm_scan_cmd_end(const uint8_t *data, size_t limit, size_t pos)
{
    while (pos < limit) {
        int ended = vgm_cmd_advance(data, limit, &pos);
        if (ended < 0) {
            break;
        }
        if (ended > 0) {
            return pos;
        }
    }
    return pos;
}

static uint32_t read_header_u32_le(const uint8_t *data, size_t size, size_t cmd_start, size_t off)
{
    if (off + 4u > cmd_start || off + 4u > size) {
        return 0;
    }
    return read_u32_le(data + off);
}

static uint32_t normalize_ay_clock_hz(uint32_t ay_clk)
{
    if (ay_clk < VGM_AY_CLOCK_MIN_HZ || ay_clk > VGM_AY_CLOCK_MAX_HZ) {
        return 0;
    }
    return ay_clk;
}

#define VGM_SCC_CLOCK_MIN_HZ 100000u
#define VGM_SCC_CLOCK_MAX_HZ 10000000u

static uint32_t normalize_scc_clock_hz(uint32_t scc_clk)
{
    uint32_t hz = scc_clk & 0x7fffffffu;
    if (hz < VGM_SCC_CLOCK_MIN_HZ || hz > VGM_SCC_CLOCK_MAX_HZ) {
        return 0;
    }
    return hz;
}

/* MSX SCC loggers store K051649 clock at VGM header 0x9C (YM2608 slot). */
static int vgm_header_scc_clock_looks_msx(uint32_t scc_hdr)
{
    uint32_t hz = normalize_scc_clock_hz(scc_hdr);

    if (hz == 0) {
        return 0;
    }
    if (hz >= 1500000u && hz <= 2000000u) {
        return 1;
    }
    if (hz >= 3500000u && hz <= 4000000u) {
        return 1;
    }
    return 0;
}

static void vgm_count_chip_writes(VgmFile *file)
{
    size_t pos;
    size_t limit;

    if (!file) {
        return;
    }

    file->ay_write_count = 0;
    file->fm_write_count = 0;
    file->scc_write_count = 0;
    pos = file->cmd_start;
    limit = file->cmd_end;

    while (pos < limit) {
        uint8_t cmd = file->data[pos];

        if (cmd == 0xA0) {
            if (pos + 3u > limit) {
                break;
            }
            if ((file->data[pos + 1] & 0x80u) == 0) {
                file->ay_write_count++;
            }
            pos += 3u;
            continue;
        }
        if (cmd >= 0x51 && cmd <= 0x5f) {
            if (pos + 3u > limit) {
                break;
            }
            file->fm_write_count++;
            pos += 3u;
            continue;
        }
        if (cmd == 0xD2) {
            if (pos + 4u > limit) {
                break;
            }
            file->scc_write_count++;
            pos += 4u;
            continue;
        }

        {
            int ended = vgm_cmd_advance(file->data, limit, &pos);
            if (ended < 0) {
                break;
            }
            if (ended > 0) {
                break;
            }
        }
    }
}

static VgmFile *vgm_file_parse_owned(uint8_t *data, size_t size)
{
    if (!data || size < VGM_HEADER_MIN || memcmp(data, "Vgm ", 4) != 0) {
        free(data);
        return NULL;
    }

    VgmFile *file = (VgmFile *)calloc(1, sizeof(VgmFile));
    if (!file) {
        free(data);
        return NULL;
    }

    file->data = data;
    file->size = size;

    file->info.version = read_u32_le(data + VGM_VERSION_OFFSET);
    file->info.total_samples_44100 = read_u32_le(data + VGM_TOTAL_SAMPLES_OFFSET);
    file->info.loop_samples_44100 = read_u32_le(data + VGM_LOOP_SAMPLES_OFFSET);
    file->info.loop_offset = read_u32_le(data + VGM_LOOP_OFFSET);
    file->info.has_loop = file->info.loop_samples_44100 > 0;

    uint32_t data_offset = read_u32_le(data + VGM_DATA_OFFSET);
    file->cmd_start = VGM_HEADER_END + data_offset;
    if (file->cmd_start > size) {
        vgm_file_free(file);
        return NULL;
    }

    {
        uint32_t ay_clk =
            normalize_ay_clock_hz(read_header_u32_le(data, size, file->cmd_start, VGM_AY8910_CLK_OFFSET));
        file->info.ay8910_clock_hz = ay_clk ? ay_clk : (uint32_t)VGMPSG_AY_CLOCK_HZ;
    }

    if (size > VGM_YM2608_CLK_OFFSET + 4u) {
        uint32_t scc_raw = read_u32_le(data + VGM_YM2608_CLK_OFFSET);
        file->scc_plus = (scc_raw & 0x80000000u) != 0 ? 1 : 0;
        file->scc_clock_hz = vgm_scc_emu_clock_hz(normalize_scc_clock_hz(scc_raw));
        if (file->scc_clock_hz == 0 && (scc_raw & 0x7fffffffu) > 0) {
            file->scc_clock_hz = (uint32_t)VGM_SCC_DEFAULT_CLOCK_HZ;
        }
    }

    uint32_t gd3_rel = read_u32_le(data + VGM_GD3_OFFSET);
    size_t cmd_end = size;
    if (gd3_rel > 0) {
        size_t gd3_abs = VGM_HEADER_END + gd3_rel;
        if (gd3_abs < size) {
            cmd_end = gd3_abs;
        }
    }

    size_t naive_end = vgm_find_naive_eof(data, file->cmd_start, cmd_end);
    file->cmd_end = vgm_scan_cmd_end(data, cmd_end, file->cmd_start);
    file->has_false_eof = naive_end < file->cmd_end ? 1 : 0;
    vgm_count_chip_writes(file);

    return file;
}

VgmFile *vgm_file_load_bytes(const uint8_t *data, size_t size, int is_vgz)
{
    if (!data || size == 0) {
        return NULL;
    }

    uint8_t *owned = NULL;
    size_t owned_size = 0;

    if (is_vgz && !looks_like_vgm(data, size)) {
        if (decompress_gzip(data, size, &owned, &owned_size) != 0) {
            return NULL;
        }
    } else {
        owned = (uint8_t *)malloc(size);
        if (!owned) {
            return NULL;
        }
        memcpy(owned, data, size);
        owned_size = size;
    }

    return vgm_file_parse_owned(owned, owned_size);
}

VgmFile *vgm_file_load(const char *path)
{
    uint8_t *raw = NULL;
    size_t raw_size = 0;
    if (load_raw_bytes(path, &raw, &raw_size) != 0) {
        return NULL;
    }

    if (ends_with_ci(path, ".vgz") && !looks_like_vgm(raw, raw_size)) {
        uint8_t *decomp = NULL;
        size_t decomp_size = 0;
        if (decompress_gzip(raw, raw_size, &decomp, &decomp_size) != 0) {
            free(raw);
            return NULL;
        }
        free(raw);
        return vgm_file_parse_owned(decomp, decomp_size);
    }

    return vgm_file_parse_owned(raw, raw_size);
}

void vgm_file_free(VgmFile *file)
{
    if (!file) {
        return;
    }
    free(file->data);
    free(file);
}

const uint8_t *vgm_file_data(const VgmFile *file)
{
    return file ? file->data : NULL;
}

size_t vgm_file_size(const VgmFile *file)
{
    return file ? file->size : 0;
}

int vgm_file_info(const VgmFile *file, VgmFileInfo *out)
{
    if (!file || !out) {
        return -1;
    }
    *out = file->info;
    return 0;
}

double vgm_file_duration_sec(const VgmFile *file)
{
    if (!file) {
        return 0.0;
    }
    return file->info.total_samples_44100 / (double)VGMPSG_VGM_WAIT_RATE;
}

int vgm_file_has_false_eof(const VgmFile *file)
{
    return file ? file->has_false_eof : 0;
}

uint32_t vgm_file_extra_chip_flags(const VgmFile *file)
{
    uint32_t flags = 0;

    if (!file || file->size < VGM_HEADER_MIN) {
        return 0;
    }

    const uint8_t *h = file->data;

    if (read_u32_le(h + VGM_SN76489_CLK_OFFSET) != 0) {
        flags |= VGMPSG_CHIP_SN76489;
    }
    if (read_u32_le(h + VGM_YM2413_CLK_OFFSET) != 0) {
        flags |= VGMPSG_CHIP_YM2413;
    }
    if (read_u32_le(h + VGM_YM2612_CLK_OFFSET) != 0) {
        flags |= VGMPSG_CHIP_YM2612;
    }
    if (read_u32_le(h + VGM_YM2151_CLK_OFFSET) != 0) {
        flags |= VGMPSG_CHIP_YM2151;
    }
    if (file->size > VGM_YM2203_CLK_OFFSET + 4u && read_u32_le(h + VGM_YM2203_CLK_OFFSET) != 0) {
        flags |= VGMPSG_CHIP_YM2203;
    }
    if (file->size > VGM_YM2608_CLK_OFFSET + 4u) {
        uint32_t scc_hdr = read_u32_le(h + VGM_YM2608_CLK_OFFSET);
        int msx_scc = file->scc_write_count > 0 || (scc_hdr & 0x80000000u) != 0 ||
                      vgm_header_scc_clock_looks_msx(scc_hdr);

        if ((scc_hdr & 0x7fffffffu) != 0 && msx_scc) {
            flags |= VGMPSG_CHIP_SCC;
        }
        if ((scc_hdr & 0x7fffffffu) != 0 && (scc_hdr & 0x80000000u) == 0 && !msx_scc) {
            flags |= VGMPSG_CHIP_YM2608;
        }
    }
    if (file->scc_write_count > 0) {
        flags |= VGMPSG_CHIP_SCC;
    }

    return flags;
}

uint32_t vgm_file_ay_write_count(const VgmFile *file)
{
    return file ? file->ay_write_count : 0;
}

uint32_t vgm_file_fm_write_count(const VgmFile *file)
{
    return file ? file->fm_write_count : 0;
}

uint32_t vgm_file_scc_write_count(const VgmFile *file)
{
    return file ? file->scc_write_count : 0;
}

uint32_t vgm_file_scc_clock_hz(const VgmFile *file)
{
    return file ? file->scc_clock_hz : 0;
}

int vgm_file_scc_plus(const VgmFile *file)
{
    return file ? file->scc_plus : 0;
}

int vgm_file_has_psg_data(const VgmFile *file)
{
    return file && file->ay_write_count > 0;
}

int vgm_file_has_scc_data(const VgmFile *file)
{
    return file && (file->scc_write_count > 0 || file->scc_clock_hz > 0);
}
