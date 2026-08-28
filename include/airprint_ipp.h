#ifndef AIRPRINT_IPP_H
#define AIRPRINT_IPP_H

#include <stddef.h>
#include <stdint.h>

#define AP_IPP_MAX_REQUEST 2048

#define AP_IPP_STATUS_OK 0x0000

#define AP_IPP_TAG_OPERATION_ATTRIBUTES 0x01
#define AP_IPP_TAG_JOB_ATTRIBUTES       0x02
#define AP_IPP_TAG_END_OF_ATTRIBUTES    0x03
#define AP_IPP_TAG_PRINTER_ATTRIBUTES   0x04
#define AP_IPP_TAG_UNSUPPORTED          0x05

#define AP_IPP_TAG_INTEGER              0x21
#define AP_IPP_TAG_BOOLEAN              0x22
#define AP_IPP_TAG_ENUM                 0x23
#define AP_IPP_TAG_OCTET_STRING         0x30
#define AP_IPP_TAG_DATETIME             0x31
#define AP_IPP_TAG_RESOLUTION           0x32
#define AP_IPP_TAG_RANGE                0x33
#define AP_IPP_TAG_BEGIN_COLLECTION     0x34
#define AP_IPP_TAG_TEXT_WITH_LANGUAGE   0x35
#define AP_IPP_TAG_NAME_WITH_LANGUAGE   0x36
#define AP_IPP_TAG_END_COLLECTION       0x37
#define AP_IPP_TAG_TEXT                 0x41
#define AP_IPP_TAG_NAME                 0x42
#define AP_IPP_TAG_KEYWORD              0x44
#define AP_IPP_TAG_URI                  0x45
#define AP_IPP_TAG_URI_SCHEME           0x46
#define AP_IPP_TAG_CHARSET              0x47
#define AP_IPP_TAG_NATURAL_LANGUAGE     0x48
#define AP_IPP_TAG_MIME_MEDIA_TYPE      0x49
#define AP_IPP_TAG_MEMBER_ATTR_NAME     0x4A

typedef void (*APIPPAttributeCallback)(
    uint8_t group_tag,
    const char *name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    void *user_data);

int ap_ipp_build_get_printer_attributes(
    const char *printer_uri,
    uint8_t version_major,
    uint8_t version_minor,
    uint32_t request_id,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *request_size);

int ap_ipp_build_print_job(
    const char *printer_uri,
    uint8_t version_major,
    uint8_t version_minor,
    uint32_t request_id,
    const char *job_name,
    const char *document_format,
    const char *color_mode,
    unsigned int print_quality,
    const char *media,
    const char *media_source,
    const char *sides,
    unsigned int orientation_requested,
    uint32_t resolution_x,
    uint32_t resolution_y,
    uint8_t resolution_units,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *request_size);

int ap_ipp_parse_response(
    const uint8_t *data,
    size_t data_size,
    uint16_t *status_code,
    uint32_t *request_id,
    APIPPAttributeCallback callback,
    void *user_data);

const char *ap_ipp_group_name(uint8_t group_tag);
const char *ap_ipp_value_tag_name(uint8_t value_tag);

int ap_ipp_format_value(
    const char *attribute_name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    char *output,
    size_t output_size);

#endif
