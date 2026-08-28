#include "airprint_advanced.h"
#include "ami_airprint_locale.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <string.h>

extern struct IntuitionBase *IntuitionBase;
extern struct Library *GadToolsBase;

enum {
    AGID_ENGINE = 2001,
    AGID_RESOLUTION,
    AGID_SIDES,
    AGID_SOURCE,
    AGID_USE,
    AGID_CANCEL
};

#define AP_ADV_ENGINE_MAX 3U
#define AP_ADV_SIDES_MAX  3U
#define AP_ADV_SOURCE_MAX (AP_CAPS_MAX_MEDIA_SOURCES + 1U)
#define AP_ADV_LABEL_LEN   64U

struct APAdvancedState {
    STRPTR engine_labels[AP_ADV_ENGINE_MAX + 1U];
    const char *engine_keys[AP_ADV_ENGINE_MAX];
    unsigned int engine_count;

    STRPTR resolution_labels[AP_CAPS_MAX_RESOLUTIONS + 2U];
    char resolution_storage[AP_CAPS_MAX_RESOLUTIONS + 1U][AP_ADV_LABEL_LEN];
    struct APResolution resolutions[AP_CAPS_MAX_RESOLUTIONS + 1U];
    unsigned int resolution_count;

    STRPTR sides_labels[AP_ADV_SIDES_MAX + 1U];
    const char *sides_keys[AP_ADV_SIDES_MAX];
    unsigned int sides_count;

    STRPTR source_labels[AP_ADV_SOURCE_MAX + 1U];
    const char *source_keys[AP_ADV_SOURCE_MAX];
    unsigned int source_count;
};

static struct APAdvancedState g_advanced;

static int ap_adv_resolution_eligible(const struct APResolution *resolution)
{
    return resolution != NULL && resolution->units == 3U &&
           resolution->x == resolution->y &&
           resolution->x >= 72U && resolution->x <= 2400U;
}

static int ap_adv_resolution_equal(const struct APResolution *a,
                                   const struct APResolution *b)
{
    return a != NULL && b != NULL &&
           a->x == b->x && a->y == b->y && a->units == b->units;
}

static int ap_adv_source_supported(const struct APPrinterCapabilities *caps,
                                   const char *source)
{
    unsigned int i;
    if (source == NULL || source[0] == '\0') return 1;
    if (caps == NULL) return 0;
    for (i = 0U; i < caps->media_source_count; ++i) {
        if (strcmp(caps->media_sources[i], source) == 0) return 1;
    }
    return 0;
}

static int ap_adv_engine_supported(const struct APPrinterCapabilities *caps,
                                   const char *engine)
{
    if (caps == NULL || engine == NULL) return 0;
    if (strcmp(engine, "pwg-raster") == 0) return caps->format_pwg_raster_supported;
    if (strcmp(engine, "pdf") == 0) return caps->format_pdf_supported;
    if (strcmp(engine, "postscript") == 0) return caps->format_postscript_supported;
    return 0;
}

void ap_advanced_normalize_prefs(struct APPrefs *prefs,
                                 const struct APPrinterCapabilities *caps)
{
    unsigned int i;
    int found;

    if (prefs == NULL || caps == NULL) return;

    if (!ap_adv_engine_supported(caps, prefs->engine)) {
        if (caps->format_pwg_raster_supported)
            snprintf(prefs->engine, sizeof(prefs->engine), "pwg-raster");
        else if (caps->format_pdf_supported)
            snprintf(prefs->engine, sizeof(prefs->engine), "pdf");
        else if (caps->format_postscript_supported)
            snprintf(prefs->engine, sizeof(prefs->engine), "postscript");
        else
            snprintf(prefs->engine, sizeof(prefs->engine), "pwg-raster");
    }

    found = 0;
    if (ap_adv_resolution_eligible(&prefs->resolution)) {
        for (i = 0U; i < caps->resolution_count; ++i) {
            if (ap_adv_resolution_equal(&prefs->resolution, &caps->resolutions[i])) {
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        memset(&prefs->resolution, 0, sizeof(prefs->resolution));
        prefs->resolution.units = 3U;
        if (ap_adv_resolution_eligible(&caps->resolution_default)) {
            prefs->resolution = caps->resolution_default;
        } else {
            for (i = 0U; i < caps->resolution_count; ++i) {
                if (ap_adv_resolution_eligible(&caps->resolutions[i])) {
                    prefs->resolution = caps->resolutions[i];
                    break;
                }
            }
        }
    }

    if (strcmp(prefs->engine, "pwg-raster") != 0) {
        snprintf(prefs->sides, sizeof(prefs->sides), "one-sided");
    } else if (strcmp(prefs->sides, "two-sided-long-edge") == 0) {
        if (!caps->duplex_long_edge_supported)
            snprintf(prefs->sides, sizeof(prefs->sides), "one-sided");
    } else if (strcmp(prefs->sides, "two-sided-short-edge") == 0) {
        if (!caps->duplex_short_edge_supported)
            snprintf(prefs->sides, sizeof(prefs->sides), "one-sided");
    } else {
        snprintf(prefs->sides, sizeof(prefs->sides), "one-sided");
    }

    if (!ap_adv_source_supported(caps, prefs->media_source))
        prefs->media_source[0] = '\0';
}

static unsigned int ap_adv_find_key(const char *const *keys,
                                    unsigned int count,
                                    const char *value)
{
    unsigned int i;
    if (value == NULL) value = "";
    for (i = 0U; i < count; ++i) {
        if (keys[i] != NULL && strcmp(keys[i], value) == 0) return i;
    }
    return 0U;
}

static unsigned int ap_adv_find_resolution(const struct APPrefs *prefs)
{
    unsigned int i;
    for (i = 0U; i < g_advanced.resolution_count; ++i) {
        if (ap_adv_resolution_equal(&prefs->resolution, &g_advanced.resolutions[i]))
            return i;
    }
    return 0U;
}

static void ap_adv_build_options(const struct APPrinterCapabilities *caps)
{
    unsigned int i;

    memset(&g_advanced, 0, sizeof(g_advanced));

    if (caps->format_pwg_raster_supported) {
        g_advanced.engine_labels[g_advanced.engine_count] =
            (STRPTR)AP_TR(MSG_ENGINE_PWG, "PWG Raster");
        g_advanced.engine_keys[g_advanced.engine_count++] = "pwg-raster";
    }
    if (caps->format_pdf_supported) {
        g_advanced.engine_labels[g_advanced.engine_count] =
            (STRPTR)AP_TR(MSG_ENGINE_PDF, "PDF");
        g_advanced.engine_keys[g_advanced.engine_count++] = "pdf";
    }
    if (caps->format_postscript_supported) {
        g_advanced.engine_labels[g_advanced.engine_count] =
            (STRPTR)AP_TR(MSG_ENGINE_POSTSCRIPT, "PostScript");
        g_advanced.engine_keys[g_advanced.engine_count++] = "postscript";
    }
    if (g_advanced.engine_count == 0U) {
        g_advanced.engine_labels[0] = (STRPTR)AP_TR(MSG_ENGINE_PWG, "PWG Raster");
        g_advanced.engine_keys[0] = "pwg-raster";
        g_advanced.engine_count = 1U;
    }
    g_advanced.engine_labels[g_advanced.engine_count] = NULL;

    if (ap_adv_resolution_eligible(&caps->resolution_default)) {
        g_advanced.resolutions[0] = caps->resolution_default;
        snprintf(g_advanced.resolution_storage[0],
                 sizeof(g_advanced.resolution_storage[0]), "%lu dpi",
                 (unsigned long)caps->resolution_default.x);
        g_advanced.resolution_labels[0] = (STRPTR)g_advanced.resolution_storage[0];
        g_advanced.resolution_count = 1U;
    }

    for (i = 0U; i < caps->resolution_count &&
                 g_advanced.resolution_count < AP_CAPS_MAX_RESOLUTIONS; ++i) {
        unsigned int j;
        int duplicate = 0;
        if (!ap_adv_resolution_eligible(&caps->resolutions[i])) continue;
        for (j = 0U; j < g_advanced.resolution_count; ++j) {
            if (ap_adv_resolution_equal(&caps->resolutions[i], &g_advanced.resolutions[j])) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        g_advanced.resolutions[g_advanced.resolution_count] = caps->resolutions[i];
        snprintf(g_advanced.resolution_storage[g_advanced.resolution_count],
                 sizeof(g_advanced.resolution_storage[0]), "%lu dpi",
                 (unsigned long)caps->resolutions[i].x);
        g_advanced.resolution_labels[g_advanced.resolution_count] =
            (STRPTR)g_advanced.resolution_storage[g_advanced.resolution_count];
        ++g_advanced.resolution_count;
    }
    if (g_advanced.resolution_count == 0U) {
        memset(&g_advanced.resolutions[0], 0, sizeof(g_advanced.resolutions[0]));
        g_advanced.resolution_labels[0] = (STRPTR)AP_TR(MSG_DRIVER_FALLBACK_600, "600 dpi (driver fallback)");
        g_advanced.resolution_count = 1U;
    }
    g_advanced.resolution_labels[g_advanced.resolution_count] = NULL;

    g_advanced.sides_labels[0] = (STRPTR)AP_TR(MSG_SIDES_ONE, "One-sided");
    g_advanced.sides_keys[0] = "one-sided";
    g_advanced.sides_count = 1U;
    if (caps->duplex_long_edge_supported) {
        g_advanced.sides_labels[g_advanced.sides_count] =
            (STRPTR)AP_TR(MSG_SIDES_LONG, "Two-sided (long edge)");
        g_advanced.sides_keys[g_advanced.sides_count++] = "two-sided-long-edge";
    }
    if (caps->duplex_short_edge_supported) {
        g_advanced.sides_labels[g_advanced.sides_count] =
            (STRPTR)AP_TR(MSG_SIDES_SHORT, "Two-sided (short edge)");
        g_advanced.sides_keys[g_advanced.sides_count++] = "two-sided-short-edge";
    }
    g_advanced.sides_labels[g_advanced.sides_count] = NULL;

    g_advanced.source_labels[0] = (STRPTR)AP_TR(MSG_PRINTER_DEFAULT, "Printer default");
    g_advanced.source_keys[0] = "";
    g_advanced.source_count = 1U;
    for (i = 0U; i < caps->media_source_count &&
                 g_advanced.source_count < AP_ADV_SOURCE_MAX; ++i) {
        g_advanced.source_labels[g_advanced.source_count] = (STRPTR)caps->media_sources[i];
        g_advanced.source_keys[g_advanced.source_count] = caps->media_sources[i];
        ++g_advanced.source_count;
    }
    g_advanced.source_labels[g_advanced.source_count] = NULL;
}

static struct Gadget *ap_adv_add(struct Screen *screen, APTR visual,
                                 struct Gadget *previous, ULONG kind, UWORD id,
                                 STRPTR label, UWORD left, UWORD top,
                                 UWORD width, UWORD height,
                                 struct TagItem *tags)
{
    struct NewGadget ng;
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = left;
    ng.ng_TopEdge = top;
    ng.ng_Width = width;
    ng.ng_Height = height;
    ng.ng_GadgetText = label;
    ng.ng_TextAttr = screen->Font;
    ng.ng_GadgetID = id;
    ng.ng_Flags = (kind == BUTTON_KIND) ? PLACETEXT_IN : PLACETEXT_LEFT;
    ng.ng_VisualInfo = visual;
    return CreateGadgetA(kind, previous, &ng, tags);
}

int ap_advanced_requester(struct Screen *screen,
                          struct APPrefs *prefs,
                          const struct APPrinterCapabilities *caps)
{
    struct Gadget *glist = NULL;
    struct Gadget *previous;
    struct Gadget *engine_gadget = NULL;
    struct Gadget *resolution_gadget = NULL;
    struct Gadget *sides_gadget = NULL;
    struct Gadget *source_gadget = NULL;
    APTR visual;
    struct Window *window;
    struct TagItem tags[5];
    struct TagItem window_tags[12];
    ULONG screen_width;
    ULONG screen_height;
    UWORD width;
    UWORD height;
    UWORD row_height;
    UWORD field_x;
    UWORD field_w;
    UWORD y;
    LONG left;
    LONG top;
    int done = 0;
    int accepted = 0;

    if (screen == NULL || prefs == NULL || caps == NULL ||
        IntuitionBase == NULL || GadToolsBase == NULL) return 0;

    ap_advanced_normalize_prefs(prefs, caps);
    ap_adv_build_options(caps);

    screen_width = (ULONG)screen->Width;
    screen_height = (ULONG)screen->Height;
    width = (UWORD)(screen_width > 500UL ? 480UL : screen_width - 12UL);
    if (width < 300U) width = (UWORD)(screen_width - 8UL);
    row_height = (UWORD)(screen->Font->ta_YSize + 9U);
    if (row_height < 18U) row_height = 18U;
    height = (UWORD)(screen->WBorTop + screen->Font->ta_YSize + 18U +
                     row_height * 5U + 12U);
    if ((ULONG)height > screen_height - 8UL) height = (UWORD)(screen_height - 8UL);

    visual = GetVisualInfoA(screen, NULL);
    if (visual == NULL) return 0;
    previous = CreateContext(&glist);
    if (previous == NULL) { FreeVisualInfo(visual); return 0; }

    field_x = 150U;
    if (field_x + 20U >= width) field_x = 112U;
    field_w = (UWORD)(width - field_x - 16U);
    y = (UWORD)(screen->WBorTop + screen->Font->ta_YSize + 10U);

    tags[0].ti_Tag = GTCY_Labels; tags[0].ti_Data = (ULONG)g_advanced.engine_labels;
    tags[1].ti_Tag = GTCY_Active; tags[1].ti_Data = ap_adv_find_key(g_advanced.engine_keys, g_advanced.engine_count, prefs->engine);
    tags[2].ti_Tag = GA_Disabled; tags[2].ti_Data = g_advanced.engine_count <= 1U ? TRUE : FALSE;
    tags[3].ti_Tag = TAG_END; tags[3].ti_Data = 0UL;
    previous = engine_gadget = ap_adv_add(screen, visual, previous, CYCLE_KIND, AGID_ENGINE,
        (STRPTR)AP_TR(MSG_LABEL_ENGINE, "Output format"), field_x, y, field_w, row_height - 3U, tags);
    if (previous == NULL) goto cleanup;

    y = (UWORD)(y + row_height);
    tags[0].ti_Tag = GTCY_Labels; tags[0].ti_Data = (ULONG)g_advanced.resolution_labels;
    tags[1].ti_Tag = GTCY_Active; tags[1].ti_Data = ap_adv_find_resolution(prefs);
    tags[2].ti_Tag = GA_Disabled; tags[2].ti_Data = g_advanced.resolution_count <= 1U ? TRUE : FALSE;
    tags[3].ti_Tag = TAG_END; tags[3].ti_Data = 0UL;
    previous = resolution_gadget = ap_adv_add(screen, visual, previous, CYCLE_KIND, AGID_RESOLUTION,
        (STRPTR)AP_TR(MSG_LABEL_DPI, "DPI"), field_x, y, field_w, row_height - 3U, tags);
    if (previous == NULL) goto cleanup;

    y = (UWORD)(y + row_height);
    tags[0].ti_Tag = GTCY_Labels; tags[0].ti_Data = (ULONG)g_advanced.sides_labels;
    tags[1].ti_Tag = GTCY_Active; tags[1].ti_Data = ap_adv_find_key(g_advanced.sides_keys, g_advanced.sides_count, prefs->sides);
    tags[2].ti_Tag = GA_Disabled;
    tags[2].ti_Data = (strcmp(prefs->engine, "pwg-raster") != 0 ||
                       g_advanced.sides_count <= 1U) ? TRUE : FALSE;
    tags[3].ti_Tag = TAG_END; tags[3].ti_Data = 0UL;
    previous = sides_gadget = ap_adv_add(screen, visual, previous, CYCLE_KIND, AGID_SIDES,
        (STRPTR)AP_TR(MSG_LABEL_DUPLEX, "Duplex"), field_x, y, field_w, row_height - 3U, tags);
    if (previous == NULL) goto cleanup;

    y = (UWORD)(y + row_height);
    tags[0].ti_Tag = GTCY_Labels; tags[0].ti_Data = (ULONG)g_advanced.source_labels;
    tags[1].ti_Tag = GTCY_Active; tags[1].ti_Data = ap_adv_find_key(g_advanced.source_keys, g_advanced.source_count, prefs->media_source);
    tags[2].ti_Tag = GA_Disabled; tags[2].ti_Data = g_advanced.source_count <= 1U ? TRUE : FALSE;
    tags[3].ti_Tag = TAG_END; tags[3].ti_Data = 0UL;
    previous = source_gadget = ap_adv_add(screen, visual, previous, CYCLE_KIND, AGID_SOURCE,
        (STRPTR)AP_TR(MSG_LABEL_PAPER_SOURCE, "Paper source"), field_x, y, field_w, row_height - 3U, tags);
    if (previous == NULL) goto cleanup;

    y = (UWORD)(y + row_height + 2U);
    tags[0].ti_Tag = TAG_END; tags[0].ti_Data = 0UL;
    previous = ap_adv_add(screen, visual, previous, BUTTON_KIND, AGID_USE,
        (STRPTR)AP_TR(MSG_BUTTON_ADVANCED_USE, "Use"), 60U, y,
        (UWORD)((width - 140U) / 2U), row_height - 3U, tags);
    if (previous == NULL) goto cleanup;
    previous = ap_adv_add(screen, visual, previous, BUTTON_KIND, AGID_CANCEL,
        (STRPTR)AP_TR(MSG_BUTTON_DISCOVERY_CANCEL_CLASSIC, "Cancel"),
        (UWORD)(80U + (width - 140U) / 2U), y,
        (UWORD)((width - 140U) / 2U), row_height - 3U, tags);
    if (previous == NULL) goto cleanup;

    left = ((LONG)screen_width - (LONG)width) / 2L;
    top = ((LONG)screen_height - (LONG)height) / 2L;
    if (left < 0L) left = 0L;
    if (top < 0L) top = 0L;

    window_tags[0].ti_Tag = WA_Left; window_tags[0].ti_Data = (ULONG)left;
    window_tags[1].ti_Tag = WA_Top; window_tags[1].ti_Data = (ULONG)top;
    window_tags[2].ti_Tag = WA_Width; window_tags[2].ti_Data = width;
    window_tags[3].ti_Tag = WA_Height; window_tags[3].ti_Data = height;
    window_tags[4].ti_Tag = WA_Title; window_tags[4].ti_Data = (ULONG)AP_TR(MSG_ADVANCED_TITLE, "Advanced print options");
    window_tags[5].ti_Tag = WA_Gadgets; window_tags[5].ti_Data = (ULONG)glist;
    window_tags[6].ti_Tag = WA_IDCMP; window_tags[6].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW;
    window_tags[7].ti_Tag = WA_Flags; window_tags[7].ti_Data = WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET | WFLG_DRAGBAR | WFLG_SMART_REFRESH;
    window_tags[8].ti_Tag = WA_PubScreen; window_tags[8].ti_Data = (ULONG)screen;
    window_tags[9].ti_Tag = WA_AutoAdjust; window_tags[9].ti_Data = TRUE;
    window_tags[10].ti_Tag = TAG_END; window_tags[10].ti_Data = 0UL;
    window = OpenWindowTagList(NULL, window_tags);
    if (window == NULL) goto cleanup;

    GT_RefreshWindow(window, NULL);
    while (!done) {
        ULONG signals = Wait((1UL << window->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);
        struct IntuiMessage *message;
        if ((signals & SIGBREAKF_CTRL_C) != 0U) break;
        while ((message = GT_GetIMsg(window->UserPort)) != NULL) {
            ULONG cls = message->Class;
            struct Gadget *gadget = cls == IDCMP_GADGETUP ? (struct Gadget *)message->IAddress : NULL;
            UWORD id = gadget != NULL ? gadget->GadgetID : 0U;
            UWORD code = message->Code;
            if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(window); GT_EndRefresh(window, TRUE);
                GT_ReplyIMsg(message); continue;
            }
            GT_ReplyIMsg(message);
            if (cls == IDCMP_CLOSEWINDOW) done = 1;
            else if (cls == IDCMP_GADGETUP && id == AGID_ENGINE) {
                ULONG engine_index = (ULONG)code;
                int pwg = engine_index < g_advanced.engine_count &&
                          strcmp(g_advanced.engine_keys[engine_index], "pwg-raster") == 0;
                struct TagItem update[3];
                update[0].ti_Tag = GA_Disabled;
                update[0].ti_Data = (pwg && g_advanced.sides_count > 1U) ? FALSE : TRUE;
                update[1].ti_Tag = GTCY_Active; update[1].ti_Data = pwg ? ap_adv_find_key(g_advanced.sides_keys, g_advanced.sides_count, prefs->sides) : 0U;
                update[2].ti_Tag = TAG_END; update[2].ti_Data = 0UL;
                GT_SetGadgetAttrsA(sides_gadget, window, NULL, update);
            } else if (cls == IDCMP_GADGETUP && id == AGID_USE) {
                ULONG engine_index = 0U, resolution_index = 0U, sides_index = 0U, source_index = 0U;
                struct TagItem get[2];
                get[1].ti_Tag = TAG_END; get[1].ti_Data = 0UL;
                get[0].ti_Tag = GTCY_Active; get[0].ti_Data = (ULONG)&engine_index;
                (void)GT_GetGadgetAttrsA(engine_gadget, window, NULL, get);
                get[0].ti_Data = (ULONG)&resolution_index;
                (void)GT_GetGadgetAttrsA(resolution_gadget, window, NULL, get);
                get[0].ti_Data = (ULONG)&sides_index;
                (void)GT_GetGadgetAttrsA(sides_gadget, window, NULL, get);
                get[0].ti_Data = (ULONG)&source_index;
                (void)GT_GetGadgetAttrsA(source_gadget, window, NULL, get);

                if (engine_index < g_advanced.engine_count)
                    snprintf(prefs->engine, sizeof(prefs->engine), "%s", g_advanced.engine_keys[engine_index]);
                if (resolution_index < g_advanced.resolution_count)
                    prefs->resolution = g_advanced.resolutions[resolution_index];
                if (strcmp(prefs->engine, "pwg-raster") == 0 && sides_index < g_advanced.sides_count)
                    snprintf(prefs->sides, sizeof(prefs->sides), "%s", g_advanced.sides_keys[sides_index]);
                else
                    snprintf(prefs->sides, sizeof(prefs->sides), "one-sided");
                if (source_index < g_advanced.source_count)
                    snprintf(prefs->media_source, sizeof(prefs->media_source), "%s", g_advanced.source_keys[source_index]);
                ap_advanced_normalize_prefs(prefs, caps);
                accepted = 1;
                done = 1;
            } else if (cls == IDCMP_GADGETUP && id == AGID_CANCEL) {
                done = 1;
            }
        }
    }

    CloseWindow(window);
cleanup:
    if (glist != NULL) FreeGadgets(glist);
    FreeVisualInfo(visual);
    return accepted;
}
