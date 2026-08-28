#include "airprint_device.h"
#include "ami_airprint_brand.h"
#include "ami_airprint_version.h"

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
/*
 * The AmigaOS bsdsocket proto header pulls in sys/socket.h, whose
 * prototypes use ssize_t.  With the freestanding device build the NDK
 * does not arrange for sys/types.h to be included first, so make the
 * dependency explicit before proto/bsdsocket.h.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stddef.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>

#define STR_INNER(s) #s
#define STR(s) STR_INNER(s)

#define DEVICE_VERSION AMIAIRPRINT_CORE_VERSION_MAJOR
#define DEVICE_REVISION AMIAIRPRINT_CORE_VERSION_MINOR
#define DEVICE_PRIORITY 0
#define DEVICE_DATE "(" AMIAIRPRINT_CORE_VERSION_DATE_LONG ")"
#define DEVICE_ID_STRING "$VER: airprint.device " AMIAIRPRINT_CORE_VERSION_TEXT " " DEVICE_DATE "\r\n" AMIAIRPRINT_BRAND_TEXT

#define APD_PREFS_ENV    "ENV:AirPrint.prefs"
#define APD_PREFS_ENVARC "ENVARC:AirPrint.prefs"
#define APD_SPOOL_PATH   "T:AirPrint.spool"

#define APD_HOST_LEN 64U
#define APD_PATH_LEN 128U
#define APD_VALUE_LEN 96U
#define APD_IPP_MAX 1024U
#define APD_HTTP_MAX 1024U
#define APD_REPLY_MAX 4096U
#define APD_FILE_CHUNK 4096U

struct APDPreferences {
    char host[APD_HOST_LEN];
    UWORD port;
    char path[APD_PATH_LEN];
    char color[APD_VALUE_LEN];
    UWORD quality;
    char media[APD_VALUE_LEN];
    char media_source[APD_VALUE_LEN];
    char sides[APD_VALUE_LEN];
    UWORD resolution_x;
    UWORD resolution_y;
    UBYTE resolution_units;
    char orientation[APD_VALUE_LEN];
    UWORD landscape_orientation_preferred;
    UBYTE http_no_expect_required;
    UWORD http_expect_reject_status;
    UBYTE http_postbody_500_ok;
};

/*
 * Keep the large HTTP/IPP/file buffers off the caller stack. printer.device
 * may invoke the primitive device from a task with a small stack, so the
 * transport workspace is allocated explicitly for each submitted job.
 */
struct APDWorkspace {
    struct APDPreferences prefs;
    UBYTE ipp[APD_IPP_MAX];
    char http[APD_HTTP_MAX];
    UBYTE reply[APD_REPLY_MAX];
    UBYTE file_buffer[APD_FILE_CHUNK];
    char uri[256];
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;

static BPTR saved_seg_list;
static BOOL g_job_active;
static ULONG g_job_size;
static UWORD g_job_format;
static UWORD g_last_stage;
static LONG g_last_socket_errno;
static LONG g_last_http_status;
static UWORD g_last_ipp_status;
static ULONG g_last_bytes;

extern const ULONG auto_init_tables[4] __asm__("auto_init_tables");

int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

asm("romtag: \n"
    " dc.w " STR(RTC_MATCHWORD) " \n"
    " dc.l romtag \n"
    " dc.l endcode \n"
    " dc.b " STR(RTF_AUTOINIT) " \n"
    " dc.b " STR(DEVICE_VERSION) " \n"
    " dc.b " STR(NT_DEVICE) " \n"
    " dc.b " STR(DEVICE_PRIORITY) " \n"
    " dc.l device_name \n"
    " dc.l device_id_string \n"
    " dc.l auto_init_tables \n"
    "endcode: \n");

char device_name[] __asm__("device_name") = APDEV_NAME;
char device_id_string[] __asm__("device_id_string") = DEVICE_ID_STRING;
char device_brand_string[] __attribute__((used)) = AMIAIRPRINT_BRAND_TEXT;

static ULONG apd_strlen(const char *s)
{
    ULONG n;
    n = 0U;
    if (s == NULL) return 0U;
    while (s[n] != '\0') ++n;
    return n;
}

static void apd_copy(char *dst, ULONG dst_size, const char *src)
{
    ULONG i;
    if (dst == NULL || dst_size == 0U) return;
    if (src == NULL) src = "";
    i = 0U;
    while (i + 1U < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void apd_memzero(void *ptr, ULONG size)
{
    UBYTE *p;
    p = (UBYTE *)ptr;
    while (size-- != 0U) *p++ = 0U;
}

static int apd_streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static ULONG apd_parse_ulong(const char *text, ULONG fallback)
{
    ULONG value;
    int digits;

    if (text == NULL || *text == '\0') return fallback;
    value = 0U;
    digits = 0;
    while (*text >= '0' && *text <= '9') {
        if (value > 429496729UL) return fallback;
        value = (value << 3) + (value << 1) + (ULONG)(*text - '0');
        ++text;
        digits = 1;
    }
    if (!digits || *text != '\0') return fallback;
    return value;
}

static void apd_prefs_defaults(struct APDPreferences *prefs)
{
    apd_memzero(prefs, (ULONG)sizeof(*prefs));
    prefs->port = 631U;
    prefs->quality = 4U;
    apd_copy(prefs->path, sizeof(prefs->path), "/ipp/print");
    apd_copy(prefs->color, sizeof(prefs->color), "color");
    apd_copy(prefs->media, sizeof(prefs->media), "iso_a4_210x297mm");
    prefs->media_source[0] = '\0';
    apd_copy(prefs->sides, sizeof(prefs->sides), "one-sided");
    prefs->resolution_x = 0U;
    prefs->resolution_y = 0U;
    prefs->resolution_units = 3U;
    apd_copy(prefs->orientation, sizeof(prefs->orientation), "portrait");
    prefs->landscape_orientation_preferred = 0U;
    prefs->http_no_expect_required = 0U;
    prefs->http_expect_reject_status = 0U;
    prefs->http_postbody_500_ok = 0U;
}

static void apd_parse_pref_line(struct APDPreferences *prefs, char *line)
{
    char *value;
    char *p;
    ULONG port;
    ULONG quality;

    if (line == NULL || line[0] == '\0' || line[0] == '#') return;
    p = line;
    while (*p != '\0' && *p != '=') ++p;
    if (*p != '=') return;
    *p = '\0';
    value = p + 1;

    if (apd_streq(line, "HOST")) {
        apd_copy(prefs->host, sizeof(prefs->host), value);
    } else if (apd_streq(line, "PORT")) {
        port = apd_parse_ulong(value, prefs->port);
        if (port >= 1U && port <= 65535U) prefs->port = (UWORD)port;
    } else if (apd_streq(line, "PATH")) {
        apd_copy(prefs->path, sizeof(prefs->path), value);
    } else if (apd_streq(line, "COLOR")) {
        apd_copy(prefs->color, sizeof(prefs->color), value);
    } else if (apd_streq(line, "QUALITY")) {
        quality = apd_parse_ulong(value, prefs->quality);
        if (quality >= 3U && quality <= 5U) prefs->quality = (UWORD)quality;
    } else if (apd_streq(line, "MEDIA")) {
        apd_copy(prefs->media, sizeof(prefs->media), value);
    } else if (apd_streq(line, "MEDIA_SOURCE")) {
        apd_copy(prefs->media_source, sizeof(prefs->media_source), value);
    } else if (apd_streq(line, "SIDES")) {
        if (apd_streq(value, "two-sided-long-edge") ||
            apd_streq(value, "two-sided-short-edge"))
            apd_copy(prefs->sides, sizeof(prefs->sides), value);
        else
            apd_copy(prefs->sides, sizeof(prefs->sides), "one-sided");
    } else if (apd_streq(line, "RESOLUTION")) {
        const char *q;
        ULONG x;
        ULONG y;
        ULONG units;
        char number[16];
        ULONG n;

        q = value;
        n = 0U;
        while (*q >= '0' && *q <= '9' && n + 1U < sizeof(number)) number[n++] = *q++;
        number[n] = '\0';
        x = apd_parse_ulong(number, 0U);
        if (*q == ',') ++q; else x = 0U;
        n = 0U;
        while (*q >= '0' && *q <= '9' && n + 1U < sizeof(number)) number[n++] = *q++;
        number[n] = '\0';
        y = apd_parse_ulong(number, 0U);
        if (*q == ',') ++q; else y = 0U;
        units = apd_parse_ulong(q, 3U);
        if (x > 0U && x <= 65535U && y > 0U && y <= 65535U &&
            (units == 3U || units == 4U)) {
            prefs->resolution_x = (UWORD)x;
            prefs->resolution_y = (UWORD)y;
            prefs->resolution_units = (UBYTE)units;
        }
    } else if (apd_streq(line, "ORIENTATION")) {
        if (apd_streq(value, "landscape"))
            apd_copy(prefs->orientation, sizeof(prefs->orientation), "landscape");
        else
            apd_copy(prefs->orientation, sizeof(prefs->orientation), "portrait");
    } else if (apd_streq(line, "CAP_LANDSCAPE_ORIENTATION_PREFERRED")) {
        ULONG orientation;
        orientation = apd_parse_ulong(value, 0U);
        if (orientation == 4U || orientation == 5U)
            prefs->landscape_orientation_preferred = (UWORD)orientation;
    } else if (apd_streq(line, "CAP_HTTP_NO_EXPECT")) {
        prefs->http_no_expect_required = apd_parse_ulong(value, 0U) != 0U ? 1U : 0U;
    } else if (apd_streq(line, "CAP_HTTP_EXPECT_REJECT_STATUS")) {
        ULONG status;
        status = apd_parse_ulong(value, 0U);
        prefs->http_expect_reject_status = status <= 65535U ? (UWORD)status : 0U;
    } else if (apd_streq(line, "CAP_HTTP_POSTBODY_500_OK")) {
        prefs->http_postbody_500_ok = apd_parse_ulong(value, 0U) != 0U ? 1U : 0U;
    }
}

static int apd_load_prefs_file(CONST_STRPTR path, struct APDPreferences *prefs)
{
    BPTR file;
    char input[256];
    char line[256];
    LONG got;
    ULONG line_len;
    LONG i;

    file = Open(path, MODE_OLDFILE);
    if (file == 0) return 0;

    line_len = 0U;
    for (;;) {
        got = Read(file, input, (LONG)sizeof(input));
        if (got <= 0) break;
        for (i = 0; i < got; ++i) {
            char c;
            c = input[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                apd_parse_pref_line(prefs, line);
                line_len = 0U;
            } else if (line_len + 1U < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len != 0U) {
        line[line_len] = '\0';
        apd_parse_pref_line(prefs, line);
    }
    Close(file);
    return 1;
}

static int apd_load_prefs(struct APDPreferences *prefs)
{
    int loaded;

    apd_prefs_defaults(prefs);
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase == NULL) return 0;

    loaded = apd_load_prefs_file((CONST_STRPTR)APD_PREFS_ENVARC, prefs);
    if (apd_load_prefs_file((CONST_STRPTR)APD_PREFS_ENV, prefs)) loaded = 1;

    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    return loaded && prefs->host[0] != '\0' && prefs->path[0] == '/';
}

static int apd_spool_begin(void)
{
    BPTR file;

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase == NULL) return 0;
    file = Open((CONST_STRPTR)APD_SPOOL_PATH, MODE_NEWFILE);
    if (file == 0) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }
    Close(file);
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    g_job_size = 0U;
    g_last_stage = APDEV_STAGE_SPOOL;
    return 1;
}

static int apd_spool_append(const UBYTE *data, ULONG length)
{
    BPTR file;
    LONG wrote;

    if (data == NULL || length == 0U) return 0;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase == NULL) return 0;

    file = Open((CONST_STRPTR)APD_SPOOL_PATH, MODE_READWRITE);
    if (file == 0) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }
    if (Seek(file, 0, OFFSET_END) == -1) {
        Close(file);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }
    wrote = Write(file, (APTR)data, (LONG)length);
    Close(file);
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    if (wrote != (LONG)length) return 0;
    g_job_size += length;
    return 1;
}

static void apd_spool_delete(void)
{
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase != NULL) {
        DeleteFile((CONST_STRPTR)APD_SPOOL_PATH);
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
    g_job_active = FALSE;
    g_job_size = 0U;
    g_job_format = 0U;
}

static int apd_parse_ipv4(const char *text, struct in_addr *address)
{
    UBYTE octets[4];
    ULONG part;
    const char *p;
    UBYTE *bytes;

    if (text == NULL || address == NULL || *text == '\0') return 0;
    p = text;
    for (part = 0U; part < 4U; ++part) {
        ULONG value;
        ULONG digits;
        value = 0U;
        digits = 0U;
        while (*p >= '0' && *p <= '9') {
            value = (value << 3) + (value << 1) + (ULONG)(*p - '0');
            if (value > 255U) return 0;
            ++p;
            ++digits;
        }
        if (digits == 0U) return 0;
        octets[part] = (UBYTE)value;
        if (part != 3U) {
            if (*p != '.') return 0;
            ++p;
        } else if (*p != '\0') {
            return 0;
        }
    }
    bytes = (UBYTE *)&address->s_addr;
    bytes[0] = octets[0];
    bytes[1] = octets[1];
    bytes[2] = octets[2];
    bytes[3] = octets[3];
    return 1;
}

static int apd_send_all(int sock, const UBYTE *data, ULONG length)
{
    ULONG total;
    total = 0U;
    while (total < length) {
        LONG sent;
        sent = send(sock, (char *)(data + total), (LONG)(length - total), 0);
        if (sent <= 0) return 0;
        total += (ULONG)sent;
    }
    return 1;
}

static ULONG apd_find_header_end(const UBYTE *data, ULONG length)
{
    ULONG i;
    if (length < 4U) return 0U;
    for (i = 0U; i + 3U < length; ++i) {
        if (data[i] == '\r' && data[i + 1U] == '\n' &&
            data[i + 2U] == '\r' && data[i + 3U] == '\n') return i + 4U;
    }
    return 0U;
}

static int apd_http_status(const UBYTE *data, ULONG length)
{
    ULONG i;
    int status;
    if (data == NULL || length < 12U) return -1;
    if (!(data[0] == 'H' && data[1] == 'T' && data[2] == 'T' && data[3] == 'P')) return -1;
    i = 0U;
    while (i < length && data[i] != ' ') ++i;
    while (i < length && data[i] == ' ') ++i;
    if (i + 2U >= length) return -1;
    if (data[i] < '0' || data[i] > '9' ||
        data[i + 1U] < '0' || data[i + 1U] > '9' ||
        data[i + 2U] < '0' || data[i + 2U] > '9') return -1;
    {
        int d0;
        int d1;
        int d2;
        d0 = data[i] - '0';
        d1 = data[i + 1U] - '0';
        d2 = data[i + 2U] - '0';
        status = (d0 << 6) + (d0 << 5) + (d0 << 2) +
                 (d1 << 3) + (d1 << 1) + d2;
    }
    return status;
}

static int apd_recv_header(int sock, UBYTE *buffer, ULONG capacity, ULONG *used, int *status)
{
    ULONG length;
    length = 0U;
    while (length < capacity) {
        LONG got;
        ULONG end;
        got = recv(sock, (char *)(buffer + length), (LONG)(capacity - length), 0);
        if (got <= 0) return 0;
        length += (ULONG)got;
        end = apd_find_header_end(buffer, length);
        if (end != 0U) {
            *used = length;
            *status = apd_http_status(buffer, end);
            return *status >= 0;
        }
    }
    return 0;
}

static int apd_append(char *buffer, ULONG capacity, ULONG *length, const char *text)
{
    ULONG n;
    ULONG i;
    n = apd_strlen(text);
    if (*length + n >= capacity) return 0;
    for (i = 0U; i < n; ++i) buffer[(*length)++] = text[i];
    buffer[*length] = '\0';
    return 1;
}

static int apd_append_ulong(char *buffer, ULONG capacity, ULONG *length, ULONG value)
{
    static const ULONG places[] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    ULONG i;
    int started;

    started = 0;
    for (i = 0U; i < (ULONG)(sizeof(places) / sizeof(places[0])); ++i) {
        UBYTE digit;
        digit = 0U;
        while (value >= places[i]) {
            value -= places[i];
            ++digit;
        }
        if (digit != 0U || started || places[i] == 1U) {
            if (*length + 1U >= capacity) return 0;
            buffer[(*length)++] = (char)('0' + digit);
            started = 1;
        }
    }
    buffer[*length] = '\0';
    return 1;
}

static int apd_put_u8(UBYTE **cursor, UBYTE *end, UBYTE value)
{
    if (*cursor >= end) return 0;
    *(*cursor)++ = value;
    return 1;
}

static int apd_put_u16(UBYTE **cursor, UBYTE *end, UWORD value)
{
    if ((ULONG)(end - *cursor) < 2U) return 0;
    *(*cursor)++ = (UBYTE)((value >> 8) & 0xFFU);
    *(*cursor)++ = (UBYTE)(value & 0xFFU);
    return 1;
}

static int apd_put_u32(UBYTE **cursor, UBYTE *end, ULONG value)
{
    if ((ULONG)(end - *cursor) < 4U) return 0;
    *(*cursor)++ = (UBYTE)((value >> 24) & 0xFFU);
    *(*cursor)++ = (UBYTE)((value >> 16) & 0xFFU);
    *(*cursor)++ = (UBYTE)((value >> 8) & 0xFFU);
    *(*cursor)++ = (UBYTE)(value & 0xFFU);
    return 1;
}

static int apd_put_bytes(UBYTE **cursor, UBYTE *end, const UBYTE *data, ULONG length)
{
    ULONG i;
    if ((ULONG)(end - *cursor) < length) return 0;
    for (i = 0U; i < length; ++i) *(*cursor)++ = data[i];
    return 1;
}

static int apd_put_attr(UBYTE **cursor, UBYTE *end, UBYTE tag, const char *name, const char *value)
{
    ULONG name_len;
    ULONG value_len;
    name_len = apd_strlen(name);
    value_len = apd_strlen(value);
    if (name_len > 65535U || value_len > 65535U) return 0;
    return apd_put_u8(cursor, end, tag) &&
           apd_put_u16(cursor, end, (UWORD)name_len) &&
           apd_put_bytes(cursor, end, (const UBYTE *)name, name_len) &&
           apd_put_u16(cursor, end, (UWORD)value_len) &&
           apd_put_bytes(cursor, end, (const UBYTE *)value, value_len);
}

static int apd_put_attr_u32(UBYTE **cursor, UBYTE *end, UBYTE tag, const char *name, ULONG value)
{
    ULONG name_len;
    name_len = apd_strlen(name);
    if (name_len > 65535U) return 0;
    return apd_put_u8(cursor, end, tag) &&
           apd_put_u16(cursor, end, (UWORD)name_len) &&
           apd_put_bytes(cursor, end, (const UBYTE *)name, name_len) &&
           apd_put_u16(cursor, end, 4U) &&
           apd_put_u32(cursor, end, value);
}

static int apd_put_attr_resolution(UBYTE **cursor, UBYTE *end, const char *name,
                                   UWORD x, UWORD y, UBYTE units)
{
    ULONG name_len;
    name_len = apd_strlen(name);
    if (name_len > 65535U) return 0;
    return apd_put_u8(cursor, end, 0x32U) &&
           apd_put_u16(cursor, end, (UWORD)name_len) &&
           apd_put_bytes(cursor, end, (const UBYTE *)name, name_len) &&
           apd_put_u16(cursor, end, 9U) &&
           apd_put_u32(cursor, end, (ULONG)x) &&
           apd_put_u32(cursor, end, (ULONG)y) &&
           apd_put_u8(cursor, end, units != 0U ? units : 3U);
}

static int apd_build_ipp(const struct APDPreferences *prefs,
                         const char *mime,
                         UBYTE *buffer,
                         ULONG capacity,
                         char *uri,
                         ULONG uri_capacity,
                         ULONG *length)
{
    ULONG uri_len;
    UBYTE *cursor;
    UBYTE *end;

    uri_len = 0U;
    if (uri == NULL || uri_capacity == 0U) return 0;
    uri[0] = '\0';
    if (!apd_append(uri, uri_capacity, &uri_len, "ipp://") ||
        !apd_append(uri, uri_capacity, &uri_len, prefs->host) ||
        !apd_append(uri, uri_capacity, &uri_len, ":") ||
        !apd_append_ulong(uri, uri_capacity, &uri_len, prefs->port) ||
        !apd_append(uri, uri_capacity, &uri_len, prefs->path)) return 0;

    cursor = buffer;
    end = buffer + capacity;
    if (!apd_put_u8(&cursor, end, prefs->http_no_expect_required ? 1U : 2U) ||
        !apd_put_u8(&cursor, end, prefs->http_no_expect_required ? 1U : 0U) ||
        !apd_put_u16(&cursor, end, 0x0002U) ||
        !apd_put_u32(&cursor, end, 2U) ||
        !apd_put_u8(&cursor, end, 0x01U) ||
        !apd_put_attr(&cursor, end, 0x47U, "attributes-charset", "utf-8") ||
        !apd_put_attr(&cursor, end, 0x48U, "attributes-natural-language", "en") ||
        !apd_put_attr(&cursor, end, 0x45U, "printer-uri", uri) ||
        !apd_put_attr(&cursor, end, 0x42U, "requesting-user-name", "AmigaOS") ||
        !apd_put_attr(&cursor, end, 0x42U, "job-name", "AmigaOS AirPrint Job") ||
        !apd_put_attr(&cursor, end, 0x49U, "document-format", mime)) return 0;

    if (!prefs->http_no_expect_required) {
        if (!apd_put_u8(&cursor, end, 0x02U) ||
            !apd_put_attr(&cursor, end, 0x44U, "print-color-mode", prefs->color) ||
            !apd_put_attr_u32(&cursor, end, 0x23U, "print-quality", prefs->quality) ||
            !apd_put_attr(&cursor, end, 0x44U, "media", prefs->media)) return 0;

        if (prefs->media_source[0] != '\0' &&
            !apd_put_attr(&cursor, end, 0x44U, "media-source", prefs->media_source))
            return 0;

        /* Duplex is implemented only for multi-page PWG Raster jobs. */
        if (apd_streq(mime, "image/pwg-raster") && prefs->sides[0] != '\0' &&
            !apd_put_attr(&cursor, end, 0x44U, "sides", prefs->sides))
            return 0;

        if (apd_streq(mime, "image/pwg-raster") &&
            prefs->resolution_x != 0U && prefs->resolution_y != 0U &&
            !apd_put_attr_resolution(&cursor, end, "printer-resolution",
                                     prefs->resolution_x, prefs->resolution_y,
                                     prefs->resolution_units))
            return 0;

        /* Raster/PDF/PostScript carry physical orientation in their page geometry. */
        if (apd_streq(mime, "image/jpeg") || apd_streq(mime, "image/urf")) {
            ULONG orientation;
            orientation = 3U;
            if (apd_streq(prefs->orientation, "landscape")) {
                orientation = (prefs->landscape_orientation_preferred == 4U ||
                               prefs->landscape_orientation_preferred == 5U)
                    ? prefs->landscape_orientation_preferred : 4U;
            }
            if (!apd_put_attr_u32(&cursor, end, 0x23U,
                                  "orientation-requested", orientation)) return 0;
        }
    }

    if (!apd_put_u8(&cursor, end, 0x03U)) return 0;

    *length = (ULONG)(cursor - buffer);
    return 1;
}

static int apd_send_spool_file(int sock, UBYTE *buffer, ULONG capacity)
{
    BPTR file;
    LONG got;

    if (buffer == NULL || capacity == 0U) return 0;

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase == NULL) return 0;
    file = Open((CONST_STRPTR)APD_SPOOL_PATH, MODE_OLDFILE);
    if (file == 0) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return 0;
    }

    for (;;) {
        got = Read(file, buffer, (LONG)capacity);
        if (got < 0) {
            Close(file);
            CloseLibrary((struct Library *)DOSBase);
            DOSBase = NULL;
            return 0;
        }
        if (got == 0) break;
        if (!apd_send_all(sock, buffer, (ULONG)got)) {
            Close(file);
            CloseLibrary((struct Library *)DOSBase);
            DOSBase = NULL;
            return 0;
        }
    }

    Close(file);
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
    return 1;
}

static int apd_read_final_status(int sock,
                                 UBYTE *buffer,
                                 ULONG capacity,
                                 UWORD *ipp_status)
{
    ULONG used;
    ULONG header_end;
    int status;

    if (buffer == NULL || capacity < 8U || ipp_status == NULL) return 0;

    used = 0U;
    header_end = 0U;
    status = -1;

    while (used < capacity) {
        LONG got;
        got = recv(sock, (char *)(buffer + used), (LONG)(capacity - used), 0);
        if (got <= 0) return 0;
        used += (ULONG)got;

        header_end = apd_find_header_end(buffer, used);
        if (header_end == 0U) continue;
        status = apd_http_status(buffer, header_end);
        g_last_http_status = status;
        if (status >= 100 && status < 200) return 0;
        if (status != 200) return 0;

        if (used >= header_end + 8U &&
            buffer[header_end] != '\r' && buffer[header_end] != '\n') {
            ULONG pos;
            ULONG chunk_size;
            int saw_hex;
            pos = header_end;
            chunk_size = 0U;
            saw_hex = 0;
            while (pos < used && buffer[pos] != '\r') {
                int h;
                UBYTE c;
                c = buffer[pos];
                if (c >= '0' && c <= '9') h = c - '0';
                else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
                else { saw_hex = 0; break; }
                saw_hex = 1;
                chunk_size = (chunk_size << 4) | (ULONG)h;
                ++pos;
            }
            if (saw_hex && pos + 1U < used &&
                buffer[pos] == '\r' && buffer[pos + 1U] == '\n') {
                pos += 2U;
                if (chunk_size >= 8U && used >= pos + 8U) {
                    *ipp_status = (UWORD)(((UWORD)buffer[pos + 2U] << 8) |
                                          buffer[pos + 3U]);
                    return (*ipp_status & 0xFF00U) == 0U;
                }
            } else {
                *ipp_status = (UWORD)(((UWORD)buffer[header_end + 2U] << 8) |
                                      buffer[header_end + 3U]);
                return (*ipp_status & 0xFF00U) == 0U;
            }
        }
    }
    return 0;
}

static int apd_submit_job(void)
{
    struct APDWorkspace *workspace;
    const char *mime;
    ULONG ipp_len;
    ULONG http_len;
    struct sockaddr_in address;
    int sock;
    ULONG reply_used;
    int preflight_status;
    UWORD ipp_status;
    int ok;
    int result;
    int use_expect;

    if (!g_job_active || g_job_size == 0U) return 0;

    workspace = (struct APDWorkspace *)AllocMem((ULONG)sizeof(*workspace),
                                                MEMF_PUBLIC | MEMF_CLEAR);
    if (workspace == NULL) return 0;

    sock = -1;
    result = 0;
    use_expect = 1;
    g_last_socket_errno = 0;
    g_last_http_status = 0;
    g_last_ipp_status = 0xFFFFU;
    g_last_bytes = g_job_size;
    g_last_stage = APDEV_STAGE_PREFS;

    if (!apd_load_prefs(&workspace->prefs)) goto cleanup;
    if (workspace->prefs.http_no_expect_required) use_expect = 0;

    if (g_job_format == APDEV_FORMAT_JPEG) mime = "image/jpeg";
    else if (g_job_format == APDEV_FORMAT_URF) mime = "image/urf";
    else if (g_job_format == APDEV_FORMAT_PWG_RASTER) mime = "image/pwg-raster";
    else if (g_job_format == APDEV_FORMAT_PDF) mime = "application/pdf";
    else if (g_job_format == APDEV_FORMAT_POSTSCRIPT) mime = "application/postscript";
    else goto cleanup;

    if (!apd_build_ipp(&workspace->prefs, mime,
                       workspace->ipp, (ULONG)sizeof(workspace->ipp),
                       workspace->uri, (ULONG)sizeof(workspace->uri),
                       &ipp_len)) goto cleanup;

    SocketBase = (struct Library *)OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4L);
    if (SocketBase == NULL) goto cleanup;

    apd_memzero(&address, (ULONG)sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(workspace->prefs.port);
    if (!apd_parse_ipv4(workspace->prefs.host, &address.sin_addr)) goto cleanup;

    for (;;) {
        g_last_stage = APDEV_STAGE_CONNECT;
        g_last_socket_errno = 0;
        g_last_http_status = 0;

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            g_last_socket_errno = Errno();
            goto cleanup;
        }
        if (connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
            g_last_socket_errno = Errno();
            goto cleanup;
        }

        http_len = 0U;
        workspace->http[0] = '\0';
        ok = apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len, "POST ") &&
             apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len, workspace->prefs.path) &&
             apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len, " HTTP/1.1\r\nHost: ") &&
             apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len, workspace->prefs.host) &&
             apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len, ":") &&
             apd_append_ulong(workspace->http, (ULONG)sizeof(workspace->http), &http_len, workspace->prefs.port) &&
             apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len,
                        "\r\nUser-Agent: airprint.device/" AMIAIRPRINT_CORE_VERSION_TEXT " AmigaOS\r\n"
                        "Content-Type: application/ipp\r\n"
                        "Accept: application/ipp\r\nContent-Length: ") &&
             apd_append_ulong(workspace->http, (ULONG)sizeof(workspace->http), &http_len, ipp_len + g_job_size);

        if (ok && use_expect) {
            ok = apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len,
                            "\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n");
        } else if (ok) {
            ok = apd_append(workspace->http, (ULONG)sizeof(workspace->http), &http_len,
                            "\r\nConnection: close\r\n\r\n");
        }

        if (!ok || !apd_send_all(sock, (const UBYTE *)workspace->http, http_len)) {
            g_last_socket_errno = Errno();
            goto cleanup;
        }

        if (use_expect) {
            g_last_stage = APDEV_STAGE_CONTINUE;
            reply_used = 0U;
            preflight_status = -1;
            if (!apd_recv_header(sock, workspace->reply, (ULONG)sizeof(workspace->reply),
                                 &reply_used, &preflight_status) ||
                preflight_status != 100) {
                g_last_http_status = preflight_status;

                /*
                 * Compatibility fallback for embedded printer HTTP servers:
                 * 417/500 here was received before any IPP/document bytes were
                 * sent, so reopening the connection and retrying without
                 * Expect: 100-continue cannot duplicate a print job.
                 */
                if (preflight_status == 417 || preflight_status == 500) {
                    CloseSocket(sock);
                    sock = -1;
                    use_expect = 0;
                    continue;
                }

                g_last_socket_errno = Errno();
                goto cleanup;
            }
        }

        g_last_stage = APDEV_STAGE_SEND;
        if (!apd_send_all(sock, workspace->ipp, ipp_len) ||
            !apd_send_spool_file(sock, workspace->file_buffer,
                                 (ULONG)sizeof(workspace->file_buffer))) {
            g_last_socket_errno = Errno();
            goto cleanup;
        }

        g_last_stage = APDEV_STAGE_RESPONSE;
        ipp_status = 0xFFFFU;
        ok = apd_read_final_status(sock, workspace->reply,
                                   (ULONG)sizeof(workspace->reply), &ipp_status);
        g_last_ipp_status = ipp_status;
        if (!ok) {
            /*
             * The full IPP header and spool file have already been sent at
             * this point.  A retry would be unsafe.  Some embedded printer
             * firmware that previously returned HTTP 500 to Expect also
             * prints the complete job and then incorrectly returns HTTP 500
             * instead of an IPP response.  Accept only that narrowly learned
             * compatibility pattern and keep stage=response plus HTTP=500 for
             * APDEV_CMD_GET_STATUS diagnostics.
             */
            if (g_last_http_status == 500 &&
                workspace->prefs.http_no_expect_required &&
                (workspace->prefs.http_expect_reject_status == 500U ||
                 workspace->prefs.http_postbody_500_ok)) {
                g_last_socket_errno = 0;
                result = 1;
                break;
            }

            g_last_socket_errno = Errno();
            goto cleanup;
        }

        g_last_stage = APDEV_STAGE_DONE;
        result = 1;
        break;
    }

cleanup:
    if (sock >= 0 && SocketBase != NULL) CloseSocket(sock);
    if (SocketBase != NULL) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    FreeMem(workspace, (ULONG)sizeof(*workspace));
    return result;
}

static int apd_is_control(const struct IOStdReq *io, struct APDeviceControl *control)
{
    const struct APDeviceControl *source;
    if (io->io_Data == NULL || io->io_Length != (ULONG)sizeof(struct APDeviceControl)) return 0;
    source = (const struct APDeviceControl *)io->io_Data;
    if (source->magic != APDEV_CONTROL_MAGIC ||
        source->version != APDEV_CONTROL_VERSION ||
        source->reserved != 0U) return 0;
    if (source->command != APDEV_CTL_BEGIN &&
        source->command != APDEV_CTL_END &&
        source->command != APDEV_CTL_ABORT) return 0;
    if (source->format != APDEV_FORMAT_JPEG &&
        source->format != APDEV_FORMAT_URF &&
        source->format != APDEV_FORMAT_PWG_RASTER &&
        source->format != APDEV_FORMAT_PDF &&
        source->format != APDEV_FORMAT_POSTSCRIPT) return 0;
    *control = *source;
    return 1;
}

static void apd_finish_io(struct IORequest *ioreq)
{
    if (!(ioreq->io_Flags & IOF_QUICK)) ReplyMsg(&ioreq->io_Message);
}

static void apd_do_write(struct IOStdReq *io)
{
    struct APDeviceControl control;

    io->io_Error = 0;
    io->io_Actual = 0U;

    if (apd_is_control(io, &control)) {
        if (control.command == APDEV_CTL_BEGIN) {
            if (control.format != APDEV_FORMAT_JPEG &&
                control.format != APDEV_FORMAT_URF &&
                control.format != APDEV_FORMAT_PWG_RASTER &&
                control.format != APDEV_FORMAT_PDF &&
                control.format != APDEV_FORMAT_POSTSCRIPT) {
                io->io_Error = IOERR_BADADDRESS;
                return;
            }
            if (g_job_active) apd_spool_delete();
            if (!apd_spool_begin()) {
                io->io_Error = IOERR_ABORTED;
                return;
            }
            g_job_active = TRUE;
            g_job_format = control.format;
            io->io_Actual = io->io_Length;
            return;
        }
        if (control.command == APDEV_CTL_ABORT) {
            apd_spool_delete();
            io->io_Actual = io->io_Length;
            return;
        }
        if (control.command == APDEV_CTL_END) {
            if (!g_job_active || g_job_size == 0U) {
                io->io_Error = IOERR_BADLENGTH;
                return;
            }
            if (!apd_submit_job()) {
                apd_spool_delete();
                io->io_Error = IOERR_ABORTED;
                return;
            }
            apd_spool_delete();
            io->io_Actual = io->io_Length;
            return;
        }
        io->io_Error = IOERR_NOCMD;
        return;
    }

    if (!g_job_active) {
        io->io_Error = IOERR_BADADDRESS;
        return;
    }
    if (io->io_Data == NULL || io->io_Length == 0U) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }
    if (!apd_spool_append((const UBYTE *)io->io_Data, io->io_Length)) {
        io->io_Error = IOERR_ABORTED;
        return;
    }
    io->io_Actual = io->io_Length;
}

static BPTR do_expunge(struct Library *dev)
{
    BPTR seg_list;
    if (dev->lib_OpenCnt != 0U) {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seg_list = saved_seg_list;
    Remove(&dev->lib_Node);
    FreeMem((UBYTE *)dev - dev->lib_NegSize,
            (ULONG)dev->lib_NegSize + (ULONG)dev->lib_PosSize);
    return seg_list;
}

static void do_open(struct Library *dev, struct IORequest *ioreq, ULONG unitnum, ULONG flags)
{
    (void)flags;
    ioreq->io_Error = IOERR_OPENFAIL;
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    if (unitnum != 0U) return;
    dev->lib_OpenCnt++;
    dev->lib_Flags &= (UBYTE)~LIBF_DELEXP;
    ioreq->io_Error = 0;
}

static BPTR do_close(struct Library *dev, struct IORequest *ioreq)
{
    ioreq->io_Device = NULL;
    ioreq->io_Unit = NULL;
    if (dev->lib_OpenCnt != 0U) dev->lib_OpenCnt--;
    if (dev->lib_OpenCnt == 0U && (dev->lib_Flags & LIBF_DELEXP)) return do_expunge(dev);
    return 0;
}

static void do_begin_io(struct Library *dev, struct IORequest *ioreq)
{
    struct IOStdReq *io;
    (void)dev;
    io = (struct IOStdReq *)ioreq;
    ioreq->io_Error = 0;
    io->io_Actual = 0U;

    switch (ioreq->io_Command) {
        case CMD_WRITE:
            apd_do_write(io);
            break;
        case CMD_UPDATE:
            if (g_job_active && g_job_size != 0U) {
                if (!apd_submit_job()) ioreq->io_Error = IOERR_ABORTED;
                apd_spool_delete();
            }
            break;
        case CMD_CLEAR:
        case CMD_RESET:
        case CMD_FLUSH:
            apd_spool_delete();
            break;
        case CMD_STOP:
        case CMD_START:
            break;
        case APDEV_CMD_GET_STATUS:
            if (io->io_Data == NULL || io->io_Length < (ULONG)sizeof(struct APDeviceStatus)) {
                ioreq->io_Error = IOERR_BADLENGTH;
            } else {
                struct APDeviceStatus *status;
                status = (struct APDeviceStatus *)io->io_Data;
                status->bytes_spooled = g_job_active ? g_job_size : g_last_bytes;
                status->socket_errno = g_last_socket_errno;
                status->http_status = g_last_http_status;
                status->ipp_status = g_last_ipp_status;
                status->format = g_job_format;
                status->stage = g_last_stage;
                status->job_active = g_job_active ? 1U : 0U;
                io->io_Actual = sizeof(struct APDeviceStatus);
            }
            break;
        default:
            ioreq->io_Error = IOERR_NOCMD;
            break;
    }

    apd_finish_io(ioreq);
}

static ULONG do_abort_io(struct Library *dev, struct IORequest *ioreq)
{
    (void)dev;
    (void)ioreq;
    return IOERR_NOCMD;
}

static struct Library __attribute__((used)) *init_device(
    struct ExecBase *sys_base asm("a6"),
    BPTR seg_list asm("a0"),
    struct Library *dev asm("d0"))
{
    SysBase = sys_base;
    saved_seg_list = seg_list;
    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (char *)device_name;
    dev->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (APTR)device_id_string;
    DOSBase = NULL;
    SocketBase = NULL;
    g_job_active = FALSE;
    g_job_size = 0U;
    g_job_format = 0U;
    g_last_stage = APDEV_STAGE_IDLE;
    g_last_socket_errno = 0;
    g_last_http_status = 0;
    g_last_ipp_status = 0xFFFFU;
    g_last_bytes = 0U;
    return dev;
}

static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    return do_expunge(dev);
}

static void __attribute__((used)) open_device(
    struct Library *dev asm("a6"),
    struct IORequest *ioreq asm("a1"),
    ULONG unitnum asm("d0"),
    ULONG flags asm("d1"))
{
    do_open(dev, ioreq, unitnum, flags);
}

static BPTR __attribute__((used)) close_device(
    struct Library *dev asm("a6"),
    struct IORequest *ioreq asm("a1"))
{
    return do_close(dev, ioreq);
}

static void __attribute__((used)) begin_io(
    struct Library *dev asm("a6"),
    struct IORequest *ioreq asm("a1"))
{
    do_begin_io(dev, ioreq);
}

static ULONG __attribute__((used)) abort_io(
    struct Library *dev asm("a6"),
    struct IORequest *ioreq asm("a1"))
{
    return do_abort_io(dev, ioreq);
}

static ULONG device_vectors[] = {
    (ULONG)open_device,
    (ULONG)close_device,
    (ULONG)expunge,
    0U,
    (ULONG)begin_io,
    (ULONG)abort_io,
    (ULONG)-1
};

const ULONG auto_init_tables[4] __asm__("auto_init_tables") = {
    sizeof(struct Library),
    (ULONG)device_vectors,
    0U,
    (ULONG)init_device
};
