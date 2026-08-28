#include "airprint_prefs.h"
#include "ami_airprint_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_PREFS_ENV_PATH    "ENV:AirPrint.prefs"
#define AP_PREFS_ENVARC_PATH "ENVARC:AirPrint.prefs"

static void ap_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, dst_size, "%s", src);
}

void ap_prefs_defaults(struct APPrefs *prefs)
{
    if (prefs == NULL) {
        return;
    }

    memset(prefs, 0, sizeof(*prefs));
    prefs->port = 631U;
    ap_copy(prefs->path, sizeof(prefs->path), "/ipp/print");
    ap_copy(prefs->engine, sizeof(prefs->engine), "pwg-raster");
    ap_copy(prefs->color_mode, sizeof(prefs->color_mode), "color");
    prefs->quality = 4U;
    ap_copy(prefs->media, sizeof(prefs->media), "iso_a4_210x297mm");
    prefs->media_source[0] = '\0';
    ap_copy(prefs->sides, sizeof(prefs->sides), "one-sided");
    prefs->resolution.x = 0U;
    prefs->resolution.y = 0U;
    prefs->resolution.units = 3U;
    ap_copy(prefs->orientation, sizeof(prefs->orientation), "portrait");
    prefs->scale_percent = 100U;
    prefs->center_on_paper = 0;
}

static void ap_trim(char *text)
{
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len != 0U &&
           (text[len - 1U] == '\r' || text[len - 1U] == '\n' ||
            text[len - 1U] == ' ' || text[len - 1U] == '\t')) {
        text[--len] = '\0';
    }
}

static unsigned long ap_ulong(const char *value, unsigned long fallback)
{
    char *end;
    unsigned long result;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    end = NULL;
    result = strtoul(value, &end, 10);
    if (end == value || (end != NULL && *end != '\0')) {
        return fallback;
    }
    return result;
}

static int ap_parse_index(const char *key, const char *prefix, unsigned int *index)
{
    const char *p;
    unsigned long value;
    char *end;

    if (key == NULL || prefix == NULL || index == NULL) {
        return 0;
    }

    if (strncmp(key, prefix, strlen(prefix)) != 0) {
        return 0;
    }

    p = key + strlen(prefix);
    if (*p == '\0') {
        return 0;
    }

    end = NULL;
    value = strtoul(p, &end, 10);
    if (end == p || (end != NULL && *end != '\0')) {
        return 0;
    }

    *index = (unsigned int)value;
    return 1;
}

static void ap_parse_caps_line(
    struct APPrinterCapabilities *caps,
    int *caps_valid,
    const char *key,
    const char *value)
{
    unsigned int index;

    if (strcmp(key, "CAP_VALID") == 0) {
        *caps_valid = ap_ulong(value, 0UL) != 0UL;
    } else if (strcmp(key, "CAP_MODEL") == 0) {
        ap_copy(caps->model, sizeof(caps->model), value);
    } else if (strcmp(key, "CAP_PRINTER_NAME") == 0) {
        ap_copy(caps->printer_name, sizeof(caps->printer_name), value);
    } else if (strcmp(key, "CAP_AIRPRINT_VERSION") == 0) {
        ap_copy(caps->airprint_version, sizeof(caps->airprint_version), value);
    } else if (strcmp(key, "CAP_STATE_REASON") == 0) {
        ap_copy(caps->state_reason, sizeof(caps->state_reason), value);
    } else if (strcmp(key, "CAP_PRINTER_STATE") == 0) {
        caps->printer_state = (unsigned int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_ACCEPTING_JOBS") == 0) {
        caps->accepting_jobs = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_COLOR_SUPPORTED") == 0) {
        caps->color_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_DUPLEX_SUPPORTED") == 0) {
        caps->duplex_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_DUPLEX_LONG_EDGE") == 0) {
        caps->duplex_long_edge_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_DUPLEX_SHORT_EDGE") == 0) {
        caps->duplex_short_edge_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_SIDES_DEFAULT") == 0) {
        ap_copy(caps->sides_default, sizeof(caps->sides_default), value);
    } else if (strcmp(key, "CAP_FORMAT_PWG") == 0) {
        caps->format_pwg_raster_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_FORMAT_PDF") == 0) {
        caps->format_pdf_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_FORMAT_POSTSCRIPT") == 0) {
        caps->format_postscript_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_FORMAT_JPEG") == 0) {
        caps->format_jpeg_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_HTTP_NO_EXPECT") == 0) {
        caps->http_no_expect_required = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_HTTP_EXPECT_REJECT_STATUS") == 0) {
        caps->http_expect_reject_status = (unsigned int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_HTTP_POSTBODY_500_OK") == 0) {
        caps->http_postbody_500_ok = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_PWG_SRGB8_SUPPORTED") == 0) {
        caps->pwg_srgb8_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_PWG_SGRAY8_SUPPORTED") == 0) {
        caps->pwg_sgray8_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_PWG_SHEET_BACK") == 0) {
        ap_copy(caps->pwg_raster_sheet_back, sizeof(caps->pwg_raster_sheet_back), value);
    } else if (strcmp(key, "CAP_ORIENTATION_PORTRAIT") == 0) {
        caps->orientation_portrait_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_ORIENTATION_LANDSCAPE") == 0) {
        caps->orientation_landscape_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_ORIENTATION_REVERSE_LANDSCAPE") == 0) {
        caps->orientation_reverse_landscape_supported = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_ORIENTATION_DEFAULT") == 0) {
        caps->orientation_default = (unsigned int)ap_ulong(value, 3UL);
    } else if (strcmp(key, "CAP_LANDSCAPE_ORIENTATION_PREFERRED") == 0) {
        caps->landscape_orientation_preferred = (unsigned int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_QUALITY_DRAFT") == 0) {
        caps->quality_draft = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_QUALITY_NORMAL") == 0) {
        caps->quality_normal = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_QUALITY_HIGH") == 0) {
        caps->quality_high = (int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_QUALITY_DEFAULT") == 0) {
        caps->quality_default = (unsigned int)ap_ulong(value, 0UL);
    } else if (strcmp(key, "CAP_COLOR_DEFAULT") == 0) {
        ap_copy(caps->color_default, sizeof(caps->color_default), value);
    } else if (strcmp(key, "CAP_COLOR_MODE_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_COLOR_MODES) {
            caps->color_mode_count = (unsigned int)count;
        }
    } else if (ap_parse_index(key, "CAP_COLOR_MODE_", &index)) {
        if (index < AP_CAPS_MAX_COLOR_MODES) {
            ap_copy(caps->color_modes[index], sizeof(caps->color_modes[index]), value);
            if (caps->color_mode_count <= index) {
                caps->color_mode_count = index + 1U;
            }
        }
    } else if (strcmp(key, "CAP_RESOLUTION_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_RESOLUTIONS) {
            caps->resolution_count = (unsigned int)count;
        }
    } else if (ap_parse_index(key, "CAP_RESOLUTION_", &index)) {
        unsigned long x;
        unsigned long y;
        unsigned int units;
        if (index < AP_CAPS_MAX_RESOLUTIONS &&
            sscanf(value, "%lu,%lu,%u", &x, &y, &units) == 3) {
            caps->resolutions[index].x = (uint32_t)x;
            caps->resolutions[index].y = (uint32_t)y;
            caps->resolutions[index].units = (uint8_t)units;
            if (caps->resolution_count <= index) {
                caps->resolution_count = index + 1U;
            }
        }
    } else if (strcmp(key, "CAP_RESOLUTION_DEFAULT") == 0) {
        unsigned long x;
        unsigned long y;
        unsigned int units;
        if (sscanf(value, "%lu,%lu,%u", &x, &y, &units) == 3) {
            caps->resolution_default.x = (uint32_t)x;
            caps->resolution_default.y = (uint32_t)y;
            caps->resolution_default.units = (uint8_t)units;
        }
    } else if (strcmp(key, "CAP_MEDIA_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_MEDIA) {
            caps->media_count = (unsigned int)count;
        }
    } else if (ap_parse_index(key, "CAP_MEDIA_", &index)) {
        if (index < AP_CAPS_MAX_MEDIA) {
            ap_copy(caps->media[index], sizeof(caps->media[index]), value);
            if (caps->media_count <= index) {
                caps->media_count = index + 1U;
            }
        }
    } else if (strcmp(key, "CAP_MEDIA_DEFAULT") == 0) {
        ap_copy(caps->media_default, sizeof(caps->media_default), value);
    } else if (strcmp(key, "CAP_MEDIA_SOURCE_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_MEDIA_SOURCES)
            caps->media_source_count = (unsigned int)count;
    } else if (ap_parse_index(key, "CAP_MEDIA_SOURCE_", &index)) {
        if (index < AP_CAPS_MAX_MEDIA_SOURCES) {
            ap_copy(caps->media_sources[index], sizeof(caps->media_sources[index]), value);
            if (caps->media_source_count <= index)
                caps->media_source_count = index + 1U;
        }
    } else if (strcmp(key, "CAP_MEDIA_SOURCE_DEFAULT") == 0) {
        ap_copy(caps->media_source_default, sizeof(caps->media_source_default), value);
    } else if (strcmp(key, "CAP_MARKER_NAME_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_MARKERS) {
            caps->marker_name_count = (unsigned int)count;
        }
    } else if (ap_parse_index(key, "CAP_MARKER_NAME_", &index)) {
        if (index < AP_CAPS_MAX_MARKERS) {
            ap_copy(caps->marker_names[index], sizeof(caps->marker_names[index]), value);
            if (caps->marker_name_count <= index) {
                caps->marker_name_count = index + 1U;
            }
        }
    } else if (strcmp(key, "CAP_MARKER_LEVEL_COUNT") == 0) {
        unsigned long count;
        count = ap_ulong(value, 0UL);
        if (count <= AP_CAPS_MAX_MARKERS) {
            caps->marker_level_count = (unsigned int)count;
        }
    } else if (ap_parse_index(key, "CAP_MARKER_LEVEL_", &index)) {
        if (index < AP_CAPS_MAX_MARKERS) {
            caps->marker_levels[index] = (int)strtol(value, NULL, 10);
            if (caps->marker_level_count <= index) {
                caps->marker_level_count = index + 1U;
            }
        }
    } else if (strcmp(key, "CAP_IPP_VERSION") == 0) {
        unsigned int major;
        unsigned int minor;
        if (sscanf(value, "%u,%u", &major, &minor) == 2) {
            caps->ipp_version_major = (uint8_t)major;
            caps->ipp_version_minor = (uint8_t)minor;
        }
    } else if (strcmp(key, "CAP_RESOLVED_PATH") == 0) {
        ap_copy(caps->resolved_path, sizeof(caps->resolved_path), value);
    }
}

static void ap_parse_line(
    struct APPrefs *prefs,
    struct APPrinterCapabilities *caps,
    int *caps_valid,
    char *line)
{
    char *equals;
    const char *key;
    const char *value;

    ap_trim(line);
    if (line[0] == '\0' || line[0] == '#') {
        return;
    }

    equals = strchr(line, '=');
    if (equals == NULL) {
        return;
    }

    *equals = '\0';
    key = line;
    value = equals + 1;

    if (strcmp(key, "HOST") == 0) {
        ap_copy(prefs->host, sizeof(prefs->host), value);
    } else if (strcmp(key, "PORT") == 0) {
        unsigned long port;
        port = ap_ulong(value, prefs->port);
        if (port >= 1UL && port <= 65535UL) {
            prefs->port = (unsigned int)port;
        }
    } else if (strcmp(key, "PATH") == 0) {
        ap_copy(prefs->path, sizeof(prefs->path), value);
    } else if (strcmp(key, "ENGINE") == 0) {
        if (strcmp(value, "pdf") == 0 || strcmp(value, "postscript") == 0)
            ap_copy(prefs->engine, sizeof(prefs->engine), value);
        else
            ap_copy(prefs->engine, sizeof(prefs->engine), "pwg-raster");
    } else if (strcmp(key, "COLOR") == 0) {
        ap_copy(prefs->color_mode, sizeof(prefs->color_mode), value);
    } else if (strcmp(key, "QUALITY") == 0) {
        unsigned long quality;
        quality = ap_ulong(value, prefs->quality);
        if (quality >= 3UL && quality <= 5UL) {
            prefs->quality = (unsigned int)quality;
        }
    } else if (strcmp(key, "MEDIA") == 0) {
        ap_copy(prefs->media, sizeof(prefs->media), value);
    } else if (strcmp(key, "MEDIA_SOURCE") == 0) {
        ap_copy(prefs->media_source, sizeof(prefs->media_source), value);
    } else if (strcmp(key, "SIDES") == 0) {
        if (strcmp(value, "two-sided-long-edge") == 0 ||
            strcmp(value, "two-sided-short-edge") == 0)
            ap_copy(prefs->sides, sizeof(prefs->sides), value);
        else
            ap_copy(prefs->sides, sizeof(prefs->sides), "one-sided");
    } else if (strcmp(key, "RESOLUTION") == 0) {
        unsigned long x;
        unsigned long y;
        unsigned int units;
        if (sscanf(value, "%lu,%lu,%u", &x, &y, &units) == 3 &&
            x > 0UL && y > 0UL && x <= 65535UL && y <= 65535UL) {
            prefs->resolution.x = (uint32_t)x;
            prefs->resolution.y = (uint32_t)y;
            prefs->resolution.units = (uint8_t)units;
        }
    } else if (strcmp(key, "ORIENTATION") == 0) {
        if (strcmp(value, "landscape") == 0)
            ap_copy(prefs->orientation, sizeof(prefs->orientation), "landscape");
        else
            ap_copy(prefs->orientation, sizeof(prefs->orientation), "portrait");
    } else if (strcmp(key, "SCALE") == 0) {
        unsigned long scale;
        scale = ap_ulong(value, prefs->scale_percent);
        if (scale >= 10UL && scale <= 100UL) {
            prefs->scale_percent = (unsigned int)scale;
        }
    } else if (strcmp(key, "CENTER_ON_PAPER") == 0) {
        prefs->center_on_paper = ap_ulong(value, 0UL) != 0UL;
    } else if (strncmp(key, "CAP_", 4U) == 0) {
        ap_parse_caps_line(caps, caps_valid, key, value);
    }
}

static int ap_load_file(
    const char *path,
    struct APPrefs *prefs,
    struct APPrinterCapabilities *caps,
    int *caps_valid)
{
    FILE *file;
    char line[256];

    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        ap_parse_line(prefs, caps, caps_valid, line);
    }

    fclose(file);
    return 1;
}

int ap_prefs_load(
    struct APPrefs *prefs,
    struct APPrinterCapabilities *caps,
    int *caps_valid)
{
    int loaded;

    if (prefs == NULL || caps == NULL || caps_valid == NULL) {
        return 0;
    }

    ap_prefs_defaults(prefs);
    ap_caps_reset(caps);
    *caps_valid = 0;

    loaded = ap_load_file(AP_PREFS_ENVARC_PATH, prefs, caps, caps_valid);
    if (ap_load_file(AP_PREFS_ENV_PATH, prefs, caps, caps_valid)) {
        loaded = 1;
    }

    return loaded;
}

static int ap_write_caps(FILE *file, const struct APPrinterCapabilities *caps)
{
    unsigned int i;

    if (fprintf(file,
                "CAP_VALID=1\n"
                "CAP_MODEL=%s\n"
                "CAP_PRINTER_NAME=%s\n"
                "CAP_AIRPRINT_VERSION=%s\n"
                "CAP_STATE_REASON=%s\n"
                "CAP_PRINTER_STATE=%u\n"
                "CAP_ACCEPTING_JOBS=%d\n"
                "CAP_COLOR_SUPPORTED=%d\n"
                "CAP_DUPLEX_SUPPORTED=%d\n"
                "CAP_DUPLEX_LONG_EDGE=%d\n"
                "CAP_DUPLEX_SHORT_EDGE=%d\n"
                "CAP_SIDES_DEFAULT=%s\n"
                "CAP_FORMAT_PWG=%d\n"
                "CAP_FORMAT_PDF=%d\n"
                "CAP_FORMAT_POSTSCRIPT=%d\n"
                "CAP_FORMAT_JPEG=%d\n"
                "CAP_HTTP_NO_EXPECT=%d\n"
                "CAP_HTTP_EXPECT_REJECT_STATUS=%u\n"
                "CAP_HTTP_POSTBODY_500_OK=%d\n"
                "CAP_PWG_SRGB8_SUPPORTED=%d\n"
                "CAP_PWG_SGRAY8_SUPPORTED=%d\n"
                "CAP_PWG_SHEET_BACK=%s\n"
                "CAP_ORIENTATION_PORTRAIT=%d\n"
                "CAP_ORIENTATION_LANDSCAPE=%d\n"
                "CAP_ORIENTATION_REVERSE_LANDSCAPE=%d\n"
                "CAP_ORIENTATION_DEFAULT=%u\n"
                "CAP_LANDSCAPE_ORIENTATION_PREFERRED=%u\n"
                "CAP_QUALITY_DRAFT=%d\n"
                "CAP_QUALITY_NORMAL=%d\n"
                "CAP_QUALITY_HIGH=%d\n"
                "CAP_QUALITY_DEFAULT=%u\n"
                "CAP_COLOR_DEFAULT=%s\n"
                "CAP_COLOR_MODE_COUNT=%u\n",
                caps->model,
                caps->printer_name,
                caps->airprint_version,
                caps->state_reason,
                caps->printer_state,
                caps->accepting_jobs,
                caps->color_supported,
                caps->duplex_supported,
                caps->duplex_long_edge_supported,
                caps->duplex_short_edge_supported,
                caps->sides_default,
                caps->format_pwg_raster_supported,
                caps->format_pdf_supported,
                caps->format_postscript_supported,
                caps->format_jpeg_supported,
                caps->http_no_expect_required,
                caps->http_expect_reject_status,
                caps->http_postbody_500_ok,
                caps->pwg_srgb8_supported,
                caps->pwg_sgray8_supported,
                caps->pwg_raster_sheet_back,
                caps->orientation_portrait_supported,
                caps->orientation_landscape_supported,
                caps->orientation_reverse_landscape_supported,
                caps->orientation_default,
                caps->landscape_orientation_preferred,
                caps->quality_draft,
                caps->quality_normal,
                caps->quality_high,
                caps->quality_default,
                caps->color_default,
                caps->color_mode_count) < 0) {
        return 0;
    }

    for (i = 0U; i < caps->color_mode_count && i < AP_CAPS_MAX_COLOR_MODES; ++i) {
        if (fprintf(file, "CAP_COLOR_MODE_%u=%s\n", i, caps->color_modes[i]) < 0) {
            return 0;
        }
    }

    if (fprintf(file,
                "CAP_RESOLUTION_COUNT=%u\n"
                "CAP_RESOLUTION_DEFAULT=%lu,%lu,%u\n",
                caps->resolution_count,
                (unsigned long)caps->resolution_default.x,
                (unsigned long)caps->resolution_default.y,
                (unsigned int)caps->resolution_default.units) < 0) {
        return 0;
    }

    for (i = 0U; i < caps->resolution_count && i < AP_CAPS_MAX_RESOLUTIONS; ++i) {
        if (fprintf(file,
                    "CAP_RESOLUTION_%u=%lu,%lu,%u\n",
                    i,
                    (unsigned long)caps->resolutions[i].x,
                    (unsigned long)caps->resolutions[i].y,
                    (unsigned int)caps->resolutions[i].units) < 0) {
            return 0;
        }
    }

    if (fprintf(file,
                "CAP_MEDIA_COUNT=%u\n"
                "CAP_MEDIA_DEFAULT=%s\n",
                caps->media_count,
                caps->media_default) < 0) {
        return 0;
    }

    for (i = 0U; i < caps->media_count && i < AP_CAPS_MAX_MEDIA; ++i) {
        if (fprintf(file, "CAP_MEDIA_%u=%s\n", i, caps->media[i]) < 0) {
            return 0;
        }
    }

    if (fprintf(file,
                "CAP_MEDIA_SOURCE_COUNT=%u\n"
                "CAP_MEDIA_SOURCE_DEFAULT=%s\n",
                caps->media_source_count,
                caps->media_source_default) < 0) {
        return 0;
    }

    for (i = 0U; i < caps->media_source_count && i < AP_CAPS_MAX_MEDIA_SOURCES; ++i) {
        if (fprintf(file, "CAP_MEDIA_SOURCE_%u=%s\n", i, caps->media_sources[i]) < 0)
            return 0;
    }

    if (fprintf(file,
                "CAP_MARKER_NAME_COUNT=%u\n"
                "CAP_MARKER_LEVEL_COUNT=%u\n",
                caps->marker_name_count,
                caps->marker_level_count) < 0) {
        return 0;
    }

    for (i = 0U; i < caps->marker_name_count && i < AP_CAPS_MAX_MARKERS; ++i) {
        if (fprintf(file, "CAP_MARKER_NAME_%u=%s\n", i, caps->marker_names[i]) < 0) {
            return 0;
        }
    }

    for (i = 0U; i < caps->marker_level_count && i < AP_CAPS_MAX_MARKERS; ++i) {
        if (fprintf(file, "CAP_MARKER_LEVEL_%u=%d\n", i, caps->marker_levels[i]) < 0) {
            return 0;
        }
    }

    return fprintf(file,
                   "CAP_IPP_VERSION=%u,%u\n"
                   "CAP_RESOLVED_PATH=%s\n",
                   (unsigned int)caps->ipp_version_major,
                   (unsigned int)caps->ipp_version_minor,
                   caps->resolved_path) >= 0;
}

static int ap_write_file(
    const char *path,
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid)
{
    FILE *file;
    int ok;

    file = fopen(path, "w");
    if (file == NULL) {
        return 0;
    }

    ok = fprintf(file,
                 "# AmiAirPrint settings " AMIAIRPRINT_VERSION_TEXT "\n"
                 "HOST=%s\n"
                 "PORT=%u\n"
                 "PATH=%s\n"
                 "ENGINE=%s\n"
                 "COLOR=%s\n"
                 "QUALITY=%u\n"
                 "MEDIA=%s\n"
                 "MEDIA_SOURCE=%s\n"
                 "SIDES=%s\n"
                 "RESOLUTION=%lu,%lu,%u\n"
                 "ORIENTATION=%s\n"
                 "SCALE=%u\n"
                 "CENTER_ON_PAPER=%u\n",
                 prefs->host,
                 prefs->port,
                 prefs->path,
                 prefs->engine,
                 prefs->color_mode,
                 prefs->quality,
                 prefs->media,
                 prefs->media_source,
                 prefs->sides,
                 (unsigned long)prefs->resolution.x,
                 (unsigned long)prefs->resolution.y,
                 (unsigned int)prefs->resolution.units,
                 prefs->orientation,
                 prefs->scale_percent,
                 prefs->center_on_paper ? 1U : 0U) >= 0;

    if (ok) {
        if (caps_valid && caps != NULL) {
            ok = ap_write_caps(file, caps);
        } else {
            ok = fprintf(file, "CAP_VALID=0\n") >= 0;
        }
    }

    if (fclose(file) != 0) {
        ok = 0;
    }

    return ok;
}

int ap_prefs_write_env(
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid)
{
    return prefs != NULL &&
           ap_write_file(AP_PREFS_ENV_PATH, prefs, caps, caps_valid);
}

int ap_prefs_write_envarc(
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid)
{
    return prefs != NULL &&
           ap_write_file(AP_PREFS_ENVARC_PATH, prefs, caps, caps_valid);
}
