#include "airprint_caps.h"
#include "airprint_http.h"
#include "airprint_prefs.h"
#include "ami_airprint_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_CONFIG_VERSION AMIAIRPRINT_CORE_VERSION_TEXT

static struct APPrefs g_config_prefs;
static struct APPrinterCapabilities g_config_caps;

static int ap_ci_equal(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ap_ci_prefix(const char *text, const char *prefix)
{
    unsigned char a;
    unsigned char b;

    if (text == NULL || prefix == NULL) return 0;
    while (*prefix != '\0') {
        if (*text == '\0') return 0;
        a = (unsigned char)*text++;
        b = (unsigned char)*prefix++;
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

static void ap_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) return;
    if (src == NULL) src = "";
    snprintf(dst, dst_size, "%s", src);
}

static void ap_usage(void)
{
    puts("AirPrintConfig " AP_CONFIG_VERSION " - AirPrint settings for AmigaOS 3.0+");
    puts("");
    puts("Usage:");
    puts("  AirPrintConfig SHOW");
    puts("  AirPrintConfig QUERY [USE|SAVE]");
    puts("  AirPrintConfig HOST=<ip> [PORT=<n>] [PATH=<path>] [USE|SAVE]");
    puts("  AirPrintConfig COLOR=<mode> QUALITY=<mode> MEDIA=<paper> ORIENTATION=<mode> [SCALE=<10-100>] [CENTER=<0|1>] [USE|SAVE]");
    puts("");
    puts("Options:");
    puts("  HOST=10.1.0.8        Printer IPv4 address (IP= is accepted too)");
    puts("  PORT=631             IPP TCP port");
    puts("  PATH=/ipp/print      IPP resource path");
    puts("  COLOR=COLOR          COLOR, MONO, MONOCHROME, AUTO, AUTO-MONOCHROME");
    puts("  QUALITY=NORMAL       DRAFT, NORMAL, HIGH or 3,4,5");
    puts("  MEDIA=A4             A4, A5, B5, LETTER, LEGAL or raw IPP keyword");
    puts("  ORIENTATION=PORTRAIT PORTRAIT or LANDSCAPE");
    puts("  SCALE=100            Output scale from 10 to 100 percent");
    puts("  CENTER=1             Center scaled output on paper (0 or 1)");
    puts("  QUERY                Query printer and refresh stored capabilities");
    puts("  SHOW                 Show current settings/capabilities");
    puts("  USE                  Write temporary ENV: settings");
    puts("  SAVE                 Write ENV: and ENVARC: settings");
}

static const char *ap_quality_name(unsigned int quality)
{
    switch (quality) {
        case 3U: return "draft";
        case 4U: return "normal";
        case 5U: return "high";
        default: return "unknown";
    }
}

static void ap_show(const struct APPrefs *prefs,
                    const struct APPrinterCapabilities *caps,
                    int caps_valid)
{
    unsigned int i;

    printf("Host:      %s\n", prefs->host[0] != '\0' ? prefs->host : "<not set>");
    printf("Port:      %u\n", prefs->port);
    printf("IPP path:  %s\n", prefs->path);
    printf("Color:     %s\n", prefs->color_mode);
    printf("Quality:   %s (%u)\n", ap_quality_name(prefs->quality), prefs->quality);
    printf("Media:     %s\n", prefs->media);
    printf("Orientation:%s\n", prefs->orientation);
    printf("Scale:     %u%%\n", prefs->scale_percent);
    printf("Center:    %s\n", prefs->center_on_paper ? "yes" : "no");

    if (!caps_valid) {
        puts("Capabilities: not queried/stored");
        return;
    }

    puts("");
    printf("Printer:   %s\n", caps->model[0] != '\0' ? caps->model : caps->printer_name);
    printf("Status:    %s\n", ap_caps_state_text(caps));
    printf("AirPrint:  %s\n", caps->airprint_version[0] != '\0' ? caps->airprint_version : "unknown");
    printf("IPP:       %u.%u\n", (unsigned int)caps->ipp_version_major,
           (unsigned int)caps->ipp_version_minor);
    if (caps->resolution_default.x != 0U && caps->resolution_default.y != 0U) {
        printf("Resolution:%lux%lu dpi\n",
               (unsigned long)caps->resolution_default.x,
               (unsigned long)caps->resolution_default.y);
    }
    printf("Duplex:    %s\n", caps->duplex_supported ? "yes" : "no");

    if (caps->color_mode_count != 0U) {
        printf("Colors:    ");
        for (i = 0U; i < caps->color_mode_count; ++i) {
            if (i != 0U) printf(", ");
            printf("%s", caps->color_modes[i]);
        }
        putchar('\n');
    }

    printf("Quality:   %s%s%s\n",
           caps->quality_draft ? "draft " : "",
           caps->quality_normal ? "normal " : "",
           caps->quality_high ? "high" : "");

    if (caps->media_count != 0U) {
        printf("Media:     ");
        for (i = 0U; i < caps->media_count; ++i) {
            if (i != 0U) printf(", ");
            printf("%s", ap_caps_media_friendly(caps->media[i]));
        }
        putchar('\n');
    }

    if (caps->marker_name_count != 0U && caps->marker_level_count != 0U) {
        printf("Ink:       ");
        for (i = 0U; i < caps->marker_name_count && i < caps->marker_level_count; ++i) {
            if (i != 0U) printf(", ");
            printf("%s %d%%", caps->marker_names[i], caps->marker_levels[i]);
        }
        putchar('\n');
    }
}

static int ap_color_supported(const struct APPrinterCapabilities *caps, const char *mode)
{
    unsigned int i;
    for (i = 0U; i < caps->color_mode_count; ++i) {
        if (ap_ci_equal(caps->color_modes[i], mode)) return 1;
    }
    return 0;
}

static int ap_media_supported(const struct APPrinterCapabilities *caps, const char *media)
{
    unsigned int i;
    for (i = 0U; i < caps->media_count; ++i) {
        if (ap_ci_equal(caps->media[i], media)) return 1;
    }
    return 0;
}

static int ap_quality_supported(const struct APPrinterCapabilities *caps, unsigned int quality)
{
    if (quality == 3U) return caps->quality_draft;
    if (quality == 4U) return caps->quality_normal;
    if (quality == 5U) return caps->quality_high;
    return 0;
}

static const char *ap_media_keyword(const char *value)
{
    if (ap_ci_equal(value, "A4")) return "iso_a4_210x297mm";
    if (ap_ci_equal(value, "A5")) return "iso_a5_148x210mm";
    if (ap_ci_equal(value, "B5")) return "jis_b5_182x257mm";
    if (ap_ci_equal(value, "LETTER")) return "na_letter_8.5x11in";
    if (ap_ci_equal(value, "LEGAL")) return "na_legal_8.5x14in";
    if (ap_ci_equal(value, "4X6")) return "na_index-4x6_4x6in";
    if (ap_ci_equal(value, "5X7")) return "na_5x7_5x7in";
    return value;
}

static int ap_parse_quality(const char *value, unsigned int *quality)
{
    char *end;
    unsigned long number;

    if (ap_ci_equal(value, "DRAFT")) { *quality = 3U; return 1; }
    if (ap_ci_equal(value, "NORMAL")) { *quality = 4U; return 1; }
    if (ap_ci_equal(value, "HIGH")) { *quality = 5U; return 1; }

    end = NULL;
    number = strtoul(value, &end, 10);
    if (end != value && end != NULL && *end == '\0' && number >= 3UL && number <= 5UL) {
        *quality = (unsigned int)number;
        return 1;
    }
    return 0;
}

static const char *ap_color_keyword(const char *value)
{
    if (ap_ci_equal(value, "COLOR")) return "color";
    if (ap_ci_equal(value, "MONO") || ap_ci_equal(value, "MONOCHROME")) return "monochrome";
    if (ap_ci_equal(value, "AUTO")) return "auto";
    if (ap_ci_equal(value, "AUTO-MONOCHROME") || ap_ci_equal(value, "AUTOMONO")) return "auto-monochrome";
    return value;
}

int main(int argc, char **argv)
{
    int caps_valid;
    int changed;
    int want_query;
    int want_show;
    int want_use;
    int want_save;
    int i;

    ap_prefs_load(&g_config_prefs, &g_config_caps, &caps_valid);

    changed = 0;
    want_query = 0;
    want_show = 0;
    want_use = 0;
    want_save = 0;

    if (argc <= 1) {
        ap_usage();
        puts("");
        ap_show(&g_config_prefs, &g_config_caps, caps_valid);
        return 0;
    }

    for (i = 1; i < argc; ++i) {
        const char *arg;
        const char *value;
        const char *equals;

        arg = argv[i];
        if (ap_ci_equal(arg, "SHOW")) { want_show = 1; continue; }
        if (ap_ci_equal(arg, "QUERY")) { want_query = 1; continue; }
        if (ap_ci_equal(arg, "USE")) { want_use = 1; continue; }
        if (ap_ci_equal(arg, "SAVE")) { want_save = 1; continue; }
        if (ap_ci_equal(arg, "HELP") || ap_ci_equal(arg, "?") || ap_ci_equal(arg, "-H") || ap_ci_equal(arg, "--HELP")) {
            ap_usage();
            return 0;
        }

        equals = strchr(arg, '=');
        if (equals == NULL) {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return 20;
        }
        value = equals + 1;

        if (ap_ci_prefix(arg, "HOST=") || ap_ci_prefix(arg, "IP=")) {
            if (*value == '\0' || strlen(value) >= sizeof(g_config_prefs.host)) {
                fprintf(stderr, "Invalid HOST/IP value\n");
                return 20;
            }
            ap_copy(g_config_prefs.host, sizeof(g_config_prefs.host), value);
            caps_valid = 0;
            changed = 1;
        } else if (ap_ci_prefix(arg, "PORT=")) {
            char *end;
            unsigned long port;
            end = NULL;
            port = strtoul(value, &end, 10);
            if (end == value || (end != NULL && *end != '\0') || port < 1UL || port > 65535UL) {
                fprintf(stderr, "Invalid PORT value\n");
                return 20;
            }
            g_config_prefs.port = (unsigned int)port;
            caps_valid = 0;
            changed = 1;
        } else if (ap_ci_prefix(arg, "PATH=")) {
            if (*value != '/' || strlen(value) >= sizeof(g_config_prefs.path)) {
                fprintf(stderr, "PATH must start with /\n");
                return 20;
            }
            ap_copy(g_config_prefs.path, sizeof(g_config_prefs.path), value);
            caps_valid = 0;
            changed = 1;
        } else if (ap_ci_prefix(arg, "COLOR=")) {
            const char *keyword;
            keyword = ap_color_keyword(value);
            if (strlen(keyword) >= sizeof(g_config_prefs.color_mode)) {
                fprintf(stderr, "COLOR value is too long\n");
                return 20;
            }
            ap_copy(g_config_prefs.color_mode, sizeof(g_config_prefs.color_mode), keyword);
            changed = 1;
        } else if (ap_ci_prefix(arg, "ORIENTATION=")) {
            if (ap_ci_equal(value, "PORTRAIT")) {
                ap_copy(g_config_prefs.orientation, sizeof(g_config_prefs.orientation), "portrait");
            } else if (ap_ci_equal(value, "LANDSCAPE")) {
                ap_copy(g_config_prefs.orientation, sizeof(g_config_prefs.orientation), "landscape");
            } else {
                fprintf(stderr, "Invalid ORIENTATION value\n");
                return 20;
            }
            changed = 1;
        } else if (ap_ci_prefix(arg, "SCALE=")) {
            char *end;
            unsigned long scale;
            end = NULL;
            scale = strtoul(value, &end, 10);
            if (end == value || (end != NULL && *end != '\0') || scale < 10UL || scale > 100UL) {
                fprintf(stderr, "SCALE must be 10-100\n");
                return 20;
            }
            g_config_prefs.scale_percent = (unsigned int)scale;
            changed = 1;
        } else if (ap_ci_prefix(arg, "CENTER=")) {
            if (ap_ci_equal(value, "1") || ap_ci_equal(value, "YES") || ap_ci_equal(value, "ON")) {
                g_config_prefs.center_on_paper = 1;
            } else if (ap_ci_equal(value, "0") || ap_ci_equal(value, "NO") || ap_ci_equal(value, "OFF")) {
                g_config_prefs.center_on_paper = 0;
            } else {
                fprintf(stderr, "CENTER must be 0/1, YES/NO or ON/OFF\n");
                return 20;
            }
            changed = 1;
        } else if (ap_ci_prefix(arg, "QUALITY=")) {
            if (!ap_parse_quality(value, &g_config_prefs.quality)) {
                fprintf(stderr, "QUALITY must be DRAFT, NORMAL, HIGH or 3-5\n");
                return 20;
            }
            changed = 1;
        } else if (ap_ci_prefix(arg, "MEDIA=")) {
            const char *keyword;
            keyword = ap_media_keyword(value);
            if (strlen(keyword) >= sizeof(g_config_prefs.media)) {
                fprintf(stderr, "MEDIA value is too long\n");
                return 20;
            }
            ap_copy(g_config_prefs.media, sizeof(g_config_prefs.media), keyword);
            changed = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return 20;
        }
    }

    if (want_query) {
        char error_text[192];

        if (g_config_prefs.host[0] == '\0') {
            fprintf(stderr, "Set HOST=<printer IP> before QUERY\n");
            return 20;
        }

        printf("Querying ipp://%s:%u%s ...\n", g_config_prefs.host, g_config_prefs.port, g_config_prefs.path);
        if (!ap_http_open()) {
            fprintf(stderr, "%s\n", ap_http_last_error());
            return 20;
        }
        if (!ap_caps_query(g_config_prefs.host, (uint16_t)g_config_prefs.port, g_config_prefs.path,
                           &g_config_caps, error_text, sizeof(error_text))) {
            ap_http_close();
            fprintf(stderr, "Query failed: %s\n", error_text);
            return 20;
        }
        ap_http_close();
        caps_valid = 1;
        if (g_config_caps.resolved_path[0] != '\0') {
            ap_copy(g_config_prefs.path, sizeof(g_config_prefs.path), g_config_caps.resolved_path);
        }
        puts("Query successful.");
    }

    if (caps_valid) {
        if (!ap_color_supported(&g_config_caps, g_config_prefs.color_mode)) {
            fprintf(stderr, "Printer does not advertise COLOR=%s\n", g_config_prefs.color_mode);
            return 20;
        }
        if (!ap_quality_supported(&g_config_caps, g_config_prefs.quality)) {
            fprintf(stderr, "Printer does not advertise QUALITY=%s\n", ap_quality_name(g_config_prefs.quality));
            return 20;
        }
        if (!ap_media_supported(&g_config_caps, g_config_prefs.media)) {
            fprintf(stderr, "Printer does not advertise MEDIA=%s\n", g_config_prefs.media);
            return 20;
        }
    }

    if (want_save) {
        if (!ap_prefs_write_env(&g_config_prefs, &g_config_caps, caps_valid) ||
            !ap_prefs_write_envarc(&g_config_prefs, &g_config_caps, caps_valid)) {
            fprintf(stderr, "Could not save AirPrint.prefs\n");
            return 20;
        }
        puts("Saved to ENV: and ENVARC:.");
    } else if (want_use) {
        if (!ap_prefs_write_env(&g_config_prefs, &g_config_caps, caps_valid)) {
            fprintf(stderr, "Could not write ENV:AirPrint.prefs\n");
            return 20;
        }
        puts("Applied to ENV:.");
    } else if (changed || want_query) {
        puts("No settings were written. Add USE or SAVE to store them.");
    }

    if (want_show || (!want_use && !want_save)) {
        puts("");
        ap_show(&g_config_prefs, &g_config_caps, caps_valid);
    }

    return 0;
}
