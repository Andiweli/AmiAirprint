#include "airprint_testpage.h"
#include "testpage_rle.h"
#include "testpage_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_TEST_PWG_HEADER_SIZE 1796U
#define AP_TEST_DEFAULT_MEDIA "iso_a4_210x297mm"
#define AP_TEST_MAX_DPI 2400U
#define AP_TEST_MIN_DPI 72U

#define AP_TEST_PWG_COLORSPACE_SW   18U
#define AP_TEST_PWG_COLORSPACE_SRGB 19U

#define AP_TEST_IMAGE_WIDTH  600U
#define AP_TEST_IMAGE_HEIGHT 848U

struct APTestBuffer {
    uint8_t *data;
    size_t length;
    size_t capacity;
};

static void ap_test_set_error(char *error_text, size_t error_text_size, const char *message)
{
    if (error_text == NULL || error_text_size == 0U) return;
    if (message == NULL) message = "Could not build test page";
    snprintf(error_text, error_text_size, "%s", message);
}

static int ap_test_buffer_reserve(struct APTestBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *new_data;

    if (buffer == NULL) return 0;
    if (additional > ((size_t)-1) - buffer->length) return 0;
    needed = buffer->length + additional;
    if (needed <= buffer->capacity) return 1;

    capacity = buffer->capacity != 0U ? buffer->capacity : 4096U;
    while (capacity < needed) {
        size_t next = capacity << 1;
        if (next <= capacity) {
            capacity = needed;
            break;
        }
        capacity = next;
    }

    new_data = (uint8_t *)realloc(buffer->data, capacity);
    if (new_data == NULL) return 0;
    buffer->data = new_data;
    buffer->capacity = capacity;
    return 1;
}

static int ap_test_buffer_append(struct APTestBuffer *buffer, const void *data, size_t length)
{
    if (length == 0U) return 1;
    if (buffer == NULL || data == NULL) return 0;
    if (!ap_test_buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 1;
}

static int ap_test_buffer_append_text(struct APTestBuffer *buffer, const char *text)
{
    return ap_test_buffer_append(buffer, text, strlen(text));
}

static void ap_test_put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void ap_test_copy_header_text(uint8_t *dst, size_t capacity, const char *src)
{
    size_t i = 0U;
    if (capacity == 0U) return;
    while (i + 1U < capacity && src[i] != '\0') {
        dst[i] = (uint8_t)src[i];
        ++i;
    }
}

static int ap_test_parse_decimal_1000(const char **cursor, char stop, uint32_t *value)
{
    const char *p;
    uint32_t whole = 0U;
    uint32_t fraction = 0U;
    unsigned int fraction_digits = 0U;
    int saw_digit = 0;

    if (cursor == NULL || *cursor == NULL || value == NULL) return 0;
    p = *cursor;

    while (*p >= '0' && *p <= '9') {
        whole = whole * 10U + (uint32_t)(*p - '0');
        ++p;
        saw_digit = 1;
    }
    if (!saw_digit) return 0;

    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (fraction_digits < 3U)
                fraction = fraction * 10U + (uint32_t)(*p - '0');
            ++fraction_digits;
            ++p;
        }
    }

    if (*p != stop) return 0;
    while (fraction_digits < 3U) {
        fraction *= 10U;
        ++fraction_digits;
    }

    *value = whole * 1000U + fraction;
    *cursor = p + 1;
    return 1;
}

static int ap_test_media_geometry(const char *keyword,
                                  uint32_t dpi,
                                  uint32_t *width_pixels,
                                  uint32_t *height_pixels,
                                  uint32_t *width_points,
                                  uint32_t *height_points)
{
    const char *p;
    const char *dims;
    const char *unit;
    uint32_t w1000;
    uint32_t h1000;
    uint32_t wp;
    uint32_t hp;
    uint32_t wpt;
    uint32_t hpt;
    int millimeters;

    if (keyword == NULL || width_pixels == NULL || height_pixels == NULL ||
        width_points == NULL || height_points == NULL) return 0;

    if (strncmp(keyword, "custom_min_", 11U) == 0 ||
        strncmp(keyword, "custom_max_", 11U) == 0) return 0;

    dims = keyword;
    for (p = keyword; *p != '\0'; ++p) {
        if (*p == '_') dims = p + 1;
    }
    if (*dims == '\0') return 0;

    p = dims;
    if (!ap_test_parse_decimal_1000(&p, 'x', &w1000)) return 0;

    unit = p;
    while (*unit >= '0' && *unit <= '9') ++unit;
    if (*unit == '.') {
        ++unit;
        while (*unit >= '0' && *unit <= '9') ++unit;
    }

    if (unit[0] == 'm' && unit[1] == 'm' && unit[2] == '\0')
        millimeters = 1;
    else if (unit[0] == 'i' && unit[1] == 'n' && unit[2] == '\0')
        millimeters = 0;
    else
        return 0;

    if (!ap_test_parse_decimal_1000(&p, millimeters ? 'm' : 'i', &h1000)) return 0;
    if ((millimeters && (p[0] != 'm' || p[1] != '\0')) ||
        (!millimeters && (p[0] != 'n' || p[1] != '\0'))) return 0;

    if (millimeters) {
        wp = (w1000 * dpi) / 25400U;
        hp = (h1000 * dpi) / 25400U;
        wpt = (w1000 * 72U + 12700U) / 25400U;
        hpt = (h1000 * 72U + 12700U) / 25400U;
    } else {
        wp = (w1000 * dpi) / 1000U;
        hp = (h1000 * dpi) / 1000U;
        wpt = (w1000 * 72U + 500U) / 1000U;
        hpt = (h1000 * 72U + 500U) / 1000U;
    }

    if (wp == 0U || hp == 0U || wp > 30000U || hp > 30000U ||
        wpt == 0U || hpt == 0U || wpt > 3000U || hpt > 3000U) return 0;

    *width_pixels = wp;
    *height_pixels = hp;
    *width_points = wpt;
    *height_points = hpt;
    return 1;
}

static uint32_t ap_test_select_dpi(const struct APPrefs *prefs,
                                   const struct APPrinterCapabilities *caps,
                                   int caps_valid)
{
    unsigned int i;
    uint32_t dpi;

    dpi = prefs != NULL && prefs->resolution.units == 3U &&
          prefs->resolution.x == prefs->resolution.y
        ? prefs->resolution.x : 0U;

    if (dpi == 0U && caps_valid && caps != NULL &&
        caps->resolution_default.units == 3U &&
        caps->resolution_default.x == caps->resolution_default.y) {
        dpi = caps->resolution_default.x;
    }

    if (dpi == 0U && caps_valid && caps != NULL) {
        for (i = 0U; i < caps->resolution_count; ++i) {
            if (caps->resolutions[i].units == 3U &&
                caps->resolutions[i].x == caps->resolutions[i].y) {
                dpi = caps->resolutions[i].x;
                break;
            }
        }
    }

    if (dpi < AP_TEST_MIN_DPI || dpi > AP_TEST_MAX_DPI) dpi = 600U;
    return dpi;
}

static int ap_test_decode_source_row(uint32_t src_y, uint8_t *indices)
{
    size_t pos;
    size_t end;
    uint32_t out_pos = 0U;

    if (indices == NULL || src_y >= AP_TESTPAGE_RLE_HEIGHT) return 0;

    pos = (size_t)g_airprint_testpage_rle_row_offsets[src_y];
    end = (size_t)g_airprint_testpage_rle_row_offsets[src_y + 1U];
    if (pos > end || end > g_airprint_testpage_rle_data_len) return 0;

    while (pos < end && out_pos < AP_TESTPAGE_RLE_WIDTH) {
        uint8_t command = g_airprint_testpage_rle_data[pos++];
        uint32_t count = (uint32_t)(command & 0x7fU) + 1U;

        if (out_pos + count > AP_TESTPAGE_RLE_WIDTH) return 0;

        if ((command & 0x80U) != 0U) {
            uint8_t value;
            if (pos >= end) return 0;
            value = g_airprint_testpage_rle_data[pos++];
            memset(indices + out_pos, value, count);
        } else {
            if (pos + count > end) return 0;
            memcpy(indices + out_pos, g_airprint_testpage_rle_data + pos, count);
            pos += count;
        }
        out_pos += count;
    }

    return pos == end && out_pos == AP_TESTPAGE_RLE_WIDTH;
}

static int ap_test_encode_pwg_row(struct APTestBuffer *buffer,
                                  const uint8_t *row,
                                  uint32_t width,
                                  unsigned int components,
                                  uint8_t repeat_minus_one)
{
    uint32_t pos = 0U;

    if (!ap_test_buffer_append(buffer, &repeat_minus_one, 1U)) return 0;

    while (pos < width) {
        uint32_t run = 1U;
        uint32_t i;
        uint32_t count;
        int same;
        uint8_t control;

        while (run < 128U && pos + run < width) {
            same = 1;
            for (i = 0U; i < components; ++i) {
                if (row[(pos + run) * components + i] != row[pos * components + i]) {
                    same = 0;
                    break;
                }
            }
            if (!same) break;
            ++run;
        }

        if (run >= 2U) {
            control = (uint8_t)(run - 1U);
            if (!ap_test_buffer_append(buffer, &control, 1U) ||
                !ap_test_buffer_append(buffer, row + pos * components, components)) return 0;
            pos += run;
        } else {
            count = 1U;
            while (count < 128U && pos + count < width) {
                uint32_t next_run = 1U;
                while (next_run < 2U && pos + count + next_run < width) {
                    same = 1;
                    for (i = 0U; i < components; ++i) {
                        if (row[(pos + count + next_run) * components + i] !=
                            row[(pos + count) * components + i]) {
                            same = 0;
                            break;
                        }
                    }
                    if (!same) break;
                    ++next_run;
                }
                if (next_run >= 2U) break;
                ++count;
            }

            control = count == 1U ? 0U : (uint8_t)(257U - count);
            if (!ap_test_buffer_append(buffer, &control, 1U) ||
                !ap_test_buffer_append(buffer, row + pos * components,
                                       (size_t)count * components)) return 0;
            pos += count;
        }
    }

    return 1;
}

static void ap_test_make_scaled_row(uint8_t *row,
                                    uint32_t dst_width,
                                    unsigned int components,
                                    const uint8_t *source_indices)
{
    uint32_t x;

    if (components == 1U) {
        for (x = 0U; x < dst_width; ++x) {
            uint32_t src_x = (x * AP_TESTPAGE_RLE_WIDTH) / dst_width;
            const uint8_t *entry;
            uint8_t idx;
            uint32_t gray;
            if (src_x >= AP_TESTPAGE_RLE_WIDTH) src_x = AP_TESTPAGE_RLE_WIDTH - 1U;
            idx = source_indices[src_x];
            entry = g_airprint_testpage_rle_palette + (size_t)idx * 3U;
            gray = (30U * (uint32_t)entry[0] +
                    59U * (uint32_t)entry[1] +
                    11U * (uint32_t)entry[2] + 50U) / 100U;
            row[x] = (uint8_t)gray;
        }
    } else {
        for (x = 0U; x < dst_width; ++x) {
            uint32_t src_x = (x * AP_TESTPAGE_RLE_WIDTH) / dst_width;
            const uint8_t *entry;
            uint8_t idx;
            if (src_x >= AP_TESTPAGE_RLE_WIDTH) src_x = AP_TESTPAGE_RLE_WIDTH - 1U;
            idx = source_indices[src_x];
            entry = g_airprint_testpage_rle_palette + (size_t)idx * 3U;
            row[x * 3U + 0U] = entry[0];
            row[x * 3U + 1U] = entry[1];
            row[x * 3U + 2U] = entry[2];
        }
    }
}

static int ap_test_build_pwg(const struct APPrefs *prefs,
                             const struct APPrinterCapabilities *caps,
                             int caps_valid,
                             struct APTestPageDocument *document,
                             char *error_text,
                             size_t error_text_size)
{
    struct APTestBuffer buffer;
    const char *media;
    uint8_t *header;
    uint8_t *row;
    uint8_t *source_row;
    uint32_t dpi;
    uint32_t width;
    uint32_t height;
    uint32_t width_points;
    uint32_t height_points;
    uint32_t row_bytes;
    unsigned int components;
    uint32_t color_space;
    uint32_t y;
    uint32_t cached_src_y = (uint32_t)-1;

    memset(&buffer, 0, sizeof(buffer));

    dpi = ap_test_select_dpi(prefs, caps, caps_valid);
    media = prefs != NULL && prefs->media[0] != '\0' ? prefs->media : AP_TEST_DEFAULT_MEDIA;
    if (!ap_test_media_geometry(media, dpi, &width, &height, &width_points, &height_points)) {
        media = AP_TEST_DEFAULT_MEDIA;
        if (!ap_test_media_geometry(media, dpi, &width, &height, &width_points, &height_points)) {
            ap_test_set_error(error_text, error_text_size, "Could not determine test-page media geometry");
            return 0;
        }
    }

    components = 3U;
    color_space = AP_TEST_PWG_COLORSPACE_SRGB;
    if (caps_valid && caps != NULL && caps->pwg_sgray8_supported &&
        (!caps->pwg_srgb8_supported ||
         (prefs != NULL && (strcmp(prefs->color_mode, "monochrome") == 0 ||
                            strcmp(prefs->color_mode, "auto-monochrome") == 0)))) {
        components = 1U;
        color_space = AP_TEST_PWG_COLORSPACE_SW;
    }

    row_bytes = width * components;
    header = (uint8_t *)calloc(1U, 4U + AP_TEST_PWG_HEADER_SIZE);
    row = (uint8_t *)malloc(row_bytes);
    source_row = (uint8_t *)malloc(AP_TESTPAGE_RLE_WIDTH);
    if (header == NULL || row == NULL || source_row == NULL) {
        free(header);
        free(row);
        free(source_row);
        ap_test_set_error(error_text, error_text_size, "Not enough memory for PWG test page");
        return 0;
    }

    header[0] = 0x52U;
    header[1] = 0x61U;
    header[2] = 0x53U;
    header[3] = 0x32U;
    ap_test_copy_header_text(header + 4U + 0U, 64U, "PwgRaster");
    ap_test_copy_header_text(header + 4U + 128U, 64U, "stationery");
    ap_test_put_be32(header + 4U + 276U, dpi);
    ap_test_put_be32(header + 4U + 280U, dpi);
    ap_test_put_be32(header + 4U + 340U, 1U);
    ap_test_put_be32(header + 4U + 344U, 0U);
    ap_test_put_be32(header + 4U + 352U, width_points);
    ap_test_put_be32(header + 4U + 356U, height_points);
    ap_test_put_be32(header + 4U + 372U, width);
    ap_test_put_be32(header + 4U + 376U, height);
    ap_test_put_be32(header + 4U + 384U, 8U);
    ap_test_put_be32(header + 4U + 388U, components == 1U ? 8U : 24U);
    ap_test_put_be32(header + 4U + 392U, row_bytes);
    ap_test_put_be32(header + 4U + 396U, 0U);
    ap_test_put_be32(header + 4U + 400U, color_space);
    ap_test_put_be32(header + 4U + 420U, components);
    ap_test_put_be32(header + 4U + 452U, 1U);
    ap_test_put_be32(header + 4U + 456U, 1U);
    ap_test_put_be32(header + 4U + 460U, 1U);
    ap_test_put_be32(header + 4U + 464U, 0U);
    ap_test_put_be32(header + 4U + 468U, 0U);
    ap_test_put_be32(header + 4U + 472U, width);
    ap_test_put_be32(header + 4U + 476U, height);
    ap_test_put_be32(header + 4U + 484U, 0U);
    ap_test_copy_header_text(header + 4U + 1732U, 64U, media);

    if (!ap_test_buffer_append(&buffer, header, 4U + AP_TEST_PWG_HEADER_SIZE)) goto memory_fail;
    free(header);
    header = NULL;

    y = 0U;
    while (y < height) {
        uint32_t src_y = (y * AP_TESTPAGE_RLE_HEIGHT) / height;
        uint32_t repeat = 1U;
        if (src_y >= AP_TESTPAGE_RLE_HEIGHT) src_y = AP_TESTPAGE_RLE_HEIGHT - 1U;

        while (y + repeat < height && repeat < 256U &&
               ((y + repeat) * AP_TESTPAGE_RLE_HEIGHT) / height == src_y) {
            ++repeat;
        }

        if (src_y != cached_src_y) {
            if (!ap_test_decode_source_row(src_y, source_row)) goto image_fail;
            cached_src_y = src_y;
        }

        ap_test_make_scaled_row(row, width, components, source_row);
        if (!ap_test_encode_pwg_row(&buffer, row, width, components,
                                    (uint8_t)(repeat - 1U))) goto memory_fail;
        y += repeat;
    }

    free(source_row);
    free(row);
    document->allocated = buffer.data;
    document->data = buffer.data;
    document->length = buffer.length;
    snprintf(document->format, sizeof(document->format), "%s", "image/pwg-raster");
    return 1;

image_fail:
    free(header);
    free(source_row);
    free(row);
    free(buffer.data);
    ap_test_set_error(error_text, error_text_size, "Embedded PWG test image is invalid");
    return 0;

memory_fail:
    free(header);
    free(source_row);
    free(row);
    free(buffer.data);
    ap_test_set_error(error_text, error_text_size, "Not enough memory for PWG test page");
    return 0;
}

static int ap_test_append_pdf_image(struct APTestBuffer *buffer,
                                    uint32_t page_width,
                                    uint32_t page_height)
{
    size_t offsets[6];
    size_t xref_offset;
    size_t content_start;
    size_t content_length;
    char temp[256];
    unsigned int i;

    memset(offsets, 0, sizeof(offsets));

    if (!ap_test_buffer_append_text(buffer, "%PDF-1.3\n")) return 0;

    offsets[1] = buffer->length;
    if (!ap_test_buffer_append_text(buffer, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) return 0;

    offsets[2] = buffer->length;
    if (!ap_test_buffer_append_text(buffer, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) return 0;

    offsets[3] = buffer->length;
    snprintf(temp, sizeof(temp),
             "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %lu %lu] "
             "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n",
             (unsigned long)page_width, (unsigned long)page_height);
    if (!ap_test_buffer_append_text(buffer, temp)) return 0;

    offsets[4] = buffer->length;
    snprintf(temp, sizeof(temp),
             "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %u /Height %u "
             "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %lu >>\nstream\n",
             (unsigned int)AP_TEST_IMAGE_WIDTH,
             (unsigned int)AP_TEST_IMAGE_HEIGHT,
             (unsigned long)g_airprint_testpage_jpeg_len);
    if (!ap_test_buffer_append_text(buffer, temp) ||
        !ap_test_buffer_append(buffer, g_airprint_testpage_jpeg, g_airprint_testpage_jpeg_len) ||
        !ap_test_buffer_append_text(buffer, "\nendstream\nendobj\n")) return 0;

    offsets[5] = buffer->length;
    if (!ap_test_buffer_append_text(buffer, "5 0 obj\n<< /Length ")) return 0;
    {
        size_t length_marker = buffer->length;
        if (!ap_test_buffer_append_text(buffer, "0000000000 >>\nstream\n")) return 0;
        content_start = buffer->length;
        snprintf(temp, sizeof(temp),
                 "q\n%lu 0 0 %lu 0 0 cm\n/Im0 Do\nQ\n",
                 (unsigned long)page_width,
                 (unsigned long)page_height);
        if (!ap_test_buffer_append_text(buffer, temp)) return 0;
        content_length = buffer->length - content_start;
        if (!ap_test_buffer_append_text(buffer, "endstream\nendobj\n")) return 0;
        snprintf(temp, sizeof(temp), "%010lu", (unsigned long)content_length);
        memcpy(buffer->data + length_marker, temp, 10U);
    }

    xref_offset = buffer->length;
    if (!ap_test_buffer_append_text(buffer, "xref\n0 6\n0000000000 65535 f \n")) return 0;
    for (i = 1U; i <= 5U; ++i) {
        snprintf(temp, sizeof(temp), "%010lu 00000 n \n", (unsigned long)offsets[i]);
        if (!ap_test_buffer_append_text(buffer, temp)) return 0;
    }
    snprintf(temp, sizeof(temp),
             "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%lu\n%%%%EOF\n",
             (unsigned long)xref_offset);
    if (!ap_test_buffer_append_text(buffer, temp)) return 0;

    return 1;
}

static int ap_test_build_pdf(const struct APPrefs *prefs,
                             struct APTestPageDocument *document,
                             char *error_text,
                             size_t error_text_size)
{
    struct APTestBuffer buffer;
    uint32_t width_points = 595U;
    uint32_t height_points = 842U;
    uint32_t dummy_w;
    uint32_t dummy_h;
    const char *media;

    memset(&buffer, 0, sizeof(buffer));
    media = prefs != NULL && prefs->media[0] != '\0' ? prefs->media : AP_TEST_DEFAULT_MEDIA;
    (void)ap_test_media_geometry(media, 600U, &dummy_w, &dummy_h, &width_points, &height_points);

    if (!ap_test_append_pdf_image(&buffer, width_points, height_points)) {
        free(buffer.data);
        ap_test_set_error(error_text, error_text_size, "Not enough memory for PDF test page");
        return 0;
    }

    document->allocated = buffer.data;
    document->data = buffer.data;
    document->length = buffer.length;
    snprintf(document->format, sizeof(document->format), "%s", "application/pdf");
    return 1;
}

static int ap_test_build_postscript(const struct APPrefs *prefs,
                                    struct APTestPageDocument *document,
                                    char *error_text,
                                    size_t error_text_size)
{
    struct APTestBuffer buffer;
    uint32_t width_points = 595U;
    uint32_t height_points = 842U;
    uint32_t dummy_w;
    uint32_t dummy_h;
    uint32_t inner_width;
    const char *media;
    char temp[256];

    memset(&buffer, 0, sizeof(buffer));
    media = prefs != NULL && prefs->media[0] != '\0' ? prefs->media : AP_TEST_DEFAULT_MEDIA;
    (void)ap_test_media_geometry(media, 600U, &dummy_w, &dummy_h, &width_points, &height_points);
    inner_width = width_points > 108U ? width_points - 108U : 20U;

    if (!ap_test_buffer_append_text(&buffer,
        "%!PS-Adobe-3.0\n"
        "%%Pages: 1\n")) goto memory_fail;

    snprintf(temp, sizeof(temp), "%%%%BoundingBox: 0 0 %lu %lu\n",
             (unsigned long)width_points, (unsigned long)height_points);
    if (!ap_test_buffer_append_text(&buffer, temp) ||
        !ap_test_buffer_append_text(&buffer,
            "%%LanguageLevel: 2\n"
            "%%EndComments\n")) goto memory_fail;

    snprintf(temp, sizeof(temp), "<< /PageSize [%lu %lu] >> setpagedevice\n",
             (unsigned long)width_points, (unsigned long)height_points);
    if (!ap_test_buffer_append_text(&buffer, temp) ||
        !ap_test_buffer_append_text(&buffer,
            "/Helvetica-Bold findfont 24 scalefont setfont\n")) goto memory_fail;

    snprintf(temp, sizeof(temp),
             "54 %lu moveto (AmiAirPrint Test Page) show\n",
             (unsigned long)(height_points > 80U ? height_points - 70U : 20U));
    if (!ap_test_buffer_append_text(&buffer, temp)) goto memory_fail;

    snprintf(temp, sizeof(temp),
             "0 setgray 54 %lu moveto %lu 0 rlineto 0 10 rlineto -%lu 0 rlineto closepath fill\n",
             (unsigned long)(height_points > 120U ? height_points - 110U : 10U),
             (unsigned long)inner_width,
             (unsigned long)inner_width);
    if (!ap_test_buffer_append_text(&buffer, temp)) goto memory_fail;

    snprintf(temp, sizeof(temp),
             "0.35 setgray 54 %lu moveto %lu 0 rlineto 0 24 rlineto -%lu 0 rlineto closepath fill\n",
             (unsigned long)(height_points > 180U ? height_points - 165U : 10U),
             (unsigned long)inner_width,
             (unsigned long)inner_width);
    if (!ap_test_buffer_append_text(&buffer, temp)) goto memory_fail;

    snprintf(temp, sizeof(temp),
             "0.70 setgray 54 %lu moveto %lu 0 rlineto 0 24 rlineto -%lu 0 rlineto closepath fill\n",
             (unsigned long)(height_points > 230U ? height_points - 215U : 10U),
             (unsigned long)inner_width,
             (unsigned long)inner_width);
    if (!ap_test_buffer_append_text(&buffer, temp)) goto memory_fail;

    snprintf(temp, sizeof(temp),
             "0 setgray 54 54 moveto %lu 0 rlineto stroke\n",
             (unsigned long)inner_width);
    if (!ap_test_buffer_append_text(&buffer, temp) ||
        !ap_test_buffer_append_text(&buffer, "showpage\n%%EOF\n")) goto memory_fail;

    document->allocated = buffer.data;
    document->data = buffer.data;
    document->length = buffer.length;
    snprintf(document->format, sizeof(document->format), "%s", "application/postscript");
    return 1;

memory_fail:
    free(buffer.data);
    ap_test_set_error(error_text, error_text_size, "Not enough memory for PostScript test page");
    return 0;
}
static int ap_test_engine_supported(const char *engine,
                                    const struct APPrinterCapabilities *caps,
                                    int caps_valid)
{
    if (!caps_valid || caps == NULL) return 1;
    if (strcmp(engine, "pwg-raster") == 0) return caps->format_pwg_raster_supported;
    if (strcmp(engine, "pdf") == 0) return caps->format_pdf_supported;
    if (strcmp(engine, "postscript") == 0) return caps->format_postscript_supported;
    return 0;
}

int ap_testpage_build(const struct APPrefs *prefs,
                      const struct APPrinterCapabilities *caps,
                      int caps_valid,
                      struct APTestPageDocument *document,
                      char *error_text,
                      size_t error_text_size)
{
    const char *engine;

    if (document == NULL || prefs == NULL) {
        ap_test_set_error(error_text, error_text_size, "Invalid test-page arguments");
        return 0;
    }

    memset(document, 0, sizeof(*document));
    if (error_text != NULL && error_text_size != 0U) error_text[0] = '\0';

    engine = prefs->engine[0] != '\0' ? prefs->engine : "pwg-raster";

    if (ap_test_engine_supported(engine, caps, caps_valid)) {
        if (strcmp(engine, "pwg-raster") == 0)
            return ap_test_build_pwg(prefs, caps, caps_valid, document, error_text, error_text_size);
        if (strcmp(engine, "pdf") == 0)
            return ap_test_build_pdf(prefs, document, error_text, error_text_size);
        if (strcmp(engine, "postscript") == 0)
            return ap_test_build_postscript(prefs, document, error_text, error_text_size);
    }

    if (caps_valid && caps != NULL) {
        if (caps->format_pwg_raster_supported)
            return ap_test_build_pwg(prefs, caps, caps_valid, document, error_text, error_text_size);
        if (caps->format_pdf_supported)
            return ap_test_build_pdf(prefs, document, error_text, error_text_size);
        if (caps->format_postscript_supported)
            return ap_test_build_postscript(prefs, document, error_text, error_text_size);
        if (caps->format_jpeg_supported) {
            document->data = g_airprint_testpage_jpeg;
            document->length = g_airprint_testpage_jpeg_len;
            snprintf(document->format, sizeof(document->format), "%s", "image/jpeg");
            return 1;
        }
        ap_test_set_error(error_text, error_text_size, "Printer reports no supported AmiAirPrint test-page format");
        return 0;
    }

    document->data = g_airprint_testpage_jpeg;
    document->length = g_airprint_testpage_jpeg_len;
    snprintf(document->format, sizeof(document->format), "%s", "image/jpeg");
    return 1;
}

void ap_testpage_free(struct APTestPageDocument *document)
{
    if (document == NULL) return;
    free(document->allocated);
    memset(document, 0, sizeof(*document));
}
