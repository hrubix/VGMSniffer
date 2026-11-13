#include "vgm_cmd.h"

#include <stdint.h>

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int vgm_cmd_advance(const uint8_t *data, size_t len, size_t *pos)
{
    if (!data || !pos || *pos >= len) {
        return -1;
    }

    uint8_t cmd = data[*pos];

    if (cmd >= 0x70 && cmd <= 0x7f) {
        *pos += 1u;
        return 0;
    }
    if (cmd == 0x61) {
        if (*pos + 3 > len) {
            return -1;
        }
        *pos += 3u;
        return 0;
    }
    if (cmd == 0x62 || cmd == 0x63) {
        *pos += 1u;
        return 0;
    }
    if (cmd == 0x66) {
        *pos += 1u;
        return 1;
    }
    if (cmd == 0xA0) {
        if (*pos + 3 > len) {
            return -1;
        }
        *pos += 3u;
        return 0;
    }
    if (cmd == 0xA1) {
        if (*pos + 2 > len) {
            return -1;
        }
        *pos += 2u;
        return 0;
    }
    /* Reserved / other chips — skip per VGM spec instead of aborting playback. */
    if (cmd >= 0x30 && cmd <= 0x4e) {
        if (*pos + 2 > len) {
            return -1;
        }
        *pos += 2u;
        return 0;
    }
    if (cmd == 0x4f || cmd == 0x50) {
        if (*pos + 2 > len) {
            return -1;
        }
        *pos += 2u;
        return 0;
    }
    if (cmd >= 0x51 && cmd <= 0x5f) {
        if (*pos + 3 > len) {
            return -1;
        }
        *pos += 3u;
        return 0;
    }
    if (cmd >= 0xb0 && cmd <= 0xbf) {
        if (*pos + 3 > len) {
            return -1;
        }
        *pos += 3u;
        return 0;
    }
    if (cmd >= 0x80 && cmd <= 0x8f) {
        if (*pos + 2 > len) {
            return -1;
        }
        *pos += 2u;
        return 0;
    }
    if (cmd == 0x67) {
        if (*pos + 8 > len || data[*pos + 1] != 0x66) {
            return -1;
        }
        uint32_t block_size = read_u32_le(data + *pos + 3);
        *pos += 7u + (size_t)block_size;
        return 0;
    }
    if (cmd == 0x68) {
        if (*pos + 7 > len) {
            return -1;
        }
        uint8_t mode = data[*pos + 1];
        uint32_t offset = read_u32_le(data + *pos + 2);
        (void)mode;
        (void)offset;
        *pos += 7u;
        return 0;
    }
    if (cmd >= 0x90 && cmd <= 0x95) {
        if (*pos + 5 > len) {
            return -1;
        }
        *pos += 5u;
        return 0;
    }
    if (cmd >= 0xc0 && cmd <= 0xdf) {
        if (*pos + 4 > len) {
            return -1;
        }
        *pos += 4u;
        return 0;
    }
    if (cmd == 0xe0) {
        if (*pos + 5 > len) {
            return -1;
        }
        *pos += 5u;
        return 0;
    }
    if (cmd >= 0xe1 && cmd <= 0xff) {
        if (*pos + 5 > len) {
            return -1;
        }
        *pos += 5u;
        return 0;
    }

    return -1;
}
