#include "ami_airprint_locale.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/locale.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/locale.h>

#include <string.h>

struct LocaleBase *LocaleBase = NULL;

static struct Catalog *g_catalog = NULL;

void ap_locale_open(void)
{
    struct TagItem catalog_tags[4];

    if (LocaleBase != NULL) {
        return;
    }

    LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38L);
    if (LocaleBase == NULL) {
        return;
    }

    catalog_tags[0].ti_Tag = OC_BuiltInLanguage;
    catalog_tags[0].ti_Data = (ULONG)(STRPTR)"english";
    catalog_tags[1].ti_Tag = OC_BuiltInCodeSet;
    catalog_tags[1].ti_Data = 0UL;
    catalog_tags[2].ti_Tag = OC_Version;
    catalog_tags[2].ti_Data = 1UL;
    catalog_tags[3].ti_Tag = TAG_DONE;
    catalog_tags[3].ti_Data = 0UL;

    g_catalog = OpenCatalogA(NULL, (STRPTR)"AmiAirPrint.catalog", catalog_tags);
}

void ap_locale_close(void)
{
    if (g_catalog != NULL && LocaleBase != NULL) {
        CloseCatalog(g_catalog);
    }
    g_catalog = NULL;

    if (LocaleBase != NULL) {
        CloseLibrary((struct Library *)LocaleBase);
    }
    LocaleBase = NULL;
}

const char *ap_locale_get(ULONG id, const char *fallback)
{
    if (fallback == NULL) {
        fallback = "";
    }

    if (LocaleBase != NULL && g_catalog != NULL) {
        return (const char *)GetCatalogStr(g_catalog, (LONG)id, (STRPTR)fallback);
    }

    return fallback;
}

const char *ap_locale_color_name(const char *key)
{
    if (key == NULL) return "";
    if (strcmp(key, "color") == 0) return AP_TR(MSG_COLOR, "Color");
    if (strcmp(key, "monochrome") == 0) return AP_TR(MSG_MONOCHROME, "Monochrome");
    if (strcmp(key, "auto") == 0) return AP_TR(MSG_AUTOMATIC, "Automatic");
    if (strcmp(key, "auto-monochrome") == 0) return AP_TR(MSG_AUTO_MONOCHROME, "Auto monochrome");
    return key;
}

const char *ap_locale_media_name(const char *keyword)
{
    if (keyword == NULL) return "";
    if (strcmp(keyword, "iso_a4_210x297mm") == 0) return AP_TR(MSG_MEDIA_A4, "A4");
    if (strcmp(keyword, "iso_a5_148x210mm") == 0) return AP_TR(MSG_MEDIA_A5, "A5");
    if (strcmp(keyword, "jis_b5_182x257mm") == 0) return AP_TR(MSG_MEDIA_B5_JIS, "B5 (JIS)");
    if (strcmp(keyword, "na_letter_8.5x11in") == 0) return AP_TR(MSG_MEDIA_LETTER, "Letter");
    if (strcmp(keyword, "na_legal_8.5x14in") == 0) return AP_TR(MSG_MEDIA_LEGAL, "Legal");
    if (strcmp(keyword, "na_index-4x6_4x6in") == 0) return AP_TR(MSG_MEDIA_PHOTO_4X6, "Photo 4 x 6 in");
    if (strcmp(keyword, "na_5x7_5x7in") == 0) return AP_TR(MSG_MEDIA_PHOTO_5X7, "Photo 5 x 7 in");
    if (strcmp(keyword, "oe_photo-l_3.5x5in") == 0) return AP_TR(MSG_MEDIA_PHOTO_L, "Photo L 3.5 x 5 in");
    if (strcmp(keyword, "oe_square-photo_5x5in") == 0) return AP_TR(MSG_MEDIA_SQUARE_5X5, "Square 5 x 5 in");
    return keyword;
}

const char *ap_locale_state_name(unsigned int printer_state)
{
    switch (printer_state) {
        case 3U: return AP_TR(MSG_STATE_READY, "Ready");
        case 4U: return AP_TR(MSG_STATE_PRINTING, "Printing");
        case 5U: return AP_TR(MSG_STATE_STOPPED, "Stopped");
        default: return AP_TR(MSG_STATE_UNKNOWN, "Unknown");
    }
}
