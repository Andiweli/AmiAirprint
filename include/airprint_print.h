#ifndef AIRPRINT_PRINT_H
#define AIRPRINT_PRINT_H

#include "airprint_caps.h"
#include "airprint_prefs.h"

#include <stddef.h>
#include <stdint.h>

#define AP_PRINT_JOB_URI_LEN 192
#define AP_PRINT_STATUS_TEXT_LEN 128

struct APPrintResult {
    uint16_t ipp_status;
    int http_status;
    int postbody_http_500_accepted;
    uint32_t request_id;
    unsigned long job_id;
    unsigned long job_state;
    char job_uri[AP_PRINT_JOB_URI_LEN];
    char status_message[AP_PRINT_STATUS_TEXT_LEN];
};

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
    size_t error_text_size);

#endif
