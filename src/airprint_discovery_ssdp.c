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

#define AP_SSDP_PORT       1900U
#define AP_SSDP_ADDRESS    0xEFFFFFFAUL /* 239.255.255.250 */
#define AP_SSDP_PACKET_MAX 1024U
#define AP_SSDP_WAIT_USEC  250000L
#define AP_SSDP_ROUNDS     4U

static const char *g_ssdp_targets[] = {
    "urn:schemas-upnp-org:device:Printer:1",
    "urn:schemas-upnp-org:service:PrintBasic:1"
};

/* Discovery is synchronous, so one static receive buffer avoids placing a
 * 1 KiB datagram buffer on the small classic Amiga stack. */
static char g_ssdp_packet[AP_SSDP_PACKET_MAX];

static int ap_ssdp_same_address(const struct APDiscoveryResult *result,
                                const char *address)
{
    unsigned int i;
    if (result == NULL || address == NULL) return 0;
    for (i = 0U; i < result->count; ++i) {
        if (strcmp(result->printers[i].address, address) == 0) return 1;
    }
    return 0;
}

static void ap_ssdp_add(struct APDiscoveryResult *result,
                        const struct sockaddr_in *source)
{
    const UBYTE *bytes;
    char address[AP_DISCOVERY_ADDRESS_LEN];
    struct APDiscoveredPrinter *printer;

    if (result == NULL || source == NULL ||
        result->count >= AP_DISCOVERY_MAX_PRINTERS) return;

    bytes = (const UBYTE *)&source->sin_addr.s_addr;
    snprintf(address, sizeof(address), "%u.%u.%u.%u",
             (unsigned int)bytes[0], (unsigned int)bytes[1],
             (unsigned int)bytes[2], (unsigned int)bytes[3]);
    if (strcmp(address, "0.0.0.0") == 0 ||
        strncmp(address, "127.", 4U) == 0 ||
        ap_ssdp_same_address(result, address)) return;

    printer = &result->printers[result->count++];
    memset(printer, 0, sizeof(*printer));
    snprintf(printer->name, sizeof(printer->name), "IPP printer (%s)", address);
    snprintf(printer->host_name, sizeof(printer->host_name), "%s", address);
    snprintf(printer->address, sizeof(printer->address), "%s", address);
    snprintf(printer->path, sizeof(printer->path), "/ipp/print");
    printer->port = 631U;
    printer->airprint = 0;
}

static int ap_ssdp_send(int sock,
                        struct sockaddr_in *destination,
                        const char *target)
{
    char request[512];
    int length;
    LONG sent;

    length = snprintf(request, sizeof(request),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: %s\r\n\r\n", target);
    if (length <= 0 || (size_t)length >= sizeof(request)) return 0;

    sent = sendto(sock, request, (LONG)length, 0,
                  (struct sockaddr *)destination,
                  (socklen_t)sizeof(*destination));
    return sent == (LONG)length;
}

static void ap_ssdp_receive(struct APDiscoveryResult *result, int sock)
{
    unsigned int round;

    for (round = 0U; round < AP_SSDP_ROUNDS; ++round) {
        fd_set read_set;
        struct timeval timeout;
        LONG ready;

        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        timeout.tv_sec = 0L;
        timeout.tv_usec = AP_SSDP_WAIT_USEC;
        ready = WaitSelect((LONG)sock + 1L, &read_set, NULL, NULL, &timeout, NULL);
        if (ready <= 0L || !FD_ISSET(sock, &read_set)) continue;

        for (;;) {
            struct sockaddr_in source;
            socklen_t source_len;
            LONG received;

            memset(&source, 0, sizeof(source));
            source_len = (socklen_t)sizeof(source);
            received = recvfrom(sock, g_ssdp_packet, (LONG)(sizeof(g_ssdp_packet) - 1U), 0,
                                (struct sockaddr *)&source, &source_len);
            if (received <= 0L) break;
            g_ssdp_packet[received] = '\0';
            if (strncmp(g_ssdp_packet, "HTTP/1.1 200", 12U) == 0 ||
                strncmp(g_ssdp_packet, "HTTP/1.0 200", 12U) == 0)
                ap_ssdp_add(result, &source);

            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            timeout.tv_sec = 0L;
            timeout.tv_usec = 0L;
            ready = WaitSelect((LONG)sock + 1L, &read_set, NULL, NULL, &timeout, NULL);
            if (ready <= 0L || !FD_ISSET(sock, &read_set)) break;
        }
    }
}

static int ap_discovery_ssdp(struct APDiscoveryResult *result)
{
    struct sockaddr_in destination;
    int sock;
    unsigned int i;

    unsigned int before;

    if (result == NULL || SocketBase == NULL ||
        result->count >= AP_DISCOVERY_MAX_PRINTERS) return 0;

    before = result->count;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;

    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)AP_SSDP_PORT);
    destination.sin_addr.s_addr = htonl(AP_SSDP_ADDRESS);

    for (i = 0U; i < sizeof(g_ssdp_targets) / sizeof(g_ssdp_targets[0]); ++i) {
        if (ap_ssdp_send(sock, &destination, g_ssdp_targets[i]))
            ap_ssdp_receive(result, sock);
        if (result->count >= AP_DISCOVERY_MAX_PRINTERS) break;
    }

    CloseSocket(sock);
    return result->count > before;
}

int ap_discovery_search(struct APDiscoveryResult *result)
{
    int mdns_ok;

    if (result == NULL) return 0;
    mdns_ok = ap_discovery_search_mdns(result);
    if (!mdns_ok) memset(result, 0, sizeof(*result));
    if (result->count < AP_DISCOVERY_MAX_PRINTERS)
        (void)ap_discovery_ssdp(result);
    return mdns_ok || result->count != 0U;
}
