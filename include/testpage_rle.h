#ifndef AIRPRINT_TESTPAGE_RLE_H
#define AIRPRINT_TESTPAGE_RLE_H

#include <stddef.h>
#include <stdint.h>

#define AP_TESTPAGE_RLE_WIDTH  600U
#define AP_TESTPAGE_RLE_HEIGHT 848U
#define AP_TESTPAGE_RLE_PALETTE_COLORS 256U

extern const uint8_t g_airprint_testpage_rle_palette[AP_TESTPAGE_RLE_PALETTE_COLORS * 3U];
extern const uint32_t g_airprint_testpage_rle_row_offsets[AP_TESTPAGE_RLE_HEIGHT + 1U];
extern const uint8_t g_airprint_testpage_rle_data[];
extern const size_t g_airprint_testpage_rle_data_len;

#endif
