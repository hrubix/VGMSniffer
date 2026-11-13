#ifndef VGM_FILE_H
#define VGM_FILE_H

#include "vgmpsg.h"

struct VgmFile {
    uint8_t *data;
    size_t size;
    size_t cmd_start;
    size_t cmd_end;
    int has_false_eof; /* 0x66 byte inside 0x61 operand would truncate naive parsers */
    uint32_t ay_write_count; /* 0xA0 PSG port writes */
    uint32_t fm_write_count; /* 0x51–0x5f FM chip writes */
    uint32_t scc_write_count; /* 0xD2 SCC writes */
    uint32_t scc_clock_hz;
    int scc_plus;
    VgmFileInfo info;
};

#endif
