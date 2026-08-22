#include "airprint_http.h"
#include "airprint_ipp.h"
#include "ami_airprint_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AIRPRINT_PROBE_VERSION AMIAIRPRINT_CORE_VERSION_TEXT
#define AP_DIAG_TEXT_MAX 512U
#define AP_DIAG_HEX_MAX   96U

struct APIPPVersion {
    uint8_t major;
    uint8_t minor;
};

static const struct APIPPVersion g_versions[] = {
    { 2U, 0U },
    { 1U, 1U },
    { 1U, 0U }
};

static uint8_t g_probe_request[AP_IPP_MAX_REQUEST];

static void print_usage(const char *program)
{
    printf("AirPrintProbe %s - IPP capability probe for AmigaOS 3.0+\n", AIRPRINT_PROBE_VERSION);
    printf("\n");
    printf("Usage:\n");
    printf("  %s <printer-ip> [ipp-path] [port]\n", program);
    printf("\n");
    printf("Examples:\n");
    printf("  %s 192.168.1.50\n", program);
    printf("  %s 192.168.1.50 /ipp/print\n", program);
    printf("  %s 192.168.1.50 /ipp/print 631\n", program);
    printf("\n");
    printf("Without ipp-path, common IPP paths are tried automatically.\n");
}

static void print_attribute(
    uint8_t group_tag,
    const char *name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    void *user_data)
{
    char formatted[512];
    const char *group_name;

    (void)user_data;

    group_name = ap_ipp_group_name(group_tag);
    if (!ap_ipp_format_value(name, value_tag, value, value_len,
                             formatted, sizeof(formatted))) {
        snprintf(formatted, sizeof(formatted), "%s", "<invalid value>");
    }

    printf("[%s] %-32s %-18s %s\n",
           group_name,
           name != NULL && name[0] != '\0' ? name : "(continued)",
           ap_ipp_value_tag_name(value_tag),
           formatted);
}

static void print_http_body_preview(const uint8_t *data, size_t data_size)
{
    size_t i;
    size_t limit;

    if (data == NULL || data_size == 0U) {
        printf("HTTP response body is empty.\n");
        return;
    }

    limit = data_size < AP_DIAG_TEXT_MAX ? data_size : AP_DIAG_TEXT_MAX;
    printf("HTTP response preview (%lu of %lu bytes):\n",
           (unsigned long)limit, (unsigned long)data_size);

    for (i = 0U; i < limit; ++i) {
        unsigned int c;
        c = (unsigned int)data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || (c >= 32U && c < 127U)) {
            putchar((int)c);
        } else {
            putchar('.');
        }
    }

    if (limit == 0U || data[limit - 1U] != '\n') {
        putchar('\n');
    }
    if (limit < data_size) {
        printf("...\n");
    }
}

static void print_hex_preview(const uint8_t *data, size_t data_size)
{
    size_t i;
    size_t limit;

    limit = data_size < AP_DIAG_HEX_MAX ? data_size : AP_DIAG_HEX_MAX;
    printf("First %lu response bytes:", (unsigned long)limit);
    for (i = 0U; i < limit; ++i) {
        if ((i & 15U) == 0U) {
            printf("\n  ");
        }
        printf("%02X ", (unsigned int)data[i]);
    }
    printf("\n");
}

static int probe_path_version(
    const char *host,
    unsigned int port,
    const char *path,
    uint8_t version_major,
    uint8_t version_minor)
{
    size_t request_size;
    char printer_uri[384];
    uint8_t *response;
    size_t response_size;
    int http_status;
    uint16_t ipp_status;
    uint32_t request_id;

    if (snprintf(printer_uri, sizeof(printer_uri), "ipp://%s:%u%s",
                 host, port, path) >= (int)sizeof(printer_uri)) {
        printf("URI is too long.\n");
        return 0;
    }

    if (!ap_ipp_build_get_printer_attributes(
            printer_uri,
            version_major,
            version_minor,
            1U,
            g_probe_request,
            sizeof(g_probe_request),
            &request_size)) {
        printf("Could not build IPP request.\n");
        return 0;
    }

    printf("\nTrying %s with IPP/%u.%u ...\n",
           printer_uri,
           (unsigned int)version_major,
           (unsigned int)version_minor);
    printf("IPP request: %lu bytes\n", (unsigned long)request_size);

    if (!ap_http_post_ipp(
            host,
            (uint16_t)port,
            path,
            g_probe_request,
            request_size,
            &response,
            &response_size,
            &http_status)) {
        printf("Network/HTTP error: %s\n", ap_http_last_error());
        return 0;
    }

    printf("HTTP status: %d\n", http_status);
    printf("HTTP body:   %lu bytes\n", (unsigned long)response_size);

    if (http_status != 200) {
        print_http_body_preview(response, response_size);
        ap_http_free(response);
        return 0;
    }

    if (!ap_ipp_parse_response(
            response,
            response_size,
            &ipp_status,
            &request_id,
            NULL,
            NULL)) {
        printf("HTTP 200 was received, but the body is not a complete IPP response.\n");
        print_hex_preview(response, response_size);
        ap_http_free(response);
        return 0;
    }

    printf("IPP status:     0x%04X\n", (unsigned int)ipp_status);
    printf("IPP request-id: %lu\n", (unsigned long)request_id);

    if (ipp_status >= 0x0400U) {
        printf("The printer rejected this IPP request.\n");
        printf("Trying another IPP version/path if available.\n");
        ap_http_free(response);
        return 0;
    }

    printf("\n--- Printer attributes ---\n");
    if (!ap_ipp_parse_response(
            response,
            response_size,
            &ipp_status,
            &request_id,
            print_attribute,
            NULL)) {
        printf("IPP attribute parser stopped on malformed data.\n");
        print_hex_preview(response, response_size);
        ap_http_free(response);
        return 0;
    }
    printf("--- End of attributes ---\n");

    ap_http_free(response);
    return 1;
}

static int probe_path(const char *host, unsigned int port, const char *path)
{
    size_t i;

    for (i = 0U; i < sizeof(g_versions) / sizeof(g_versions[0]); ++i) {
        if (probe_path_version(
                host,
                port,
                path,
                g_versions[i].major,
                g_versions[i].minor)) {
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *host;
    const char *explicit_path;
    unsigned int port;
    static const char *const common_paths[] = {
        "/ipp/print",
        "/ipp/printer",
        "/ipp",
        "/"
    };
    size_t i;
    int success;

    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 10;
    }

    host = argv[1];
    explicit_path = argc >= 3 ? argv[2] : NULL;
    port = argc >= 4 ? (unsigned int)atoi(argv[3]) : 631U;

    if (host[0] == '\0' || strlen(host) > 63U) {
        printf("Invalid printer IP.\n");
        return 10;
    }

    if (port == 0U || port > 65535U) {
        printf("Invalid TCP port.\n");
        return 10;
    }

    if (explicit_path != NULL &&
        (explicit_path[0] != '/' || strlen(explicit_path) > 255U)) {
        printf("IPP path must start with '/' and be at most 255 characters.\n");
        return 10;
    }

    if (!ap_http_open()) {
        printf("AirPrintProbe: %s\n", ap_http_last_error());
        printf("A bsdsocket.library compatible TCP/IP stack must be running.\n");
        return 20;
    }

    printf("AirPrintProbe %s\n", AIRPRINT_PROBE_VERSION);
    printf("Printer IP: %s\n", host);
    printf("TCP port:   %u\n", port);
    printf("HTTP mode:  two-stage POST + Expect: 100-continue\n");

    success = 0;
    if (explicit_path != NULL) {
        success = probe_path(host, port, explicit_path);
    } else {
        for (i = 0U; i < sizeof(common_paths) / sizeof(common_paths[0]); ++i) {
            if (probe_path(host, port, common_paths[i])) {
                success = 1;
                break;
            }
        }
    }

    ap_http_close();

    if (!success) {
        printf("\nNo working IPP endpoint was found.\n");
        printf("The diagnostics above now show whether the failure is HTTP,\n");
        printf("IPP version/status, or an invalid IPP response.\n");
        return 5;
    }

    printf("\nProbe completed successfully.\n");
    return 0;
}
