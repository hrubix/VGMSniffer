#ifndef VGM_CMD_H
#define VGM_CMD_H

#include <stddef.h>
#include <stdint.h>

/* Advance *pos over one VGM command. Returns 0=ok, 1=end (0x66), -1=error. */
int vgm_cmd_advance(const uint8_t *data, size_t len, size_t *pos);

#endif
