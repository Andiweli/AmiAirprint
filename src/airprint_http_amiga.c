#include "airprint_http.h"
#include "ami_airprint_version.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_HTTP_INITIAL_BUFFER 8192U
#define AP_HTTP_MAX_RESPONSE   (1024U * 1024U)
#define AP_HTTP_HEADER_MAX     2048U
#define AP_HTTP_PREFLIGHT_MAX  4096U
#define AP_HTTP_CONNECT_TIMEOUT_SECONDS 5L
#define AP_HTTP_IO_TIMEOUT_SECONDS      10L

/*
 * Keep the request header and Expect: 100-continue preflight buffer off the
 * caller stack.  The preferences tools may be launched from a Shell with a
 * small default stack, and these two buffers alone would otherwise consume
 * 6 KiB before the IPP caller's own frame is counted.
 */
struct APHTTPWorkspace {
    char header[AP_HTTP_HEADER_MAX];
    uint8_t preflight[AP_HTTP_PREFLIGHT_MAX];
};

/* BSD errno value used by AmiTCP/Roadshow compatible stacks. */
#define AP_ECONNRESET 54L

struct Library *SocketBase = NULL;

static char g_last_error[192];

static void ap_set_error(const char *message)
{
    if (message == NULL) {
        g_last_error[0] = '\0';
        return;
    }
    strncpy(g_last_error, message, sizeof(g_last_error) - 1U);
    g_last_error[sizeof(g_last_error) - 1U] = '\0';
}

static void ap_set_socket_error(const char *prefix)
{
    snprintf(g_last_error, sizeof(g_last_error), "%s (bsdsocket errno %ld)",
             prefix, (long)Errno());
}

const char *ap_http_last_error(void)
{
    return g_last_error;
}

int ap_http_open(void)
{
    if (SocketBase != NULL) {
        return 1;
    }

    SocketBase = (struct Library *)OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4L);
    if (SocketBase == NULL) {
        ap_set_error("Could not open bsdsocket.library V4+");
        return 0;
    }

    g_last_error[0] = '\0';
    return 1;
}

void ap_http_close(void)
{
    if (SocketBase != NULL) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

void ap_http_free(void *memory)
{
    free(memory);
}

static int ap_wait_socket(
    int sock,
    int wait_read,
    int wait_write,
    LONG timeout_seconds,
    const char *timeout_message)
{
    fd_set read_set;
    fd_set write_set;
    fd_set except_set;
    fd_set *read_ptr;
    fd_set *write_ptr;
    struct timeval timeout;
    LONG ready;

    read_ptr = NULL;
    write_ptr = NULL;

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    if (wait_read) {
        FD_SET(sock, &read_set);
        read_ptr = &read_set;
    }
    if (wait_write) {
        FD_SET(sock, &write_set);
        write_ptr = &write_set;
    }
    FD_SET(sock, &except_set);

    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0L;

    ready = WaitSelect(
        (LONG)sock + 1L,
        read_ptr,
        write_ptr,
        &except_set,
        &timeout,
        NULL);

    if (ready < 0L) {
        ap_set_socket_error("WaitSelect() failed");
        return 0;
    }
    if (ready == 0L) {
        ap_set_error(timeout_message);
        return 0;
    }

    if (FD_ISSET(sock, &except_set)) {
        LONG socket_error;
        socklen_t option_length;

        socket_error = 0L;
        option_length = (socklen_t)sizeof(socket_error);
        if (getsockopt(
                sock,
                SOL_SOCKET,
                SO_ERROR,
                (char *)&socket_error,
                &option_length) < 0) {
            ap_set_socket_error("getsockopt(SO_ERROR) failed");
        } else if (socket_error != 0L) {
            snprintf(
                g_last_error,
                sizeof(g_last_error),
                "Socket failed (bsdsocket errno %ld)",
                (long)socket_error);
        } else {
            ap_set_error("Socket exception while contacting printer");
        }
        return 0;
    }

    if (wait_read && !FD_ISSET(sock, &read_set)) {
        ap_set_error("Socket did not become readable");
        return 0;
    }
    if (wait_write && !FD_ISSET(sock, &write_set)) {
        ap_set_error("Socket did not become writable");
        return 0;
    }

    return 1;
}

static int ap_connect_with_timeout(
    int sock,
    const struct sockaddr_in *address)
{
    ULONG nonblocking;
    LONG socket_error;
    socklen_t option_length;

    nonblocking = 1UL;
    if (IoctlSocket(sock, FIONBIO, (char *)&nonblocking) < 0) {
        ap_set_socket_error("IoctlSocket(FIONBIO) failed");
        return 0;
    }

    if (connect(
            sock,
            (struct sockaddr *)address,
            (socklen_t)sizeof(*address)) == 0) {
        return 1;
    }

    if (!ap_wait_socket(
            sock,
            0,
            1,
            AP_HTTP_CONNECT_TIMEOUT_SECONDS,
            "Connection to printer timed out")) {
        return 0;
    }

    socket_error = 0L;
    option_length = (socklen_t)sizeof(socket_error);
    if (getsockopt(
            sock,
            SOL_SOCKET,
            SO_ERROR,
            (char *)&socket_error,
            &option_length) < 0) {
        ap_set_socket_error("getsockopt(SO_ERROR) after connect failed");
        return 0;
    }
    if (socket_error != 0L) {
        snprintf(
            g_last_error,
            sizeof(g_last_error),
            "connect() failed (bsdsocket errno %ld)",
            (long)socket_error);
        return 0;
    }

    return 1;
}

static int ap_send_all(int sock, const uint8_t *data, size_t data_len)
{
    size_t sent_total;

    sent_total = 0U;
    while (sent_total < data_len) {
        LONG sent;

        if (!ap_wait_socket(
                sock,
                0,
                1,
                AP_HTTP_IO_TIMEOUT_SECONDS,
                "Timed out while sending data to printer")) {
            return 0;
        }

        sent = send(sock, (char *)(data + sent_total),
                    (LONG)(data_len - sent_total), 0);
        if (sent <= 0) {
            ap_set_socket_error("send() failed");
            return 0;
        }
        sent_total += (size_t)sent;
    }

    return 1;
}

static int ap_parse_ipv4(const char *text, struct in_addr *address)
{
    unsigned int octets[4];
    unsigned int part;
    unsigned int value;
    const char *p;
    UBYTE *bytes;

    if (text == NULL || address == NULL || *text == '\0') {
        return 0;
    }

    p = text;
    for (part = 0U; part < 4U; ++part) {
        unsigned int digits;

        value = 0U;
        digits = 0U;

        while (*p >= '0' && *p <= '9') {
            value = value * 10U + (unsigned int)(*p - '0');
            if (value > 255U) {
                return 0;
            }
            ++digits;
            ++p;
        }

        if (digits == 0U) {
            return 0;
        }

        octets[part] = value;

        if (part < 3U) {
            if (*p != '.') {
                return 0;
            }
            ++p;
        } else if (*p != '\0') {
            return 0;
        }
    }

    bytes = (UBYTE *)&address->s_addr;
    bytes[0] = (UBYTE)octets[0];
    bytes[1] = (UBYTE)octets[1];
    bytes[2] = (UBYTE)octets[2];
    bytes[3] = (UBYTE)octets[3];
    return 1;
}

static int ap_ascii_equal_ci(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static const char *ap_find_header_ci(
    const char *headers,
    size_t headers_len,
    const char *needle)
{
    size_t needle_len;
    size_t i;

    needle_len = strlen(needle);
    if (needle_len == 0U || needle_len > headers_len) {
        return NULL;
    }

    for (i = 0U; i + needle_len <= headers_len; ++i) {
        size_t j;
        for (j = 0U; j < needle_len; ++j) {
            if (!ap_ascii_equal_ci(headers[i + j], needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            return headers + i;
        }
    }
    return NULL;
}

static size_t ap_find_header_end(const uint8_t *data, size_t data_len)
{
    size_t i;

    if (data_len < 4U) {
        return 0U;
    }

    for (i = 0U; i + 3U < data_len; ++i) {
        if (data[i] == '\r' && data[i + 1U] == '\n' &&
            data[i + 2U] == '\r' && data[i + 3U] == '\n') {
            return i + 4U;
        }
    }
    return 0U;
}

static int ap_parse_http_status(
    const uint8_t *data,
    size_t header_len,
    int *status_code)
{
    char line[96];
    size_t copy_len;
    int status;

    if (data == NULL || header_len == 0U || status_code == NULL) {
        return 0;
    }

    copy_len = header_len;
    if (copy_len >= sizeof(line)) {
        copy_len = sizeof(line) - 1U;
    }
    memcpy(line, data, copy_len);
    line[copy_len] = '\0';

    if (sscanf(line, "HTTP/%*u.%*u %d", &status) != 1) {
        return 0;
    }

    *status_code = status;
    return 1;
}

static int ap_locate_final_response(
    const uint8_t *data,
    size_t data_len,
    size_t *header_start,
    size_t *header_end,
    int *status_code)
{
    size_t offset;

    offset = 0U;
    while (offset < data_len) {
        size_t relative_end;
        int status;

        relative_end = ap_find_header_end(data + offset, data_len - offset);
        if (relative_end == 0U) {
            return 0;
        }

        if (!ap_parse_http_status(data + offset, relative_end, &status)) {
            return 0;
        }

        if (status >= 100 && status < 200) {
            offset += relative_end;
            continue;
        }

        if (header_start != NULL) {
            *header_start = offset;
        }
        if (header_end != NULL) {
            *header_end = offset + relative_end;
        }
        if (status_code != NULL) {
            *status_code = status;
        }
        return 1;
    }

    return 0;
}

static int ap_get_content_length(
    const char *headers,
    size_t headers_len,
    size_t *content_length)
{
    const char *p;
    const char *end;
    unsigned long value;

    p = ap_find_header_ci(headers, headers_len, "Content-Length:");
    if (p == NULL) {
        return 0;
    }

    p += strlen("Content-Length:");
    end = headers + headers_len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;

    value = 0UL;
    if (p >= end || *p < '0' || *p > '9') {
        return 0;
    }

    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10UL + (unsigned long)(*p - '0');
        ++p;
        if (value > AP_HTTP_MAX_RESPONSE) {
            return 0;
        }
    }

    *content_length = (size_t)value;
    return 1;
}

static int ap_is_chunked(const char *headers, size_t headers_len)
{
    const char *p;

    p = ap_find_header_ci(headers, headers_len, "Transfer-Encoding:");
    if (p == NULL) {
        return 0;
    }

    return ap_find_header_ci(
        p,
        (size_t)((headers + headers_len) - p),
        "chunked") != NULL;
}

static int ap_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ap_decode_chunked(
    const uint8_t *input,
    size_t input_len,
    uint8_t **output,
    size_t *output_len)
{
    size_t pos;
    size_t out_pos;
    uint8_t *decoded;

    decoded = (uint8_t *)malloc(input_len == 0U ? 1U : input_len);
    if (decoded == NULL) {
        ap_set_error("Out of memory while decoding chunked HTTP response");
        return 0;
    }

    pos = 0U;
    out_pos = 0U;

    while (pos < input_len) {
        unsigned long chunk_size;
        int saw_digit;

        chunk_size = 0UL;
        saw_digit = 0;

        while (pos < input_len && input[pos] != '\r' && input[pos] != '\n') {
            int h;
            if (input[pos] == ';') {
                while (pos < input_len && input[pos] != '\r') ++pos;
                break;
            }
            h = ap_hex_digit(input[pos]);
            if (h < 0) {
                free(decoded);
                ap_set_error("Malformed chunk size in HTTP response");
                return 0;
            }
            saw_digit = 1;
            chunk_size = (chunk_size << 4) | (unsigned long)h;
            if (chunk_size > AP_HTTP_MAX_RESPONSE) {
                free(decoded);
                ap_set_error("Chunked HTTP response is too large");
                return 0;
            }
            ++pos;
        }

        if (!saw_digit || pos + 1U >= input_len ||
            input[pos] != '\r' || input[pos + 1U] != '\n') {
            free(decoded);
            ap_set_error("Incomplete chunk header in HTTP response");
            return 0;
        }
        pos += 2U;

        if (chunk_size == 0UL) {
            *output = decoded;
            *output_len = out_pos;
            return 1;
        }

        if ((size_t)chunk_size > input_len - pos ||
            out_pos + (size_t)chunk_size > AP_HTTP_MAX_RESPONSE) {
            free(decoded);
            ap_set_error("Incomplete chunk data in HTTP response");
            return 0;
        }

        memcpy(decoded + out_pos, input + pos, (size_t)chunk_size);
        out_pos += (size_t)chunk_size;
        pos += (size_t)chunk_size;

        if (pos + 1U >= input_len ||
            input[pos] != '\r' || input[pos + 1U] != '\n') {
            free(decoded);
            ap_set_error("Missing CRLF after HTTP chunk");
            return 0;
        }
        pos += 2U;
    }

    free(decoded);
    ap_set_error("Incomplete chunked HTTP response");
    return 0;
}


/*
 * Return TRUE as soon as a complete HTTP/1.1 chunked body has arrived.
 * This lets us stop reading immediately after the terminating zero chunk
 * instead of waiting for an embedded printer web server to close the socket.
 */
static int ap_chunked_body_complete(const uint8_t *input, size_t input_len)
{
    size_t pos;

    pos = 0U;
    while (pos < input_len) {
        unsigned long chunk_size;
        int saw_digit;

        chunk_size = 0UL;
        saw_digit = 0;

        while (pos < input_len && input[pos] != '\r' && input[pos] != '\n') {
            int h;

            if (input[pos] == ';') {
                while (pos < input_len && input[pos] != '\r') {
                    ++pos;
                }
                break;
            }

            h = ap_hex_digit(input[pos]);
            if (h < 0) {
                return 0;
            }

            saw_digit = 1;
            chunk_size = (chunk_size << 4) | (unsigned long)h;
            if (chunk_size > AP_HTTP_MAX_RESPONSE) {
                return 0;
            }
            ++pos;
        }

        if (!saw_digit || pos + 1U >= input_len ||
            input[pos] != '\r' || input[pos + 1U] != '\n') {
            return 0;
        }
        pos += 2U;

        if (chunk_size == 0UL) {
            /* Empty trailer section: the next CRLF completes the message. */
            if (pos + 1U < input_len &&
                input[pos] == '\r' && input[pos + 1U] == '\n') {
                return 1;
            }

            /* Otherwise wait until the trailer fields end with a blank line. */
            while (pos < input_len) {
                size_t line_start;

                line_start = pos;
                while (pos + 1U < input_len &&
                       !(input[pos] == '\r' && input[pos + 1U] == '\n')) {
                    ++pos;
                }

                if (pos + 1U >= input_len) {
                    return 0;
                }

                if (pos == line_start) {
                    return 1;
                }
                pos += 2U;
            }
            return 0;
        }

        if ((size_t)chunk_size > input_len - pos) {
            return 0;
        }
        pos += (size_t)chunk_size;

        if (pos + 1U >= input_len ||
            input[pos] != '\r' || input[pos + 1U] != '\n') {
            return 0;
        }
        pos += 2U;
    }

    return 0;
}

/*
 * Send only the HTTP header first and wait for the printer's interim
 * response. With Expect: 100-continue the IPP body MUST NOT be sent before
 * the printer has replied with 100 Continue. Some Canon firmware resets the
 * TCP connection if the client pipelines the body immediately.
 *
 * If the printer returns a final status (for example 401/403) instead of
 * 100, the complete bytes received so far are returned in preflight and the
 * caller must not send the IPP body.
 */
static int ap_wait_for_continue(
    int sock,
    uint8_t *preflight,
    size_t preflight_capacity,
    size_t *preflight_len,
    int *got_continue,
    int *final_status)
{
    size_t used;

    used = 0U;
    *preflight_len = 0U;
    *got_continue = 0;
    *final_status = 0;

    for (;;) {
        size_t header_end;
        int status;

        header_end = ap_find_header_end(preflight, used);
        if (header_end != 0U) {
            if (!ap_parse_http_status(preflight, header_end, &status)) {
                ap_set_error("Malformed HTTP response while waiting for 100 Continue");
                return 0;
            }

            if (status == 100) {
                size_t remaining;

                remaining = used - header_end;
                if (remaining != 0U) {
                    memmove(preflight, preflight + header_end, remaining);
                }
                *preflight_len = remaining;
                *got_continue = 1;
                return 1;
            }

            if (status >= 100 && status < 200) {
                size_t remaining;

                remaining = used - header_end;
                if (remaining != 0U) {
                    memmove(preflight, preflight + header_end, remaining);
                }
                used = remaining;
                continue;
            }

            *preflight_len = used;
            *final_status = status;
            return 1;
        }

        if (used >= preflight_capacity) {
            ap_set_error("HTTP preflight response header is too large");
            return 0;
        }

        {
            LONG received;

            if (!ap_wait_socket(
                    sock,
                    1,
                    0,
                    AP_HTTP_IO_TIMEOUT_SECONDS,
                    "Timed out waiting for printer HTTP response")) {
                return 0;
            }

            received = recv(
                sock,
                (char *)preflight + used,
                (LONG)(preflight_capacity - used),
                0);

            if (received < 0) {
                ap_set_socket_error("recv() while waiting for 100 Continue failed");
                return 0;
            }
            if (received == 0) {
                ap_set_error("Printer closed connection before 100 Continue");
                return 0;
            }
            used += (size_t)received;
        }
    }
}

static int ap_raw_response_complete(
    const uint8_t *raw,
    size_t raw_len)
{
    size_t header_start;
    size_t header_end;
    int status;
    size_t content_length;

    if (!ap_locate_final_response(
            raw, raw_len, &header_start, &header_end, &status)) {
        return 0;
    }

    (void)status;

    if (ap_get_content_length(
            (const char *)raw + header_start,
            header_end - header_start,
            &content_length)) {
        return raw_len >= header_end + content_length;
    }

    if (ap_is_chunked((const char *)raw + header_start,
                      header_end - header_start)) {
        return ap_chunked_body_complete(raw + header_end,
                                        raw_len - header_end);
    }

    /* A close-delimited non-chunked response can only be ended by EOF/RST. */
    return 1;
}

int ap_http_post_ipp(
    const char *host,
    uint16_t port,
    const char *path,
    const uint8_t *request_body,
    size_t request_body_len,
    uint8_t **response_body,
    size_t *response_body_len,
    int *http_status)
{
    int sock;
    struct sockaddr_in address;
    struct APHTTPWorkspace *workspace;
    char *header;
    uint8_t *preflight;
    int header_len;
    size_t preflight_len;
    int got_continue;
    int preflight_final_status;
    uint8_t *raw;
    size_t raw_len;
    size_t raw_capacity;
    size_t header_start;
    size_t header_end;
    size_t content_length;
    int have_content_length;
    int final_status;
    int chunked;
    uint8_t *body;
    size_t body_len;

    if (host == NULL || path == NULL || request_body == NULL ||
        response_body == NULL || response_body_len == NULL || http_status == NULL) {
        ap_set_error("Invalid HTTP arguments");
        return 0;
    }

    *response_body = NULL;
    *response_body_len = 0U;
    *http_status = 0;

    if (SocketBase == NULL) {
        ap_set_error("bsdsocket.library is not open");
        return 0;
    }

    if (strlen(host) > 63U || strlen(path) > 255U) {
        ap_set_error("Host or IPP path is too long");
        return 0;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (!ap_parse_ipv4(host, &address.sin_addr)) {
        ap_set_error("Host must currently be a valid IPv4 address");
        return 0;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ap_set_socket_error("socket() failed");
        return 0;
    }

    if (!ap_connect_with_timeout(sock, &address)) {
        CloseSocket(sock);
        return 0;
    }

    workspace = (struct APHTTPWorkspace *)malloc(sizeof(*workspace));
    if (workspace == NULL) {
        ap_set_error("Out of memory while preparing HTTP request");
        CloseSocket(sock);
        return 0;
    }
    header = workspace->header;
    preflight = workspace->preflight;

    header_len = snprintf(
        header,
        AP_HTTP_HEADER_MAX,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: AmiAirPrint/" AMIAIRPRINT_VERSION_TEXT " AmigaOS\r\n"
        "Content-Type: application/ipp\r\n"
        "Accept: application/ipp\r\n"
        "Content-Length: %lu\r\n"
        "Expect: 100-continue\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        host,
        (unsigned int)port,
        (unsigned long)request_body_len);

    if (header_len <= 0 || (size_t)header_len >= AP_HTTP_HEADER_MAX) {
        ap_set_error("HTTP request header is too large");
        free(workspace);
        CloseSocket(sock);
        return 0;
    }

    /* Phase 1: send the HTTP header only. */
    if (!ap_send_all(sock, (const uint8_t *)header, (size_t)header_len)) {
        free(workspace);
        CloseSocket(sock);
        return 0;
    }

    /* Phase 2: wait for 100 Continue or a final HTTP rejection. */
    if (!ap_wait_for_continue(
            sock,
            preflight,
            AP_HTTP_PREFLIGHT_MAX,
            &preflight_len,
            &got_continue,
            &preflight_final_status)) {
        free(workspace);
        CloseSocket(sock);
        return 0;
    }

    /* Only after 100 Continue is the IPP body transmitted. */
    if (got_continue) {
        if (!ap_send_all(sock, request_body, request_body_len)) {
            free(workspace);
            CloseSocket(sock);
            return 0;
        }
    }

    raw_capacity = AP_HTTP_INITIAL_BUFFER;
    while (raw_capacity < preflight_len + 1U) {
        raw_capacity *= 2U;
    }

    raw = (uint8_t *)malloc(raw_capacity + 1U);
    if (raw == NULL) {
        ap_set_error("Out of memory while receiving HTTP response");
        free(workspace);
        CloseSocket(sock);
        return 0;
    }

    raw_len = preflight_len;
    if (preflight_len != 0U) {
        memcpy(raw, preflight, preflight_len);
    }
    free(workspace);
    raw[raw_len] = 0U;

    header_start = 0U;
    header_end = 0U;
    content_length = 0U;
    have_content_length = 0;
    final_status = preflight_final_status;

    /*
     * A final status received during preflight means the body was correctly
     * not sent. There may already be response-body bytes in preflight, so the
     * normal receive loop below completes that response.
     */
    for (;;) {
        LONG received;

        if (ap_locate_final_response(
                raw, raw_len, &header_start, &header_end, &final_status)) {
            have_content_length = ap_get_content_length(
                (const char *)raw + header_start,
                header_end - header_start,
                &content_length);

            if (have_content_length &&
                raw_len >= header_end + content_length) {
                break;
            }

            if (!have_content_length &&
                ap_is_chunked((const char *)raw + header_start,
                              header_end - header_start) &&
                ap_chunked_body_complete(raw + header_end,
                                         raw_len - header_end)) {
                break;
            }
        }

        if (raw_capacity - raw_len < 2048U) {
            size_t new_capacity;
            uint8_t *new_raw;

            new_capacity = raw_capacity * 2U;
            if (new_capacity > AP_HTTP_MAX_RESPONSE) {
                new_capacity = AP_HTTP_MAX_RESPONSE;
            }
            if (new_capacity <= raw_capacity) {
                free(raw);
                CloseSocket(sock);
                ap_set_error("HTTP response exceeds 1 MiB safety limit");
                return 0;
            }

            new_raw = (uint8_t *)realloc(raw, new_capacity + 1U);
            if (new_raw == NULL) {
                free(raw);
                CloseSocket(sock);
                ap_set_error("Out of memory while growing HTTP response buffer");
                return 0;
            }
            raw = new_raw;
            raw_capacity = new_capacity;
        }

        if (!ap_wait_socket(
                sock,
                1,
                0,
                AP_HTTP_IO_TIMEOUT_SECONDS,
                "Timed out waiting for printer HTTP response")) {
            free(raw);
            CloseSocket(sock);
            return 0;
        }

        received = recv(
            sock,
            (char *)raw + raw_len,
            (LONG)(raw_capacity - raw_len),
            0);

        if (received < 0) {
            long socket_errno;
            socket_errno = (long)Errno();

            if (socket_errno == AP_ECONNRESET &&
                raw_len != 0U &&
                ap_raw_response_complete(raw, raw_len)) {
                /*
                 * Some embedded HTTP servers terminate with RST after the
                 * complete response. If we already have a valid final HTTP
                 * response, treat that like EOF and let the body validation
                 * below decide whether the data is complete.
                 */
                break;
            }

            ap_set_socket_error("recv() while reading final HTTP response failed");
            free(raw);
            CloseSocket(sock);
            return 0;
        }
        if (received == 0) {
            break;
        }

        raw_len += (size_t)received;
        raw[raw_len] = 0U;
    }

    CloseSocket(sock);

    if (!ap_locate_final_response(
            raw, raw_len, &header_start, &header_end, &final_status)) {
        free(raw);
        ap_set_error("HTTP response has no complete final response header");
        return 0;
    }

    *http_status = final_status;

    have_content_length = ap_get_content_length(
        (const char *)raw + header_start,
        header_end - header_start,
        &content_length);

    chunked = ap_is_chunked(
        (const char *)raw + header_start,
        header_end - header_start);
    body_len = raw_len - header_end;

    if (chunked) {
        int ok;
        ok = ap_decode_chunked(raw + header_end, body_len, &body, &body_len);
        free(raw);
        if (!ok) {
            return 0;
        }
    } else {
        if (have_content_length) {
            if (body_len < content_length) {
                free(raw);
                ap_set_error("HTTP body is shorter than Content-Length");
                return 0;
            }
            body_len = content_length;
        }

        body = (uint8_t *)malloc(body_len == 0U ? 1U : body_len);
        if (body == NULL) {
            free(raw);
            ap_set_error("Out of memory while copying HTTP body");
            return 0;
        }
        if (body_len != 0U) {
            memcpy(body, raw + header_end, body_len);
        }
        free(raw);
    }

    *response_body = body;
    *response_body_len = body_len;
    g_last_error[0] = '\0';
    return 1;
}
