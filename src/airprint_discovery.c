#include "airprint_discovery.h"

#include <exec/types.h>
#include <exec/libraries.h>

extern struct Library *SocketBase;

#include <proto/bsdsocket.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <stdio.h>
#include <string.h>

#define AP_MDNS_PORT             5353U
#define AP_MDNS_ADDRESS          0xE00000FBUL /* 224.0.0.251 */
#define AP_DNS_TYPE_A            1U
#define AP_DNS_TYPE_PTR          12U
#define AP_DNS_TYPE_TXT          16U
#define AP_DNS_TYPE_SRV          33U
#define AP_DNS_CLASS_IN          1U
#define AP_MDNS_PACKET_MAX       1500U
#define AP_DISCOVERY_HOSTS_MAX   16U
#define AP_DISCOVERY_ROUNDS       4U
#define AP_DISCOVERY_DETAIL_ROUNDS 4U
#define AP_DISCOVERY_A_ROUNDS     4U
#define AP_DISCOVERY_WAIT_USEC 250000L

struct APDiscoveryService {
    int used;
    int secure;
    int has_srv;
    int has_txt;
    int has_address;
    int airprint;
    char instance[AP_DISCOVERY_NAME_LEN + 40U];
    char display_name[AP_DISCOVERY_NAME_LEN];
    char host_name[AP_DISCOVERY_HOST_LEN];
    char address[AP_DISCOVERY_ADDRESS_LEN];
    char path[AP_DISCOVERY_PATH_LEN];
    uint16_t port;
};

struct APDiscoveryHost {
    int used;
    char name[AP_DISCOVERY_HOST_LEN];
    uint8_t address[4];
};

struct APDiscoveryWorkspace {
    uint8_t packet[AP_MDNS_PACKET_MAX];
    struct APDiscoveryService services[AP_DISCOVERY_MAX_PRINTERS * 2U];
    struct APDiscoveryHost hosts[AP_DISCOVERY_HOSTS_MAX];
};

static struct APDiscoveryWorkspace g_workspace;
static char g_last_error[160];

static void ap_copy_text(char *destination, size_t destination_size,
                         const char *source)
{
    size_t i;

    if (destination == NULL || destination_size == 0U) return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    i = 0U;
    while (i + 1U < destination_size && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

static void ap_copy_path(char *destination, size_t destination_size,
                         const char *source)
{
    size_t out;
    size_t in;

    if (destination == NULL || destination_size == 0U) return;
    destination[0] = '\0';
    if (source == NULL || source[0] == '\0') return;

    out = 0U;
    if (source[0] != '/' && out + 1U < destination_size)
        destination[out++] = '/';
    in = 0U;
    while (out + 1U < destination_size && source[in] != '\0')
        destination[out++] = source[in++];
    destination[out] = '\0';
}

static void ap_discovery_set_error(const char *text)
{
    if (text == NULL) {
        g_last_error[0] = '\0';
        return;
    }
    snprintf(g_last_error, sizeof(g_last_error), "%s", text);
}

static void ap_discovery_set_socket_error(const char *prefix)
{
    snprintf(g_last_error, sizeof(g_last_error), "%s (bsdsocket errno %ld)",
             prefix, (long)Errno());
}

const char *ap_discovery_last_error(void)
{
    return g_last_error;
}

static uint16_t ap_read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t ap_read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void ap_write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xffU);
}

static int ap_ascii_equal_nocase(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ap_ascii_contains_nocase(const char *text, const char *needle)
{
    size_t text_len;
    size_t needle_len;
    size_t i;
    size_t j;

    if (text == NULL || needle == NULL) return 0;
    text_len = strlen(text);
    needle_len = strlen(needle);
    if (needle_len == 0U) return 1;
    if (needle_len > text_len) return 0;

    for (i = 0U; i + needle_len <= text_len; ++i) {
        for (j = 0U; j < needle_len; ++j) {
            unsigned char a;
            unsigned char b;
            a = (unsigned char)text[i + j];
            b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) break;
        }
        if (j == needle_len) return 1;
    }
    return 0;
}

static int ap_dns_put_name(uint8_t *packet, size_t packet_size,
                           size_t *offset, const char *name)
{
    const char *label;
    const char *dot;
    size_t length;

    if (packet == NULL || offset == NULL || name == NULL) return 0;
    label = name;
    while (*label != '\0') {
        dot = strchr(label, '.');
        length = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (length == 0U || length > 63U || *offset + 1U + length >= packet_size)
            return 0;
        packet[(*offset)++] = (uint8_t)length;
        memcpy(packet + *offset, label, length);
        *offset += length;
        if (dot == NULL) break;
        label = dot + 1;
    }
    if (*offset >= packet_size) return 0;
    packet[(*offset)++] = 0U;
    return 1;
}

static int ap_dns_read_name(const uint8_t *packet, size_t packet_len,
                            size_t *offset, char *out, size_t out_size)
{
    size_t pos;
    size_t next_offset;
    size_t out_len;
    unsigned int jumps;
    int jumped;

    if (packet == NULL || offset == NULL || out == NULL || out_size == 0U)
        return 0;

    pos = *offset;
    next_offset = pos;
    out_len = 0U;
    jumps = 0U;
    jumped = 0;
    out[0] = '\0';

    while (pos < packet_len) {
        uint8_t length;
        length = packet[pos];

        if (length == 0U) {
            if (!jumped) next_offset = pos + 1U;
            if (out_len >= out_size) return 0;
            out[out_len] = '\0';
            *offset = next_offset;
            return 1;
        }

        if ((length & 0xc0U) == 0xc0U) {
            size_t pointer;
            if (pos + 1U >= packet_len) return 0;
            pointer = (size_t)(((uint16_t)(length & 0x3fU) << 8) |
                               (uint16_t)packet[pos + 1U]);
            if (pointer >= packet_len || ++jumps > 20U) return 0;
            if (!jumped) {
                next_offset = pos + 2U;
                jumped = 1;
            }
            pos = pointer;
            continue;
        }

        if ((length & 0xc0U) != 0U || length > 63U) return 0;
        ++pos;
        if (pos + (size_t)length > packet_len) return 0;
        if (out_len != 0U) {
            if (out_len + 1U >= out_size) return 0;
            out[out_len++] = '.';
        }
        if (out_len + (size_t)length >= out_size) return 0;
        memcpy(out + out_len, packet + pos, (size_t)length);
        out_len += (size_t)length;
        pos += (size_t)length;
        if (!jumped) next_offset = pos;
    }
    return 0;
}

static int ap_service_suffix(const char *name, int *secure)
{
    if (name == NULL || secure == NULL) return 0;
    if (ap_ascii_equal_nocase(name, "_ipp._tcp.local") ||
        ap_ascii_contains_nocase(name, "._ipp._tcp.local")) {
        *secure = 0;
        return 1;
    }
    if (ap_ascii_equal_nocase(name, "_ipps._tcp.local") ||
        ap_ascii_contains_nocase(name, "._ipps._tcp.local")) {
        *secure = 1;
        return 1;
    }
    return 0;
}

static void ap_service_default_name(struct APDiscoveryService *service)
{
    const char *suffix;
    size_t length;

    if (service == NULL || service->display_name[0] != '\0') return;
    suffix = service->secure ? "._ipps._tcp.local" : "._ipp._tcp.local";
    length = strlen(service->instance);
    if (length > strlen(suffix) &&
        ap_ascii_contains_nocase(service->instance, suffix)) {
        size_t keep;
        keep = length - strlen(suffix);
        if (keep >= sizeof(service->display_name))
            keep = sizeof(service->display_name) - 1U;
        memcpy(service->display_name, service->instance, keep);
        service->display_name[keep] = '\0';
    } else {
        ap_copy_text(service->display_name, sizeof(service->display_name),
                     service->instance);
    }
}

static struct APDiscoveryService *ap_get_service(const char *instance,
                                                 int secure_hint)
{
    unsigned int i;
    struct APDiscoveryService *free_service;

    free_service = NULL;
    for (i = 0U; i < AP_DISCOVERY_MAX_PRINTERS * 2U; ++i) {
        struct APDiscoveryService *service;
        service = &g_workspace.services[i];
        if (service->used) {
            if (ap_ascii_equal_nocase(service->instance, instance))
                return service;
        } else if (free_service == NULL) {
            free_service = service;
        }
    }

    if (free_service == NULL) return NULL;
    memset(free_service, 0, sizeof(*free_service));
    free_service->used = 1;
    free_service->secure = secure_hint;
    ap_copy_text(free_service->instance, sizeof(free_service->instance),
                 instance != NULL ? instance : "");
    ap_copy_text(free_service->path, sizeof(free_service->path), "/ipp/print");
    ap_service_default_name(free_service);
    return free_service;
}

static void ap_store_host_address(const char *name, const uint8_t address[4])
{
    unsigned int i;
    struct APDiscoveryHost *free_host;

    if (name == NULL || address == NULL) return;
    free_host = NULL;
    for (i = 0U; i < AP_DISCOVERY_HOSTS_MAX; ++i) {
        struct APDiscoveryHost *host;
        host = &g_workspace.hosts[i];
        if (host->used && ap_ascii_equal_nocase(host->name, name)) {
            memcpy(host->address, address, 4U);
            return;
        }
        if (!host->used && free_host == NULL) free_host = host;
    }
    if (free_host != NULL) {
        memset(free_host, 0, sizeof(*free_host));
        free_host->used = 1;
        ap_copy_text(free_host->name, sizeof(free_host->name), name);
        memcpy(free_host->address, address, 4U);
    }
}

static int ap_find_host_address(const char *name, uint8_t address[4])
{
    unsigned int i;
    if (name == NULL || address == NULL) return 0;
    for (i = 0U; i < AP_DISCOVERY_HOSTS_MAX; ++i) {
        const struct APDiscoveryHost *host;
        host = &g_workspace.hosts[i];
        if (host->used && ap_ascii_equal_nocase(host->name, name)) {
            memcpy(address, host->address, 4U);
            return 1;
        }
    }
    return 0;
}

static void ap_apply_known_addresses(void)
{
    unsigned int i;
    for (i = 0U; i < AP_DISCOVERY_MAX_PRINTERS * 2U; ++i) {
        struct APDiscoveryService *service;
        uint8_t address[4];
        service = &g_workspace.services[i];
        if (!service->used || service->host_name[0] == '\0' || service->has_address)
            continue;
        if (ap_find_host_address(service->host_name, address)) {
            snprintf(service->address, sizeof(service->address), "%u.%u.%u.%u",
                     (unsigned int)address[0], (unsigned int)address[1],
                     (unsigned int)address[2], (unsigned int)address[3]);
            service->has_address = 1;
        }
    }
}

static void ap_parse_txt(struct APDiscoveryService *service,
                         const uint8_t *data, size_t length)
{
    size_t offset;

    if (service == NULL || data == NULL) return;
    offset = 0U;
    while (offset < length) {
        size_t item_len;
        const uint8_t *item;
        const uint8_t *equals;
        size_t key_len;
        size_t value_len;
        char value[AP_DISCOVERY_NAME_LEN];

        item_len = (size_t)data[offset++];
        if (item_len == 0U) continue;
        if (offset + item_len > length) break;
        item = data + offset;
        equals = (const uint8_t *)memchr(item, '=', item_len);
        if (equals != NULL) {
            key_len = (size_t)(equals - item);
            value_len = item_len - key_len - 1U;
            if (value_len >= sizeof(value)) value_len = sizeof(value) - 1U;
            memcpy(value, equals + 1, value_len);
            value[value_len] = '\0';

            if (key_len == 2U &&
                (item[0] == 'r' || item[0] == 'R') &&
                (item[1] == 'p' || item[1] == 'P')) {
                if (value[0] != '\0')
                    ap_copy_path(service->path, sizeof(service->path), value);
            } else if (key_len == 2U &&
                       (item[0] == 't' || item[0] == 'T') &&
                       (item[1] == 'y' || item[1] == 'Y')) {
                if (value[0] != '\0')
                    ap_copy_text(service->display_name, sizeof(service->display_name), value);
            } else if (key_len == 3U &&
                       (item[0] == 'u' || item[0] == 'U') &&
                       (item[1] == 'r' || item[1] == 'R') &&
                       (item[2] == 'f' || item[2] == 'F')) {
                service->airprint = 1;
            } else if (key_len == 3U &&
                       (item[0] == 'p' || item[0] == 'P') &&
                       (item[1] == 'd' || item[1] == 'D') &&
                       (item[2] == 'l' || item[2] == 'L')) {
                if (ap_ascii_contains_nocase(value, "image/urf") ||
                    ap_ascii_contains_nocase(value, "image/pwg-raster"))
                    service->airprint = 1;
            }
        }
        offset += item_len;
    }
    service->has_txt = 1;
}

static void ap_parse_record(const uint8_t *packet, size_t packet_len,
                            size_t *offset)
{
    char owner[AP_DISCOVERY_NAME_LEN + 64U];
    uint16_t type;
    uint16_t class_value;
    uint32_t ttl;
    uint16_t rdlength;
    size_t rdata_offset;
    size_t end_offset;

    if (!ap_dns_read_name(packet, packet_len, offset, owner, sizeof(owner))) {
        *offset = packet_len;
        return;
    }
    if (*offset + 10U > packet_len) {
        *offset = packet_len;
        return;
    }

    type = ap_read_u16(packet + *offset);
    class_value = ap_read_u16(packet + *offset + 2U);
    ttl = ap_read_u32(packet + *offset + 4U);
    rdlength = ap_read_u16(packet + *offset + 8U);
    (void)class_value;
    (void)ttl;
    *offset += 10U;
    rdata_offset = *offset;
    end_offset = rdata_offset + (size_t)rdlength;
    if (end_offset > packet_len) {
        *offset = packet_len;
        return;
    }

    if (type == AP_DNS_TYPE_PTR) {
        char target[AP_DISCOVERY_NAME_LEN + 64U];
        size_t name_offset;
        int secure;
        secure = 0;
        name_offset = rdata_offset;
        if (ap_dns_read_name(packet, packet_len, &name_offset, target, sizeof(target)) &&
            ap_service_suffix(owner, &secure)) {
            (void)ap_get_service(target, secure);
        }
    } else if (type == AP_DNS_TYPE_SRV) {
        int secure;
        struct APDiscoveryService *service;
        secure = 0;
        if (ap_service_suffix(owner, &secure) && rdlength >= 6U) {
            size_t name_offset;
            service = ap_get_service(owner, secure);
            if (service != NULL) {
                service->port = ap_read_u16(packet + rdata_offset + 4U);
                name_offset = rdata_offset + 6U;
                if (ap_dns_read_name(packet, packet_len, &name_offset,
                                     service->host_name,
                                     sizeof(service->host_name))) {
                    service->has_srv = service->port != 0U;
                }
            }
        }
    } else if (type == AP_DNS_TYPE_TXT) {
        int secure;
        struct APDiscoveryService *service;
        secure = 0;
        if (ap_service_suffix(owner, &secure)) {
            service = ap_get_service(owner, secure);
            if (service != NULL)
                ap_parse_txt(service, packet + rdata_offset, (size_t)rdlength);
        }
    } else if (type == AP_DNS_TYPE_A && rdlength == 4U) {
        ap_store_host_address(owner, packet + rdata_offset);
    }

    *offset = end_offset;
}

static void ap_parse_packet(const uint8_t *packet, size_t packet_len)
{
    uint16_t questions;
    uint32_t records;
    size_t offset;
    uint16_t i;
    uint32_t record_index;

    if (packet == NULL || packet_len < 12U) return;
    questions = ap_read_u16(packet + 4U);
    records = (uint32_t)ap_read_u16(packet + 6U) +
              (uint32_t)ap_read_u16(packet + 8U) +
              (uint32_t)ap_read_u16(packet + 10U);
    offset = 12U;

    for (i = 0U; i < questions; ++i) {
        char ignored[256];
        if (!ap_dns_read_name(packet, packet_len, &offset, ignored, sizeof(ignored)))
            return;
        if (offset + 4U > packet_len) return;
        offset += 4U;
    }

    for (record_index = 0U; record_index < records && offset < packet_len;
         ++record_index) {
        ap_parse_record(packet, packet_len, &offset);
    }
    ap_apply_known_addresses();
}

static size_t ap_build_service_query(uint8_t *packet, size_t packet_size,
                                     uint16_t query_id)
{
    size_t offset;

    if (packet == NULL || packet_size < 12U) return 0U;
    memset(packet, 0, packet_size);
    ap_write_u16(packet, query_id);
    ap_write_u16(packet + 4U, 2U);
    offset = 12U;

    if (!ap_dns_put_name(packet, packet_size, &offset, "_ipp._tcp.local"))
        return 0U;
    if (offset + 4U > packet_size) return 0U;
    ap_write_u16(packet + offset, AP_DNS_TYPE_PTR);
    ap_write_u16(packet + offset + 2U, AP_DNS_CLASS_IN);
    offset += 4U;

    if (!ap_dns_put_name(packet, packet_size, &offset, "_ipps._tcp.local"))
        return 0U;
    if (offset + 4U > packet_size) return 0U;
    ap_write_u16(packet + offset, AP_DNS_TYPE_PTR);
    ap_write_u16(packet + offset + 2U, AP_DNS_CLASS_IN);
    offset += 4U;
    return offset;
}

static size_t ap_build_detail_query(uint8_t *packet, size_t packet_size,
                                    uint16_t query_id, const char *instance)
{
    size_t offset;
    if (packet == NULL || packet_size < 12U || instance == NULL || instance[0] == '\0')
        return 0U;
    memset(packet, 0, packet_size);
    ap_write_u16(packet, query_id);
    ap_write_u16(packet + 4U, 2U);
    offset = 12U;

    if (!ap_dns_put_name(packet, packet_size, &offset, instance)) return 0U;
    if (offset + 4U > packet_size) return 0U;
    ap_write_u16(packet + offset, AP_DNS_TYPE_SRV);
    ap_write_u16(packet + offset + 2U, AP_DNS_CLASS_IN);
    offset += 4U;

    if (!ap_dns_put_name(packet, packet_size, &offset, instance)) return 0U;
    if (offset + 4U > packet_size) return 0U;
    ap_write_u16(packet + offset, AP_DNS_TYPE_TXT);
    ap_write_u16(packet + offset + 2U, AP_DNS_CLASS_IN);
    return offset + 4U;
}

static size_t ap_build_a_query(uint8_t *packet, size_t packet_size,
                               uint16_t query_id, const char *host_name)
{
    size_t offset;
    if (packet == NULL || packet_size < 12U || host_name == NULL || host_name[0] == '\0')
        return 0U;
    memset(packet, 0, packet_size);
    ap_write_u16(packet, query_id);
    ap_write_u16(packet + 4U, 1U);
    offset = 12U;
    if (!ap_dns_put_name(packet, packet_size, &offset, host_name)) return 0U;
    if (offset + 4U > packet_size) return 0U;
    ap_write_u16(packet + offset, AP_DNS_TYPE_A);
    ap_write_u16(packet + offset + 2U, AP_DNS_CLASS_IN);
    return offset + 4U;
}

static int ap_send_query(int sock, const uint8_t *packet, size_t packet_len,
                         const struct sockaddr_in *destination)
{
    LONG sent;
    sent = sendto(sock, (char *)packet, (LONG)packet_len, 0,
                  (struct sockaddr *)destination,
                  (socklen_t)sizeof(*destination));
    if (sent != (LONG)packet_len) {
        ap_discovery_set_socket_error("mDNS sendto() failed");
        return 0;
    }
    return 1;
}

static int ap_receive_rounds(int sock, unsigned int rounds)
{
    unsigned int round;

    for (round = 0U; round < rounds; ++round) {
        int first_wait;

        first_wait = 1;
        for (;;) {
            fd_set read_set;
            struct timeval timeout;
            LONG ready;

            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            timeout.tv_sec = 0L;
            timeout.tv_usec = first_wait ? AP_DISCOVERY_WAIT_USEC : 0L;

            ready = WaitSelect((LONG)sock + 1L, &read_set, NULL, NULL, &timeout, NULL);
            if (ready < 0L) {
                ap_discovery_set_socket_error("mDNS WaitSelect() failed");
                return 0;
            }
            if (ready == 0L || !FD_ISSET(sock, &read_set)) break;

            {
                LONG received;
                received = recv(sock, (char *)g_workspace.packet,
                                (LONG)sizeof(g_workspace.packet), 0);
                if (received > 0L)
                    ap_parse_packet(g_workspace.packet, (size_t)received);
            }
            first_wait = 0;
        }
    }
    return 1;
}

static int ap_send_missing_detail_queries(int sock,
                                          const struct sockaddr_in *destination,
                                          uint16_t *query_id)
{
    unsigned int i;
    int sent_any;

    sent_any = 0;
    for (i = 0U; i < AP_DISCOVERY_MAX_PRINTERS * 2U; ++i) {
        const struct APDiscoveryService *service;
        size_t query_len;

        service = &g_workspace.services[i];
        if (!service->used || (service->has_srv && service->has_txt)) continue;

        ++(*query_id);
        query_len = ap_build_detail_query(g_workspace.packet,
                                          sizeof(g_workspace.packet),
                                          *query_id, service->instance);
        if (query_len != 0U) {
            if (!ap_send_query(sock, g_workspace.packet, query_len, destination))
                return -1;
            sent_any = 1;
        }
    }
    return sent_any;
}

static int ap_send_missing_a_queries(int sock,
                                     const struct sockaddr_in *destination,
                                     uint16_t *query_id)
{
    unsigned int i;
    int sent_any;

    sent_any = 0;
    for (i = 0U; i < AP_DISCOVERY_MAX_PRINTERS * 2U; ++i) {
        const struct APDiscoveryService *service;
        size_t query_len;
        unsigned int previous;
        int duplicate;

        service = &g_workspace.services[i];
        if (!service->used || !service->has_srv || service->has_address ||
            service->host_name[0] == '\0')
            continue;

        duplicate = 0;
        for (previous = 0U; previous < i; ++previous) {
            const struct APDiscoveryService *other;
            other = &g_workspace.services[previous];
            if (other->used && other->host_name[0] != '\0' &&
                ap_ascii_equal_nocase(other->host_name, service->host_name)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;

        ++(*query_id);
        query_len = ap_build_a_query(g_workspace.packet,
                                     sizeof(g_workspace.packet),
                                     *query_id, service->host_name);
        if (query_len != 0U) {
            if (!ap_send_query(sock, g_workspace.packet, query_len, destination))
                return -1;
            sent_any = 1;
        }
    }
    return sent_any;
}

static int ap_same_endpoint(const struct APDiscoveredPrinter *printer,
                            const struct APDiscoveryService *service)
{
    if (printer == NULL || service == NULL) return 0;
    return printer->port == service->port &&
           strcmp(printer->address, service->address) == 0 &&
           strcmp(printer->path, service->path) == 0;
}

static void ap_finalize_results(struct APDiscoveryResult *result)
{
    unsigned int i;

    result->count = 0U;
    ap_apply_known_addresses();

    for (i = 0U; i < AP_DISCOVERY_MAX_PRINTERS * 2U; ++i) {
        struct APDiscoveryService *service;
        unsigned int existing;
        struct APDiscoveredPrinter *printer;

        service = &g_workspace.services[i];
        if (!service->used || service->secure || !service->has_srv ||
            !service->has_address || service->port == 0U)
            continue;

        ap_service_default_name(service);
        for (existing = 0U; existing < result->count; ++existing) {
            if (ap_same_endpoint(&result->printers[existing], service)) break;
        }
        if (existing < result->count) {
            if (service->airprint) result->printers[existing].airprint = 1;
            continue;
        }
        if (result->count >= AP_DISCOVERY_MAX_PRINTERS) break;

        printer = &result->printers[result->count++];
        memset(printer, 0, sizeof(*printer));
        ap_copy_text(printer->name, sizeof(printer->name),
                     service->display_name[0] != '\0' ? service->display_name : service->instance);
        ap_copy_text(printer->host_name, sizeof(printer->host_name), service->host_name);
        ap_copy_text(printer->address, sizeof(printer->address), service->address);
        ap_copy_text(printer->path, sizeof(printer->path),
                     service->path[0] != '\0' ? service->path : "/ipp/print");
        printer->port = service->port;
        printer->airprint = service->airprint;
    }
}

int ap_discovery_search(struct APDiscoveryResult *result)
{
    struct sockaddr_in destination;
    int sock;
    uint8_t multicast_ttl;
    size_t query_len;
    uint16_t query_id;
    int a_queries;

    if (result == NULL) {
        ap_discovery_set_error("Invalid discovery result buffer");
        return 0;
    }
    memset(result, 0, sizeof(*result));
    memset(&g_workspace, 0, sizeof(g_workspace));
    g_last_error[0] = '\0';

    if (SocketBase == NULL) {
        ap_discovery_set_error("bsdsocket.library is not open");
        return 0;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ap_discovery_set_socket_error("Could not create mDNS socket");
        return 0;
    }

    multicast_ttl = 255U;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                     (char *)&multicast_ttl,
                     (socklen_t)sizeof(multicast_ttl));

    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)AP_MDNS_PORT);
    destination.sin_addr.s_addr = htonl(AP_MDNS_ADDRESS);

    query_id = 0xA11AU;
    query_len = ap_build_service_query(g_workspace.packet,
                                       sizeof(g_workspace.packet), query_id);
    if (query_len == 0U ||
        !ap_send_query(sock, g_workspace.packet, query_len, &destination)) {
        CloseSocket(sock);
        if (g_last_error[0] == '\0') ap_discovery_set_error("Could not build mDNS query");
        return 0;
    }

    if (!ap_receive_rounds(sock, AP_DISCOVERY_ROUNDS)) {
        CloseSocket(sock);
        return 0;
    }

    /* One retransmission keeps discovery useful on lossy 10/100-Mbit LANs. */
    query_len = ap_build_service_query(g_workspace.packet,
                                       sizeof(g_workspace.packet), query_id);
    if (query_len == 0U ||
        !ap_send_query(sock, g_workspace.packet, query_len, &destination) ||
        !ap_receive_rounds(sock, AP_DISCOVERY_ROUNDS)) {
        CloseSocket(sock);
        return 0;
    }

    a_queries = ap_send_missing_detail_queries(sock, &destination, &query_id);
    if (a_queries < 0) {
        CloseSocket(sock);
        return 0;
    }
    if (a_queries > 0 && !ap_receive_rounds(sock, AP_DISCOVERY_DETAIL_ROUNDS)) {
        CloseSocket(sock);
        return 0;
    }

    a_queries = ap_send_missing_a_queries(sock, &destination, &query_id);
    if (a_queries < 0) {
        CloseSocket(sock);
        return 0;
    }
    if (a_queries > 0 && !ap_receive_rounds(sock, AP_DISCOVERY_A_ROUNDS)) {
        CloseSocket(sock);
        return 0;
    }

    CloseSocket(sock);
    ap_finalize_results(result);
    return 1;
}
