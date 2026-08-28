#ifndef AIRPRINT_CAPS_H
#define AIRPRINT_CAPS_H

#include <stddef.h>
#include <stdint.h>

#define AP_CAPS_MAX_COLOR_MODES   8
#define AP_CAPS_MAX_MEDIA         32
#define AP_CAPS_MAX_MEDIA_SOURCES 16
#define AP_CAPS_MAX_RESOLUTIONS   8
#define AP_CAPS_MAX_MARKERS       8
#define AP_CAPS_TEXT_LEN          96
#define AP_CAPS_MEDIA_LEN         64
#define AP_CAPS_SOURCE_LEN        64

struct APResolution {
    uint32_t x;
    uint32_t y;
    uint8_t units;
};

struct APPrinterCapabilities {
    char model[AP_CAPS_TEXT_LEN];
    char printer_name[AP_CAPS_TEXT_LEN];
    char airprint_version[AP_CAPS_TEXT_LEN];
    char state_reason[AP_CAPS_TEXT_LEN];

    unsigned int printer_state;
    int accepting_jobs;
    int color_supported;
    int duplex_supported;
    int duplex_long_edge_supported;
    int duplex_short_edge_supported;
    char sides_default[AP_CAPS_TEXT_LEN];

    int pwg_srgb8_supported;
    int pwg_sgray8_supported;
    char pwg_raster_sheet_back[AP_CAPS_TEXT_LEN];
    int format_pwg_raster_supported;
    int format_pdf_supported;
    int format_postscript_supported;
    int format_jpeg_supported;
    int http_no_expect_required;
    unsigned int http_expect_reject_status;
    int http_postbody_500_ok;

    int orientation_portrait_supported;
    int orientation_landscape_supported;
    int orientation_reverse_landscape_supported;
    unsigned int orientation_default;
    unsigned int landscape_orientation_preferred;

    int quality_draft;
    int quality_normal;
    int quality_high;
    unsigned int quality_default;

    char color_modes[AP_CAPS_MAX_COLOR_MODES][AP_CAPS_TEXT_LEN];
    unsigned int color_mode_count;
    char color_default[AP_CAPS_TEXT_LEN];

    struct APResolution resolutions[AP_CAPS_MAX_RESOLUTIONS];
    unsigned int resolution_count;
    struct APResolution resolution_default;

    char media[AP_CAPS_MAX_MEDIA][AP_CAPS_MEDIA_LEN];
    unsigned int media_count;
    char media_default[AP_CAPS_MEDIA_LEN];

    char media_sources[AP_CAPS_MAX_MEDIA_SOURCES][AP_CAPS_SOURCE_LEN];
    unsigned int media_source_count;
    char media_source_default[AP_CAPS_SOURCE_LEN];

    char marker_names[AP_CAPS_MAX_MARKERS][AP_CAPS_TEXT_LEN];
    int marker_levels[AP_CAPS_MAX_MARKERS];
    unsigned int marker_name_count;
    unsigned int marker_level_count;

    uint8_t ipp_version_major;
    uint8_t ipp_version_minor;
    char resolved_path[128];
};

void ap_caps_reset(struct APPrinterCapabilities *caps);

int ap_caps_query(
    const char *host,
    uint16_t port,
    const char *path,
    struct APPrinterCapabilities *caps,
    char *error_text,
    size_t error_text_size);

const char *ap_caps_state_text(const struct APPrinterCapabilities *caps);
const char *ap_caps_media_friendly(const char *keyword);

#endif
