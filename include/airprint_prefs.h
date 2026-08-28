#ifndef AIRPRINT_PREFS_H
#define AIRPRINT_PREFS_H

#include "airprint_caps.h"

#include <stddef.h>

#define AP_PREFS_HOST_LEN    64
#define AP_PREFS_PATH_LEN    128
#define AP_PREFS_VALUE_LEN   96
#define AP_PREFS_ENGINE_LEN  20

struct APPrefs {
    char host[AP_PREFS_HOST_LEN];
    unsigned int port;
    char path[AP_PREFS_PATH_LEN];
    char engine[AP_PREFS_ENGINE_LEN];
    char color_mode[AP_PREFS_VALUE_LEN];
    unsigned int quality;
    char media[AP_PREFS_VALUE_LEN];
    char media_source[AP_PREFS_VALUE_LEN];
    char sides[AP_PREFS_VALUE_LEN];
    struct APResolution resolution;
    char orientation[AP_PREFS_VALUE_LEN];
    unsigned int scale_percent;
    int center_on_paper;
};

void ap_prefs_defaults(struct APPrefs *prefs);

int ap_prefs_load(
    struct APPrefs *prefs,
    struct APPrinterCapabilities *caps,
    int *caps_valid);

int ap_prefs_write_env(
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid);

int ap_prefs_write_envarc(
    const struct APPrefs *prefs,
    const struct APPrinterCapabilities *caps,
    int caps_valid);

#endif
