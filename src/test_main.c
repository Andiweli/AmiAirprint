#include "airprint_http.h"
#include "airprint_prefs.h"
#include "airprint_print.h"
#include "testpage_jpeg.h"
#include "ami_airprint_version.h"

#include <stdio.h>
#include <string.h>

#define AIRPRINT_TEST_VERSION AMIAIRPRINT_CORE_VERSION_TEXT

static struct APPrefs g_test_prefs;
static struct APPrinterCapabilities g_test_caps;
static struct APPrintResult g_test_result;

static const char *ap_quality_name(unsigned int quality)
{
    switch (quality) {
        case 3U: return "draft";
        case 4U: return "normal";
        case 5U: return "high";
        default: return "unknown";
    }
}

int main(void)
{
    int caps_valid;
    char error_text[192];

    caps_valid = 0;
    ap_prefs_load(&g_test_prefs, &g_test_caps, &caps_valid);

    printf("AirPrintTest %s\n", AIRPRINT_TEST_VERSION);
    printf("Direct IPP Print-Job test for AmigaOS\n\n");

    if (g_test_prefs.host[0] == '\0') {
        printf("No printer is configured.\n");
        printf("Run AmiAirPrint, query the printer and choose Save first.\n");
        return 10;
    }

    printf("Printer: %s\n",
           caps_valid && g_test_caps.model[0] != '\0' ? g_test_caps.model : "configured IPP printer");
    printf("Target:  ipp://%s:%u%s\n", g_test_prefs.host, g_test_prefs.port, g_test_prefs.path);
    printf("Format:  image/jpeg (%lu bytes)\n",
           (unsigned long)g_airprint_testpage_jpeg_len);
    printf("Color:   %s\n", g_test_prefs.color_mode[0] != '\0' ? g_test_prefs.color_mode : "printer default");
    printf("Quality: %s (%u)\n", ap_quality_name(g_test_prefs.quality), g_test_prefs.quality);
    printf("Paper:   %s\n", g_test_prefs.media[0] != '\0' ? g_test_prefs.media : "printer default");
    if (caps_valid && g_test_caps.resolution_default.x != 0U) {
        printf("Output:  %lux%lu dpi\n",
               (unsigned long)g_test_caps.resolution_default.x,
               (unsigned long)g_test_caps.resolution_default.y);
    }
    printf("\nSubmitting test page...\n");

    if (!ap_http_open()) {
        printf("ERROR: %s\n", ap_http_last_error());
        return 20;
    }

    memset(&g_test_result, 0, sizeof(g_test_result));
    error_text[0] = '\0';

    if (!ap_print_document(&g_test_prefs,
                           &g_test_caps,
                           caps_valid,
                           g_airprint_testpage_jpeg,
                           g_airprint_testpage_jpeg_len,
                           "image/jpeg",
                           "AmigaOS AirPrint Test Page",
                           &g_test_result,
                           error_text,
                           sizeof(error_text))) {
        printf("ERROR: %s\n", error_text[0] != '\0' ? error_text : "print failed");
        ap_http_close();
        return 20;
    }

    ap_http_close();

    printf("Print job accepted. IPP status 0x%04X\n", (unsigned int)g_test_result.ipp_status);
    if (g_test_result.job_id != 0UL) {
        printf("Job ID: %lu\n", g_test_result.job_id);
    }
    if (g_test_result.job_uri[0] != '\0') {
        printf("Job URI: %s\n", g_test_result.job_uri);
    }
    if (g_test_result.job_state != 0UL) {
        printf("Job state: %lu\n", g_test_result.job_state);
    }
    if (g_test_result.status_message[0] != '\0') {
        printf("Printer message: %s\n", g_test_result.status_message);
    }

    printf("\nThe printer should now produce the AirPrint/AmigaOS test page.\n");
    return 0;
}
