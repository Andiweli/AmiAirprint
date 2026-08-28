#ifndef AIRPRINT_ADVANCED_H
#define AIRPRINT_ADVANCED_H

#include "airprint_caps.h"
#include "airprint_prefs.h"

#include <intuition/screens.h>

void ap_advanced_normalize_prefs(struct APPrefs *prefs,
                                 const struct APPrinterCapabilities *caps);

int ap_advanced_requester(struct Screen *screen,
                          struct APPrefs *prefs,
                          const struct APPrinterCapabilities *caps);

#endif
