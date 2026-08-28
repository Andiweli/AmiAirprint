#include "airprint_print.h"
#include "airprint_http.h"
#include "airprint_ipp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_print_ipp_request[AP_IPP_MAX_REQUEST];

static uint32_t ap_print_get_u32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) |
           ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) |
           (uint32_t)value[3];
}

static void ap_print_set_error(char *error_text, size_t error_text_size, const char *message)
{
    if (error_text == NULL || error_text_size == 0U) {
        return;
    }

    snprintf(error_text, error_text_size, "%s", message != NULL ? message : "Unknown error");
}

static void ap_print_copy_value(
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

static void ap_print_response_attribute(
    uint8_t group_tag,
    const char *name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    void *user_data)
{
    struct APPrintResult *result;

    (void)group_tag;

    result = (struct APPrintResult *)user_data;
    if (result == NULL || name == NULL || value == NULL) {
        return;
    }

    if (strcmp(name, "job-id") == 0 &&
        value_tag == AP_IPP_TAG_INTEGER && value_len == 4U) {
        result->job_id = (unsigned long)ap_print_get_u32(value);
    } else if (strcmp(name, "job-state") == 0 &&
               value_tag == AP_IPP_TAG_ENUM && value_len == 4U) {
        result->job_state = (unsigned long)ap_print_get_u32(value);
    } else if (strcmp(name, "job-uri") == 0 && value_tag == AP_IPP_TAG_URI) {
        ap_print_copy_value(result->job_uri, sizeof(result->job_uri), value, value_len);
    } else if (strcmp(name, "status-message") == 0 &&
               (value_tag == AP_IPP_TAG_TEXT || value_tag == AP_IPP_TAG_NAME)) {
        ap_print_copy_value(result->status_message,
                            sizeof(result->status_message),
                            value,
                            value_len);
    }
}

int ap_print_document(
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid,
    const uint8_t *document,
    size_t document_len,
    const char *document_format,
    const char *job_name,
    struct APPrintResult *result,
    char *error_text,
    size_t error_text_size)
{
    char printer_uri[384];
    size_t ipp_request_len;
    uint8_t *http_body;
    size_t http_body_len;
    uint8_t *response;
    size_t response_len;
    int http_status;
    uint16_t ipp_status;
    uint32_t response_request_id;
    uint8_t version_major;
    uint8_t version_minor;
    uint32_t resolution_x;
    uint32_t resolution_y;
    uint8_t resolution_units;
    unsigned int orientation_requested;
    const char *job_sides;
    const char *job_color_mode;
    const char *job_media;
    const char *job_media_source;
    unsigned int job_quality;
    unsigned int job_orientation;
    int raster_resolution;
    int compatibility_mode;

    if (prefs == NULL || document == NULL || document_len == 0U ||
        document_format == NULL || result == NULL) {
        ap_print_set_error(error_text, error_text_size, "Invalid print arguments");
        return 0;
    }

    memset(result, 0, sizeof(*result));

    if (prefs->host[0] == '\0' || prefs->path[0] != '/' || prefs->port == 0U) {
        ap_print_set_error(error_text, error_text_size, "Printer address is incomplete");
        return 0;
    }

    if (snprintf(printer_uri,
                 sizeof(printer_uri),
                 "ipp://%s:%u%s",
                 prefs->host,
                 prefs->port,
                 prefs->path) >= (int)sizeof(printer_uri)) {
        ap_print_set_error(error_text, error_text_size, "Printer URI is too long");
        return 0;
    }

    version_major = 1U;
    version_minor = 1U;
    resolution_x = 0U;
    resolution_y = 0U;
    resolution_units = 3U;
    orientation_requested = 3U;
    compatibility_mode = caps_valid && caps != NULL && caps->http_no_expect_required;
    if (strcmp(prefs->orientation, "landscape") == 0) {
        orientation_requested = 4U;
        if (caps_valid && caps != NULL &&
            (caps->landscape_orientation_preferred == 4U ||
             caps->landscape_orientation_preferred == 5U)) {
            orientation_requested = caps->landscape_orientation_preferred;
        }
    }

    if (caps_valid && caps != NULL) {
        if (caps->ipp_version_major != 0U) {
            version_major = caps->ipp_version_major;
            version_minor = caps->ipp_version_minor;
        }

        if (prefs->resolution.x != 0U && prefs->resolution.y != 0U) {
            resolution_x = prefs->resolution.x;
            resolution_y = prefs->resolution.y;
            resolution_units = prefs->resolution.units != 0U
                ? prefs->resolution.units : 3U;
        } else if (caps->resolution_default.x != 0U && caps->resolution_default.y != 0U) {
            resolution_x = caps->resolution_default.x;
            resolution_y = caps->resolution_default.y;
            resolution_units = caps->resolution_default.units != 0U
                ? caps->resolution_default.units : 3U;
        }
    }

    job_sides = strcmp(document_format, "image/pwg-raster") == 0 ? prefs->sides : NULL;
    job_color_mode = prefs->color_mode;
    job_quality = prefs->quality;
    job_media = prefs->media;
    job_media_source = prefs->media_source;
    job_orientation = orientation_requested;
    raster_resolution = strcmp(document_format, "image/pwg-raster") == 0;
    if (!raster_resolution) {
        resolution_x = 0U;
        resolution_y = 0U;
    }

    /*
     * Firmware that already needed the no-Expect compatibility path for
     * Get-Printer-Attributes has proven that its embedded HTTP/IPP stack is
     * non-conforming.  For that printer, make the first Print-Job as small
     * and conservative as possible instead of discovering quirks with a
     * side-effecting operation.  IPP 1.1 plus only the operation attributes
     * avoids known crashes on optional job-template attributes while the
     * document itself still carries its PWG/PDF/PostScript page geometry.
     */
    if (compatibility_mode) {
        version_major = 1U;
        version_minor = 1U;
        job_color_mode = NULL;
        job_quality = 0U;
        job_media = NULL;
        job_media_source = NULL;
        job_sides = NULL;
        job_orientation = 0U;
        resolution_x = 0U;
        resolution_y = 0U;
    }

    if (!ap_ipp_build_print_job(
            printer_uri,
            version_major,
            version_minor,
            2U,
            job_name != NULL ? job_name : "AmigaOS AirPrint Job",
            document_format,
            job_color_mode,
            job_quality,
            job_media,
            job_media_source,
            job_sides,
            job_orientation,
            resolution_x,
            resolution_y,
            resolution_units,
            g_print_ipp_request,
            sizeof(g_print_ipp_request),
            &ipp_request_len)) {
        ap_print_set_error(error_text, error_text_size, "Could not build IPP Print-Job request");
        return 0;
    }

    if (document_len > ((size_t)-1) - ipp_request_len) {
        ap_print_set_error(error_text, error_text_size, "Print job is too large");
        return 0;
    }

    http_body_len = ipp_request_len + document_len;
    http_body = (uint8_t *)malloc(http_body_len);
    if (http_body == NULL) {
        ap_print_set_error(error_text, error_text_size, "Not enough memory for print job");
        return 0;
    }

    memcpy(http_body, g_print_ipp_request, ipp_request_len);
    memcpy(http_body + ipp_request_len, document, document_len);

    response = NULL;
    response_len = 0U;
    http_status = 0;

    if (compatibility_mode) {
        if (!ap_http_post_ipp_no_expect(
                prefs->host,
                (uint16_t)prefs->port,
                prefs->path,
                http_body,
                http_body_len,
                &response,
                &response_len,
                &http_status)) {
            free(http_body);
            ap_print_set_error(error_text, error_text_size, ap_http_last_error());
            return 0;
        }
    } else if (!ap_http_post_ipp(
                   prefs->host,
                   (uint16_t)prefs->port,
                   prefs->path,
                   http_body,
                   http_body_len,
                   &response,
                   &response_len,
                   &http_status)) {
        free(http_body);
        ap_print_set_error(error_text, error_text_size, ap_http_last_error());
        return 0;
    }

    /*
     * Some embedded printer HTTP servers reject Expect: 100-continue with
     * 417 or 500.  A Print-Job may only be retried when the first request was
     * rejected during HTTP preflight, before any IPP/document body bytes were
     * sent.  This prevents duplicate print jobs if a printer returns a final
     * error only after accepting the body.
     */
    if (!compatibility_mode &&
        (http_status == 417 || http_status == 500) &&
        !ap_http_last_request_body_sent()) {
        ap_http_free(response);
        response = NULL;
        response_len = 0U;
        http_status = 0;

        if (!ap_http_post_ipp_no_expect(
                prefs->host,
                (uint16_t)prefs->port,
                prefs->path,
                http_body,
                http_body_len,
                &response,
                &response_len,
                &http_status)) {
            free(http_body);
            ap_print_set_error(error_text, error_text_size, ap_http_last_error());
            return 0;
        }
    }

    free(http_body);

    result->http_status = http_status;

    /*
     * Narrow compatibility rule for a broken embedded HTTP/IPP stack:
     *
     * - Get-Printer-Attributes previously had to fall back because this same
     *   printer returned HTTP 500 to Expect: 100-continue.
     * - The Print-Job is therefore already running in no-Expect/IPP 1.1
     *   compatibility mode.
     * - The complete IPP + document body was successfully transmitted.
     * - Only then does the printer return HTTP 500 instead of a valid IPP
     *   response.
     *
     * Hardware testing on the affected HP shows that the page is printed in
     * exactly this state.  Never retry: the printer may already have committed
     * the job.  Treat it as accepted for the caller while preserving HTTP 500
     * in APPrintResult for diagnostics.
     */
    if (http_status == 500 &&
        compatibility_mode &&
        ap_http_last_request_body_complete() &&
        caps != NULL &&
        (caps->http_expect_reject_status == 500U ||
         caps->http_postbody_500_ok)) {
        result->ipp_status = 0xFFFFU;
        result->postbody_http_500_accepted = 1;
        ap_http_free(response);
        if (error_text != NULL && error_text_size != 0U) error_text[0] = '\0';
        return 1;
    }

    if (http_status != 200) {
        char temp[96];
        snprintf(temp, sizeof(temp), "HTTP status %d", http_status);
        ap_print_set_error(error_text, error_text_size, temp);
        ap_http_free(response);
        return 0;
    }

    ipp_status = 0xFFFFU;
    response_request_id = 0U;

    if (!ap_ipp_parse_response(response,
                               response_len,
                               &ipp_status,
                               &response_request_id,
                               ap_print_response_attribute,
                               result)) {
        ap_print_set_error(error_text, error_text_size, "Invalid IPP Print-Job response");
        ap_http_free(response);
        return 0;
    }

    ap_http_free(response);

    result->ipp_status = ipp_status;
    result->request_id = response_request_id;

    /* 0x0000-0x00FF are IPP successful-* status codes. */
    if ((ipp_status & 0xFF00U) != 0U) {
        char temp[128];
        if (result->status_message[0] != '\0') {
            snprintf(temp,
                     sizeof(temp),
                     "IPP status 0x%04X: %.96s",
                     (unsigned int)ipp_status,
                     result->status_message);
        } else {
            snprintf(temp, sizeof(temp), "IPP status 0x%04X", (unsigned int)ipp_status);
        }
        ap_print_set_error(error_text, error_text_size, temp);
        return 0;
    }

    if (error_text != NULL && error_text_size != 0U) {
        error_text[0] = '\0';
    }
    return 1;
}
