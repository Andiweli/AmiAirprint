#include "airprint_ipp.h"

#include <stdio.h>
#include <string.h>

#define AP_IPP_OP_PRINT_JOB              0x0002
#define AP_IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B

static uint16_t ap_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t ap_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int ap_put_u8(uint8_t **cursor, const uint8_t *end, uint8_t value)
{
    if (*cursor >= end) {
        return 0;
    }
    *(*cursor)++ = value;
    return 1;
}

static int ap_put_u16(uint8_t **cursor, const uint8_t *end, uint16_t value)
{
    if ((size_t)(end - *cursor) < 2U) {
        return 0;
    }
    *(*cursor)++ = (uint8_t)((value >> 8) & 0xFFU);
    *(*cursor)++ = (uint8_t)(value & 0xFFU);
    return 1;
}

static int ap_put_u32(uint8_t **cursor, const uint8_t *end, uint32_t value)
{
    if ((size_t)(end - *cursor) < 4U) {
        return 0;
    }
    *(*cursor)++ = (uint8_t)((value >> 24) & 0xFFU);
    *(*cursor)++ = (uint8_t)((value >> 16) & 0xFFU);
    *(*cursor)++ = (uint8_t)((value >> 8) & 0xFFU);
    *(*cursor)++ = (uint8_t)(value & 0xFFU);
    return 1;
}

static int ap_put_bytes(
    uint8_t **cursor,
    const uint8_t *end,
    const uint8_t *data,
    size_t data_len)
{
    if ((size_t)(end - *cursor) < data_len) {
        return 0;
    }
    if (data_len != 0U) {
        memcpy(*cursor, data, data_len);
        *cursor += data_len;
    }
    return 1;
}

static int ap_put_attribute(
    uint8_t **cursor,
    const uint8_t *end,
    uint8_t value_tag,
    const char *name,
    const char *value)
{
    size_t name_len;
    size_t value_len;

    name_len = name != NULL ? strlen(name) : 0U;
    value_len = value != NULL ? strlen(value) : 0U;

    if (name_len > 65535U || value_len > 65535U) {
        return 0;
    }

    return ap_put_u8(cursor, end, value_tag) &&
           ap_put_u16(cursor, end, (uint16_t)name_len) &&
           ap_put_bytes(cursor, end, (const uint8_t *)name, name_len) &&
           ap_put_u16(cursor, end, (uint16_t)value_len) &&
           ap_put_bytes(cursor, end, (const uint8_t *)value, value_len);
}

static int ap_put_attribute_u32(
    uint8_t **cursor,
    const uint8_t *end,
    uint8_t value_tag,
    const char *name,
    uint32_t value)
{
    size_t name_len;

    name_len = name != NULL ? strlen(name) : 0U;
    if (name_len > 65535U) {
        return 0;
    }

    return ap_put_u8(cursor, end, value_tag) &&
           ap_put_u16(cursor, end, (uint16_t)name_len) &&
           ap_put_bytes(cursor, end, (const uint8_t *)name, name_len) &&
           ap_put_u16(cursor, end, 4U) &&
           ap_put_u32(cursor, end, value);
}

static int ap_put_attribute_resolution(
    uint8_t **cursor,
    const uint8_t *end,
    const char *name,
    uint32_t x,
    uint32_t y,
    uint8_t units)
{
    size_t name_len;

    name_len = name != NULL ? strlen(name) : 0U;
    if (name_len > 65535U) {
        return 0;
    }

    return ap_put_u8(cursor, end, AP_IPP_TAG_RESOLUTION) &&
           ap_put_u16(cursor, end, (uint16_t)name_len) &&
           ap_put_bytes(cursor, end, (const uint8_t *)name, name_len) &&
           ap_put_u16(cursor, end, 9U) &&
           ap_put_u32(cursor, end, x) &&
           ap_put_u32(cursor, end, y) &&
           ap_put_u8(cursor, end, units);
}

int ap_ipp_build_get_printer_attributes(
    const char *printer_uri,
    uint8_t version_major,
    uint8_t version_minor,
    uint32_t request_id,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *request_size)
{
    uint8_t *cursor;
    const uint8_t *end;

    if (printer_uri == NULL || buffer == NULL || request_size == NULL) {
        return 0;
    }

    cursor = buffer;
    end = buffer + buffer_size;

    /*
     * Keep discovery deliberately minimal. RFC 8011 requires the charset,
     * natural-language and printer-uri operation attributes. If the optional
     * requested-attributes attribute is omitted, the printer behaves as if
     * "all" had been requested. This is also friendlier to quirky firmware.
     */
    if (!ap_put_u8(&cursor, end, version_major) ||
        !ap_put_u8(&cursor, end, version_minor) ||
        !ap_put_u16(&cursor, end, AP_IPP_OP_GET_PRINTER_ATTRIBUTES) ||
        !ap_put_u32(&cursor, end, request_id) ||
        !ap_put_u8(&cursor, end, AP_IPP_TAG_OPERATION_ATTRIBUTES) ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_CHARSET,
                          "attributes-charset", "utf-8") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_NATURAL_LANGUAGE,
                          "attributes-natural-language", "en") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_URI,
                          "printer-uri", printer_uri) ||
        !ap_put_u8(&cursor, end, AP_IPP_TAG_END_OF_ATTRIBUTES)) {
        return 0;
    }

    *request_size = (size_t)(cursor - buffer);
    return 1;
}

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
    size_t *request_size)
{
    uint8_t *cursor;
    const uint8_t *end;
    int have_job_attributes;

    if (printer_uri == NULL || document_format == NULL ||
        buffer == NULL || request_size == NULL) {
        return 0;
    }

    cursor = buffer;
    end = buffer + buffer_size;

    if (!ap_put_u8(&cursor, end, version_major) ||
        !ap_put_u8(&cursor, end, version_minor) ||
        !ap_put_u16(&cursor, end, AP_IPP_OP_PRINT_JOB) ||
        !ap_put_u32(&cursor, end, request_id) ||
        !ap_put_u8(&cursor, end, AP_IPP_TAG_OPERATION_ATTRIBUTES) ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_CHARSET,
                          "attributes-charset", "utf-8") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_NATURAL_LANGUAGE,
                          "attributes-natural-language", "en") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_URI,
                          "printer-uri", printer_uri) ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_NAME,
                          "requesting-user-name", "AmigaOS") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_NAME,
                          "job-name", job_name != NULL ? job_name : "AmigaOS AirPrint Job") ||
        !ap_put_attribute(&cursor, end, AP_IPP_TAG_MIME_MEDIA_TYPE,
                          "document-format", document_format)) {
        return 0;
    }

    have_job_attributes =
        (color_mode != NULL && color_mode[0] != '\0') ||
        (print_quality >= 3U && print_quality <= 5U) ||
        (media != NULL && media[0] != '\0') ||
        (media_source != NULL && media_source[0] != '\0') ||
        (sides != NULL && sides[0] != '\0') ||
        (orientation_requested >= 3U && orientation_requested <= 6U) ||
        (resolution_x != 0U && resolution_y != 0U);

    if (have_job_attributes) {
        if (!ap_put_u8(&cursor, end, AP_IPP_TAG_JOB_ATTRIBUTES)) {
            return 0;
        }

        if (color_mode != NULL && color_mode[0] != '\0' &&
            !ap_put_attribute(&cursor, end, AP_IPP_TAG_KEYWORD,
                              "print-color-mode", color_mode)) {
            return 0;
        }

        if (print_quality >= 3U && print_quality <= 5U &&
            !ap_put_attribute_u32(&cursor, end, AP_IPP_TAG_ENUM,
                                  "print-quality", (uint32_t)print_quality)) {
            return 0;
        }

        if (media != NULL && media[0] != '\0' &&
            !ap_put_attribute(&cursor, end, AP_IPP_TAG_KEYWORD,
                              "media", media)) {
            return 0;
        }

        if (media_source != NULL && media_source[0] != '\0' &&
            !ap_put_attribute(&cursor, end, AP_IPP_TAG_KEYWORD,
                              "media-source", media_source)) {
            return 0;
        }

        if (sides != NULL && sides[0] != '\0' &&
            !ap_put_attribute(&cursor, end, AP_IPP_TAG_KEYWORD,
                              "sides", sides)) {
            return 0;
        }

        if (orientation_requested >= 3U && orientation_requested <= 6U &&
            !ap_put_attribute_u32(&cursor, end, AP_IPP_TAG_ENUM,
                                  "orientation-requested",
                                  (uint32_t)orientation_requested)) {
            return 0;
        }

        if (resolution_x != 0U && resolution_y != 0U &&
            !ap_put_attribute_resolution(&cursor, end,
                                         "printer-resolution",
                                         resolution_x,
                                         resolution_y,
                                         resolution_units != 0U ? resolution_units : 3U)) {
            return 0;
        }
    }

    if (!ap_put_u8(&cursor, end, AP_IPP_TAG_END_OF_ATTRIBUTES)) {
        return 0;
    }

    *request_size = (size_t)(cursor - buffer);
    return 1;
}

int ap_ipp_parse_response(
    const uint8_t *data,
    size_t data_size,
    uint16_t *status_code,
    uint32_t *request_id,
    APIPPAttributeCallback callback,
    void *user_data)
{
    size_t pos;
    uint8_t group_tag;
    char current_name[256];

    if (data == NULL || data_size < 8U) {
        return 0;
    }

    if (status_code != NULL) {
        *status_code = ap_get_u16(data + 2);
    }
    if (request_id != NULL) {
        *request_id = ap_get_u32(data + 4);
    }

    pos = 8U;
    group_tag = 0U;
    current_name[0] = '\0';

    while (pos < data_size) {
        uint8_t tag;
        uint16_t name_len;
        uint16_t value_len;

        tag = data[pos++];

        if (tag == AP_IPP_TAG_END_OF_ATTRIBUTES) {
            return 1;
        }

        if (tag <= 0x0FU) {
            group_tag = tag;
            current_name[0] = '\0';
            continue;
        }

        if (pos + 2U > data_size) {
            return 0;
        }
        name_len = ap_get_u16(data + pos);
        pos += 2U;

        if (pos + (size_t)name_len + 2U > data_size) {
            return 0;
        }

        if (name_len != 0U) {
            size_t copy_len;
            copy_len = name_len;
            if (copy_len >= sizeof(current_name)) {
                copy_len = sizeof(current_name) - 1U;
            }
            memcpy(current_name, data + pos, copy_len);
            current_name[copy_len] = '\0';
        }
        pos += (size_t)name_len;

        value_len = ap_get_u16(data + pos);
        pos += 2U;

        if (pos + (size_t)value_len > data_size) {
            return 0;
        }

        if (callback != NULL) {
            callback(group_tag, current_name, tag, data + pos, value_len, user_data);
        }

        pos += (size_t)value_len;
    }

    return 0;
}

const char *ap_ipp_group_name(uint8_t group_tag)
{
    switch (group_tag) {
        case AP_IPP_TAG_OPERATION_ATTRIBUTES: return "operation";
        case AP_IPP_TAG_JOB_ATTRIBUTES: return "job";
        case AP_IPP_TAG_PRINTER_ATTRIBUTES: return "printer";
        case AP_IPP_TAG_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}

const char *ap_ipp_value_tag_name(uint8_t value_tag)
{
    switch (value_tag) {
        case AP_IPP_TAG_INTEGER: return "integer";
        case AP_IPP_TAG_BOOLEAN: return "boolean";
        case AP_IPP_TAG_ENUM: return "enum";
        case AP_IPP_TAG_OCTET_STRING: return "octetString";
        case AP_IPP_TAG_DATETIME: return "dateTime";
        case AP_IPP_TAG_RESOLUTION: return "resolution";
        case AP_IPP_TAG_RANGE: return "rangeOfInteger";
        case AP_IPP_TAG_BEGIN_COLLECTION: return "begCollection";
        case AP_IPP_TAG_TEXT_WITH_LANGUAGE: return "textWithLanguage";
        case AP_IPP_TAG_NAME_WITH_LANGUAGE: return "nameWithLanguage";
        case AP_IPP_TAG_END_COLLECTION: return "endCollection";
        case AP_IPP_TAG_TEXT: return "text";
        case AP_IPP_TAG_NAME: return "name";
        case AP_IPP_TAG_KEYWORD: return "keyword";
        case AP_IPP_TAG_URI: return "uri";
        case AP_IPP_TAG_URI_SCHEME: return "uriScheme";
        case AP_IPP_TAG_CHARSET: return "charset";
        case AP_IPP_TAG_NATURAL_LANGUAGE: return "naturalLanguage";
        case AP_IPP_TAG_MIME_MEDIA_TYPE: return "mimeMediaType";
        case AP_IPP_TAG_MEMBER_ATTR_NAME: return "memberAttrName";
        default: return "unknown";
    }
}

static int ap_format_string(
    const uint8_t *value,
    uint16_t value_len,
    char *output,
    size_t output_size)
{
    size_t i;
    size_t copy_len;

    if (output_size == 0U) {
        return 0;
    }

    copy_len = (size_t)value_len;
    if (copy_len >= output_size) {
        copy_len = output_size - 1U;
    }

    for (i = 0U; i < copy_len; ++i) {
        unsigned char c;
        c = value[i];
        output[i] = (c >= 32U && c < 127U) ? (char)c : '.';
    }
    output[copy_len] = '\0';
    return 1;
}

static const char *ap_enum_annotation(const char *attribute_name, uint32_t number)
{
    if (attribute_name != NULL && strcmp(attribute_name, "print-quality-supported") == 0) {
        if (number == 3U) return "draft";
        if (number == 4U) return "normal";
        if (number == 5U) return "high";
    }

    if (attribute_name != NULL && strcmp(attribute_name, "print-quality-default") == 0) {
        if (number == 3U) return "draft";
        if (number == 4U) return "normal";
        if (number == 5U) return "high";
    }

    if (attribute_name != NULL && strcmp(attribute_name, "printer-state") == 0) {
        if (number == 3U) return "idle";
        if (number == 4U) return "processing";
        if (number == 5U) return "stopped";
    }

    return NULL;
}

int ap_ipp_format_value(
    const char *attribute_name,
    uint8_t value_tag,
    const uint8_t *value,
    uint16_t value_len,
    char *output,
    size_t output_size)
{
    uint32_t number;
    const char *annotation;

    if (value == NULL || output == NULL || output_size == 0U) {
        return 0;
    }

    output[0] = '\0';

    switch (value_tag) {
        case AP_IPP_TAG_INTEGER:
        case AP_IPP_TAG_ENUM:
            if (value_len != 4U) return 0;
            number = ap_get_u32(value);
            annotation = value_tag == AP_IPP_TAG_ENUM
                ? ap_enum_annotation(attribute_name, number)
                : NULL;
            if (annotation != NULL) {
                snprintf(output, output_size, "%lu (%s)",
                         (unsigned long)number, annotation);
            } else {
                snprintf(output, output_size, "%lu", (unsigned long)number);
            }
            return 1;

        case AP_IPP_TAG_BOOLEAN:
            if (value_len != 1U) return 0;
            snprintf(output, output_size, "%s", value[0] != 0U ? "true" : "false");
            return 1;

        case AP_IPP_TAG_RESOLUTION:
            if (value_len != 9U) return 0;
            snprintf(output, output_size, "%lux%lu %s",
                     (unsigned long)ap_get_u32(value),
                     (unsigned long)ap_get_u32(value + 4),
                     value[8] == 3U ? "dpi" : (value[8] == 4U ? "dpcm" : "units"));
            return 1;

        case AP_IPP_TAG_RANGE:
            if (value_len != 8U) return 0;
            snprintf(output, output_size, "%lu-%lu",
                     (unsigned long)ap_get_u32(value),
                     (unsigned long)ap_get_u32(value + 4));
            return 1;

        case AP_IPP_TAG_TEXT:
        case AP_IPP_TAG_NAME:
        case AP_IPP_TAG_KEYWORD:
        case AP_IPP_TAG_URI:
        case AP_IPP_TAG_URI_SCHEME:
        case AP_IPP_TAG_CHARSET:
        case AP_IPP_TAG_NATURAL_LANGUAGE:
        case AP_IPP_TAG_MIME_MEDIA_TYPE:
        case AP_IPP_TAG_MEMBER_ATTR_NAME:
            return ap_format_string(value, value_len, output, output_size);

        case AP_IPP_TAG_BEGIN_COLLECTION:
            snprintf(output, output_size, "<collection>");
            return 1;

        case AP_IPP_TAG_END_COLLECTION:
            snprintf(output, output_size, "</collection>");
            return 1;

        default:
            snprintf(output, output_size, "<%u bytes>", (unsigned int)value_len);
            return 1;
    }
}
