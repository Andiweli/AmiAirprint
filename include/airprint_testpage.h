#ifndef AIRPRINT_TESTPAGE_H
#define AIRPRINT_TESTPAGE_H

#include "airprint_caps.h"
#include "airprint_prefs.h"

#include <stddef.h>
#include <stdint.h>

struct APTestPageDocument {
    const uint8_t *data;
    size_t length;
    char format[32];
    uint8_t *allocated;
};

int ap_testpage_build(const struct APPrefs *prefs,
                      const struct APPrinterCapabilities *caps,
                      int caps_valid,
                      struct APTestPageDocument *document,
                      char *error_text,
                      size_t error_text_size);

void ap_testpage_free(struct APTestPageDocument *document);

#endif
