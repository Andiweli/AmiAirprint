#ifndef AIRPRINT_DISCOVERY_H
#define AIRPRINT_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#define AP_DISCOVERY_MAX_PRINTERS 12U
#define AP_DISCOVERY_NAME_LEN     128U
#define AP_DISCOVERY_HOST_LEN     128U
#define AP_DISCOVERY_ADDRESS_LEN   16U
#define AP_DISCOVERY_PATH_LEN     128U

struct APDiscoveredPrinter {
    char name[AP_DISCOVERY_NAME_LEN];
    char host_name[AP_DISCOVERY_HOST_LEN];
    char address[AP_DISCOVERY_ADDRESS_LEN];
    char path[AP_DISCOVERY_PATH_LEN];
    uint16_t port;
    int airprint;
};

struct APDiscoveryResult {
    unsigned int count;
    struct APDiscoveredPrinter printers[AP_DISCOVERY_MAX_PRINTERS];
};

/*
 * Searches the local link for DNS-SD IPP services using one-shot mDNS.
 * Only plain _ipp._tcp endpoints are returned because AmiAirPrint's current
 * transport is HTTP/IPP; _ipps._tcp records are observed only to avoid
 * presenting an unusable TLS-only endpoint.
 *
 * bsdsocket.library must already be open through ap_http_open().
 */
/* Combined search: existing mDNS/DNS-SD plus a supplementary SSDP pass. */
int ap_discovery_search(struct APDiscoveryResult *result);
/* Internal mDNS implementation used by the combined search wrapper. */
int ap_discovery_search_mdns(struct APDiscoveryResult *result);
const char *ap_discovery_last_error(void);

#endif
