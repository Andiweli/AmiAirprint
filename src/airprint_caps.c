#include "airprint_caps.h"
#include "airprint_http.h"
#include "airprint_ipp.h"

#include <stdio.h>
#include <string.h>

struct APIPPVersion {
    uint8_t major;
    uint8_t minor;
};

static const struct APIPPVersion g_versions[] = {
    { 2U, 0U },
    { 1U, 1U },
    { 1U, 0U }
};

static uint32_t ap_caps_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void ap_caps_copy_string(
    char *destination,
    size_t destination_size,
    const uint8_t *value,
    uint16_t value_len)
{
    size_t copy_len;

    if (destination == NULL || destination_size == 0U) {
        return;
    }

    copy_len = (size_t)value_len;
    if (copy_len >= destination_size) {
        copy_len = destination_size - 1U;
    }

    if (copy_len != 0U) {
        memcpy(destination, value, copy_len);
    }
    destination[copy_len] = '\0';
}

static int ap_caps_has_string(
    char values[][AP_CAPS_TEXT_LEN],
    unsigned int count,
    const char *candidate)
{
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        if (strcmp(values[i], candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ap_caps_has_media(
    char values[][AP_CAPS_MEDIA_LEN],
    unsigned int count,
    const char *candidate)
{
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        if (strcmp(values[i], candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ap_caps_has_resolution(const struct APPrinterCapabilities *caps,
                                  uint32_t x, uint32_t y, uint8_t units)
{
    unsigned int i;
    if (caps == NULL) return 0;
    for (i = 0U; i < caps->resolution_count; ++i) {
        if (caps->resolutions[i].x == x && caps->resolutions[i].y == y &&
            caps->resolutions[i].units == units) return 1;
    }
    return 0;
}

static int ap_caps_is_custom_bound(const char *media)
{
    if (media == NULL) return 0;
    return strncmp(media, "custom_min_", 11U) == 0 ||
           strncmp(media, "custom_max_", 11U) == 0;
}

static void ap_caps_attribute(
    uint8_t group_tag,
    const char *name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    void *user_data)
{
    struct APPrinterCapabilities *caps;
    char text[AP_CAPS_TEXT_LEN];
    uint32_t number;

    (void)group_tag;

    caps = (struct APPrinterCapabilities *)user_data;
    if (caps == NULL || name == NULL || name[0] == '\0') {
        return;
    }

    if (strcmp(name, "printer-make-and-model") == 0 ||
        strcmp(name, "printer-info") == 0) {
        if (caps->model[0] == '\0') {
            ap_caps_copy_string(caps->model, sizeof(caps->model), value, value_len);
        }
        return;
    }

    if (strcmp(name, "printer-name") == 0) {
        ap_caps_copy_string(caps->printer_name, sizeof(caps->printer_name), value, value_len);
        return;
    }

    if (strcmp(name, "ipp-features-supported") == 0) {
        ap_caps_copy_string(text, sizeof(text), value, value_len);
        if (strncmp(text, "airprint-", 9U) == 0) {
            ap_caps_copy_string(caps->airprint_version,
                                sizeof(caps->airprint_version),
                                value + 9U,
                                value_len > 9U ? (uint16_t)(value_len - 9U) : 0U);
        }
        return;
    }

    if (strcmp(name, "printer-state") == 0 && value_len == 4U) {
        caps->printer_state = (unsigned int)ap_caps_get_u32(value);
        return;
    }

    if (strcmp(name, "printer-state-reasons") == 0) {
        ap_caps_copy_string(caps->state_reason, sizeof(caps->state_reason), value, value_len);
        return;
    }

    if (strcmp(name, "printer-is-accepting-jobs") == 0 && value_len == 1U) {
        caps->accepting_jobs = value[0] != 0U;
        return;
    }

    if (strcmp(name, "color-supported") == 0 && value_len == 1U) {
        caps->color_supported = value[0] != 0U;
        return;
    }

    if (strcmp(name, "orientation-requested-supported") == 0 && value_len == 4U) {
        number = ap_caps_get_u32(value);
        if (number == 3U) caps->orientation_portrait_supported = 1;
        if (number == 4U) caps->orientation_landscape_supported = 1;
        if (number == 5U) caps->orientation_reverse_landscape_supported = 1;
        return;
    }

    if (strcmp(name, "orientation-requested-default") == 0 && value_len == 4U) {
        caps->orientation_default = (unsigned int)ap_caps_get_u32(value);
        return;
    }

    if (strcmp(name, "landscape-orientation-requested-preferred") == 0 && value_len == 4U) {
        caps->landscape_orientation_preferred = (unsigned int)ap_caps_get_u32(value);
        return;
    }

    if (strcmp(name, "pwg-raster-document-type-supported") == 0) {
        ap_caps_copy_string(text, sizeof(text), value, value_len);
        if (strcmp(text, "srgb_8") == 0) caps->pwg_srgb8_supported = 1;
        if (strcmp(text, "sgray_8") == 0) caps->pwg_sgray8_supported = 1;
        return;
    }

    if (strcmp(name, "pwg-raster-document-sheet-back") == 0) {
        ap_caps_copy_string(caps->pwg_raster_sheet_back,
                            sizeof(caps->pwg_raster_sheet_back),
                            value, value_len);
        return;
    }

    if (strcmp(name, "sides-supported") == 0) {
        ap_caps_copy_string(text, sizeof(text), value, value_len);
        if (strcmp(text, "two-sided-long-edge") == 0) {
            caps->duplex_supported = 1;
            caps->duplex_long_edge_supported = 1;
        } else if (strcmp(text, "two-sided-short-edge") == 0) {
            caps->duplex_supported = 1;
            caps->duplex_short_edge_supported = 1;
        }
        return;
    }

    if (strcmp(name, "sides-default") == 0) {
        ap_caps_copy_string(caps->sides_default, sizeof(caps->sides_default),
                            value, value_len);
        return;
    }

    if (strcmp(name, "document-format-supported") == 0) {
        ap_caps_copy_string(text, sizeof(text), value, value_len);
        if (strcmp(text, "image/pwg-raster") == 0)
            caps->format_pwg_raster_supported = 1;
        else if (strcmp(text, "application/pdf") == 0)
            caps->format_pdf_supported = 1;
        else if (strcmp(text, "application/postscript") == 0)
            caps->format_postscript_supported = 1;
        else if (strcmp(text, "image/jpeg") == 0)
            caps->format_jpeg_supported = 1;
        return;
    }

    if (strcmp(name, "print-quality-supported") == 0 && value_len == 4U) {
        number = ap_caps_get_u32(value);
        if (number == 3U) caps->quality_draft = 1;
        if (number == 4U) caps->quality_normal = 1;
        if (number == 5U) caps->quality_high = 1;
        return;
    }

    if (strcmp(name, "print-quality-default") == 0 && value_len == 4U) {
        caps->quality_default = (unsigned int)ap_caps_get_u32(value);
        return;
    }

    if (strcmp(name, "print-color-mode-supported") == 0) {
        if (caps->color_mode_count < AP_CAPS_MAX_COLOR_MODES) {
            ap_caps_copy_string(text, sizeof(text), value, value_len);
            if (!ap_caps_has_string(caps->color_modes,
                                    caps->color_mode_count,
                                    text)) {
                snprintf(caps->color_modes[caps->color_mode_count],
                         sizeof(caps->color_modes[caps->color_mode_count]),
                         "%s", text);
                ++caps->color_mode_count;
            }
        }
        return;
    }

    if (strcmp(name, "print-color-mode-default") == 0) {
        ap_caps_copy_string(caps->color_default, sizeof(caps->color_default), value, value_len);
        return;
    }

    if ((strcmp(name, "printer-resolution-supported") == 0 ||
         strcmp(name, "pwg-raster-document-resolution-supported") == 0) &&
        value_len == 9U) {
        uint32_t x = ap_caps_get_u32(value);
        uint32_t y = ap_caps_get_u32(value + 4U);
        uint8_t units = value[8];
        if (caps->resolution_count < AP_CAPS_MAX_RESOLUTIONS &&
            !ap_caps_has_resolution(caps, x, y, units)) {
            struct APResolution *resolution;
            resolution = &caps->resolutions[caps->resolution_count++];
            resolution->x = x;
            resolution->y = y;
            resolution->units = units;
        }
        return;
    }

    if (strcmp(name, "printer-resolution-default") == 0 && value_len == 9U) {
        caps->resolution_default.x = ap_caps_get_u32(value);
        caps->resolution_default.y = ap_caps_get_u32(value + 4U);
        caps->resolution_default.units = value[8];
        return;
    }

    if (strcmp(name, "media-supported") == 0) {
        if (caps->media_count < AP_CAPS_MAX_MEDIA) {
            char media[AP_CAPS_MEDIA_LEN];
            ap_caps_copy_string(media, sizeof(media), value, value_len);
            /* custom_min/custom_max are range bounds, not selectable paper. */
            if (!ap_caps_is_custom_bound(media) &&
                !ap_caps_has_media(caps->media, caps->media_count, media)) {
                snprintf(caps->media[caps->media_count],
                         sizeof(caps->media[caps->media_count]),
                         "%s", media);
                ++caps->media_count;
            }
        }
        return;
    }

    if (strcmp(name, "media-default") == 0) {
        ap_caps_copy_string(caps->media_default, sizeof(caps->media_default), value, value_len);
        return;
    }

    if (strcmp(name, "media-source-supported") == 0) {
        if (caps->media_source_count < AP_CAPS_MAX_MEDIA_SOURCES) {
            char source[AP_CAPS_SOURCE_LEN];
            ap_caps_copy_string(source, sizeof(source), value, value_len);
            if (!ap_caps_has_media(caps->media_sources,
                                   caps->media_source_count, source)) {
                snprintf(caps->media_sources[caps->media_source_count],
                         sizeof(caps->media_sources[caps->media_source_count]),
                         "%s", source);
                ++caps->media_source_count;
            }
        }
        return;
    }

    if (strcmp(name, "media-source-default") == 0) {
        ap_caps_copy_string(caps->media_source_default,
                            sizeof(caps->media_source_default), value, value_len);
        return;
    }

    if (strcmp(name, "marker-names") == 0) {
        if (caps->marker_name_count < AP_CAPS_MAX_MARKERS) {
            ap_caps_copy_string(caps->marker_names[caps->marker_name_count],
                                sizeof(caps->marker_names[0]),
                                value,
                                value_len);
            ++caps->marker_name_count;
        }
        return;
    }

    if (strcmp(name, "marker-levels") == 0 && value_len == 4U) {
        if (caps->marker_level_count < AP_CAPS_MAX_MARKERS) {
            caps->marker_levels[caps->marker_level_count++] =
                (int)ap_caps_get_u32(value);
        }
        return;
    }

    (void)value_tag;
}

void ap_caps_reset(struct APPrinterCapabilities *caps)
{
    unsigned int i;

    if (caps == NULL) {
        return;
    }

    memset(caps, 0, sizeof(*caps));
    caps->accepting_jobs = -1;
    caps->printer_state = 0U;
    caps->quality_default = 0U;
    caps->orientation_default = 3U;
    caps->landscape_orientation_preferred = 0U;

    for (i = 0U; i < AP_CAPS_MAX_MARKERS; ++i) {
        caps->marker_levels[i] = -1;
    }
}

static void ap_caps_set_error(char *error_text, size_t error_text_size, const char *message)
{
    if (error_text == NULL || error_text_size == 0U) {
        return;
    }

    if (message == NULL) {
        message = "Unknown error";
    }

    snprintf(error_text, error_text_size, "%s", message);
}

int ap_caps_query(
    const char *host,
    uint16_t port,
    const char *path,
    struct APPrinterCapabilities *caps,
    char *error_text,
    size_t error_text_size)
{
    static uint8_t request[AP_IPP_MAX_REQUEST];
    char printer_uri[384];
    size_t i;
    int prefer_no_expect;
    int expect_reject_status;

    if (host == NULL || host[0] == '\0' ||
        path == NULL || path[0] != '/' ||
        caps == NULL) {
        ap_caps_set_error(error_text, error_text_size, "Invalid printer address or IPP path");
        return 0;
    }

    if (snprintf(printer_uri, sizeof(printer_uri), "ipp://%s:%u%s",
                 host, (unsigned int)port, path) >= (int)sizeof(printer_uri)) {
        ap_caps_set_error(error_text, error_text_size, "Printer URI is too long");
        return 0;
    }

    prefer_no_expect = 0;
    expect_reject_status = 0;

    for (i = 0U; i < sizeof(g_versions) / sizeof(g_versions[0]); ++i) {
        size_t request_size;
        uint8_t *response;
        size_t response_size;
        int http_status;
        uint16_t ipp_status;
        uint32_t request_id;

        response = NULL;
        response_size = 0U;
        http_status = 0;

        if (!ap_ipp_build_get_printer_attributes(
                printer_uri,
                g_versions[i].major,
                g_versions[i].minor,
                1U,
                request,
                sizeof(request),
                &request_size)) {
            ap_caps_set_error(error_text, error_text_size, "Could not build IPP request");
            return 0;
        }

        if (prefer_no_expect) {
            if (!ap_http_post_ipp_no_expect(host,
                                            port,
                                            path,
                                            request,
                                            request_size,
                                            &response,
                                            &response_size,
                                            &http_status)) {
                ap_caps_set_error(error_text, error_text_size, ap_http_last_error());
                return 0;
            }
        } else {
            if (!ap_http_post_ipp(host,
                                  port,
                                  path,
                                  request,
                                  request_size,
                                  &response,
                                  &response_size,
                                  &http_status)) {
                /*
                 * A transport failure is independent of the requested IPP
                 * protocol version. Retrying 2.0 -> 1.1 -> 1.0 would only repeat
                 * the same TCP timeout for an offline printer and can freeze the
                 * synchronous Prefs query for three timeout periods.
                 *
                 * Version fallback is still retained for actual HTTP/IPP
                 * responses below, where the printer has answered and the
                 * requested IPP version can genuinely matter.
                 */
                ap_caps_set_error(error_text, error_text_size, ap_http_last_error());
                return 0;
            }

            /*
             * Some embedded printer HTTP servers reject Expect: 100-continue
             * with 417 or even the generic 500 Internal Server Error.  This is
             * safe to retry here because Get-Printer-Attributes has no printing
             * side effect.  Print-Job continues to use the normal Expect path.
             * Once detected, keep compatibility mode for IPP version fallback.
             */
            if (http_status == 417 || http_status == 500) {
                if (expect_reject_status == 0) expect_reject_status = http_status;
                ap_http_free(response);
                response = NULL;
                response_size = 0U;
                http_status = 0;
                prefer_no_expect = 1;

                if (!ap_http_post_ipp_no_expect(host,
                                                port,
                                                path,
                                                request,
                                                request_size,
                                                &response,
                                                &response_size,
                                                &http_status)) {
                    ap_caps_set_error(error_text, error_text_size, ap_http_last_error());
                    return 0;
                }
            }
        }

        if (http_status != 200) {
            char temp[96];
            snprintf(temp, sizeof(temp), "HTTP status %d", http_status);
            ap_caps_set_error(error_text, error_text_size, temp);
            ap_http_free(response);
            continue;
        }

        if (!ap_ipp_parse_response(response,
                                   response_size,
                                   &ipp_status,
                                   &request_id,
                                   NULL,
                                   NULL)) {
            ap_caps_set_error(error_text, error_text_size, "Invalid or incomplete IPP response");
            ap_http_free(response);
            continue;
        }

        if (ipp_status >= 0x0400U) {
            char temp[96];
            snprintf(temp, sizeof(temp), "IPP status 0x%04X", (unsigned int)ipp_status);
            ap_caps_set_error(error_text, error_text_size, temp);
            ap_http_free(response);
            continue;
        }

        ap_caps_reset(caps);
        caps->ipp_version_major = g_versions[i].major;
        caps->ipp_version_minor = g_versions[i].minor;
        caps->http_no_expect_required = prefer_no_expect;
        caps->http_expect_reject_status = (unsigned int)expect_reject_status;
        snprintf(caps->resolved_path, sizeof(caps->resolved_path), "%s", path);

        if (!ap_ipp_parse_response(response,
                                   response_size,
                                   &ipp_status,
                                   &request_id,
                                   ap_caps_attribute,
                                   caps)) {
            ap_caps_set_error(error_text, error_text_size, "Could not parse printer attributes");
            ap_http_free(response);
            return 0;
        }

        ap_http_free(response);
        if (error_text != NULL && error_text_size != 0U) {
            error_text[0] = '\0';
        }
        return 1;
    }

    return 0;
}

const char *ap_caps_state_text(const struct APPrinterCapabilities *caps)
{
    if (caps == NULL) {
        return "Unknown";
    }

    switch (caps->printer_state) {
        case 3U: return "Ready";
        case 4U: return "Printing";
        case 5U: return "Stopped";
        default: return "Unknown";
    }
}

const char *ap_caps_media_friendly(const char *keyword)
{
    if (keyword == NULL) return "";
    if (strcmp(keyword, "iso_a4_210x297mm") == 0) return "A4";
    if (strcmp(keyword, "iso_a5_148x210mm") == 0) return "A5";
    if (strcmp(keyword, "jis_b5_182x257mm") == 0) return "B5 (JIS)";
    if (strcmp(keyword, "na_letter_8.5x11in") == 0) return "Letter";
    if (strcmp(keyword, "na_legal_8.5x14in") == 0) return "Legal";
    if (strcmp(keyword, "na_index-4x6_4x6in") == 0) return "Photo 4 x 6 in";
    if (strcmp(keyword, "na_5x7_5x7in") == 0) return "Photo 5 x 7 in";
    if (strcmp(keyword, "oe_photo-l_3.5x5in") == 0) return "Photo L 3.5 x 5 in";
    if (strcmp(keyword, "oe_square-photo_5x5in") == 0) return "Square 5 x 5 in";
    return keyword;
}
