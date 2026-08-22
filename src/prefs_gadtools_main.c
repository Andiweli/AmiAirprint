#include "airprint_caps.h"
#include "airprint_discovery.h"
#include "ami_airprint_brand.h"
#include "airprint_http.h"
#include "airprint_prefs.h"
#include "airprint_print.h"
#include "testpage_jpeg.h"
#include "ami_airprint_version.h"
#include "ami_airprint_locale.h"

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <string.h>

#define AIRPRINT_PREFS_CLASSIC_VERSION AMIAIRPRINT_PREFS_VERSION_TEXT
#define AP_GUI_MEDIA_LABEL_LEN 80
#define AP_MIN_WINDOW_WIDTH 560U
#define AP_MAX_WINDOW_WIDTH 620U
#define AP_MIN_WINDOW_HEIGHT 180U
#define AP_EDGE_MARGIN 10U
#define AP_LABEL_GAP 12U
#define AP_COLUMN_GAP 14U
#define AP_ROW_CONTROL_GAP 10U
#define AP_BOTTOM_BUTTON_GAP 16U
#define AP_BUTTON_TEXT_PADDING 24U
#define AP_MIN_COLUMN_GADGET_WIDTH 72U
#define AP_MIN_FULL_GADGET_WIDTH 180U
#define AP_MIN_HOST_GADGET_WIDTH 96U
#define AP_PORT_GADGET_WIDTH 58U
#define AP_SCALE_GADGET_WIDTH 62U

const char AmiAirPrintClassicBrand[] __attribute__((used)) = AMIAIRPRINT_BRAND_TEXT;
const char AmiAirPrintClassicVersionTag[] __attribute__((used)) =
    "$VER: AmiAirPrintPrefsClassic " AMIAIRPRINT_PREFS_VERSION_TEXT " (" AMIAIRPRINT_PREFS_VERSION_DATE ")\r\n" AMIAIRPRINT_BRAND_TEXT;

struct IntuitionBase *IntuitionBase = NULL;
struct Library *GadToolsBase = NULL;

enum {
    GID_HOST = 1,
    GID_PORT,
    GID_PATH,
    GID_QUERY,
    GID_SEARCH,
    GID_MODEL,
    GID_STATUS,
    GID_AIRPRINT,
    GID_RESOLUTION,
    GID_DUPLEX,
    GID_INK,
    GID_COLOR,
    GID_QUALITY,
    GID_MEDIA,
    GID_ORIENTATION,
    GID_SCALE,
    GID_CENTER,
    GID_TESTPAGE,
    GID_SAVE,
    GID_CANCEL,
    GID_COUNT
};

struct APGUI {
    struct Screen *screen;
    APTR visual_info;
    struct Window *window;
    struct Gadget *glist;
    struct Gadget *gadgets[GID_COUNT];

    struct APPrefs prefs;
    struct APPrinterCapabilities caps;

    char model_text[AP_CAPS_TEXT_LEN];
    char status_text[160];
    char airprint_text[128];
    char resolution_text[128];
    char duplex_text[64];
    char ink_text[192];

    STRPTR color_labels[AP_CAPS_MAX_COLOR_MODES + 1U];
    const char *color_keys[AP_CAPS_MAX_COLOR_MODES];
    unsigned int color_count;

    STRPTR quality_labels[4];
    unsigned int quality_values[3];
    unsigned int quality_count;

    STRPTR media_labels[AP_CAPS_MAX_MEDIA + 1U];
    char media_label_storage[AP_CAPS_MAX_MEDIA][AP_GUI_MEDIA_LABEL_LEN];
    const char *media_keys[AP_CAPS_MAX_MEDIA];
    unsigned int media_count;

    int queried;

    struct APDiscoveryResult discovery;
    STRPTR discovery_labels[AP_DISCOVERY_MAX_PRINTERS + 1U];
    char discovery_label_storage[AP_DISCOVERY_MAX_PRINTERS][AP_DISCOVERY_NAME_LEN + 48U];

    UWORD window_width;
    UWORD window_height;
    UWORD row_height;
    UWORD content_top;
    int screen_locked;
};

/* Singleton GUI state lives in BSS, not on the process stack. */
static struct APGUI g_gui;

static STRPTR g_not_queried_labels[2];
static STRPTR g_orientation_labels[3];

static void ap_gui_init_locale_labels(void)
{
    g_not_queried_labels[0] = (STRPTR)AP_TR(MSG_NOT_QUERIED, "Not queried");
    g_not_queried_labels[1] = NULL;

    g_orientation_labels[0] = (STRPTR)AP_TR(MSG_PORTRAIT, "Portrait");
    g_orientation_labels[1] = (STRPTR)AP_TR(MSG_LANDSCAPE, "Landscape");
    g_orientation_labels[2] = NULL;
}

static const char *ap_color_friendly(const char *key)
{
    return ap_locale_color_name(key);
}

static int ap_gui_open_libraries(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39L);
    if (IntuitionBase == NULL) return 0;

    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39L);
    if (GadToolsBase == NULL) return 0;

    return 1;
}

static void ap_gui_close_libraries(void)
{
    if (GadToolsBase != NULL) CloseLibrary(GadToolsBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    GadToolsBase = NULL;
    IntuitionBase = NULL;
}

static void ap_gui_set_text_gadget(struct APGUI *gui, UWORD id, const char *text)
{
    struct TagItem tags[2];

    if (gui == NULL || gui->window == NULL || id >= GID_COUNT ||
        gui->gadgets[id] == NULL) return;

    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)(text != NULL ? text : "");
    tags[1].ti_Tag = TAG_END;
    tags[1].ti_Data = 0UL;
    GT_SetGadgetAttrsA(gui->gadgets[id], gui->window, NULL, tags);
}

static void ap_gui_set_status(struct APGUI *gui, const char *text)
{
    if (gui == NULL) return;
    snprintf(gui->status_text, sizeof(gui->status_text), "%s", text != NULL ? text : "");
    ap_gui_set_text_gadget(gui, GID_STATUS, gui->status_text);
}

static ULONG ap_gui_get_attr(struct APGUI *gui, UWORD id, Tag tag, ULONG fallback)
{
    struct TagItem tags[2];
    ULONG value;

    if (gui == NULL || gui->window == NULL || id >= GID_COUNT ||
        gui->gadgets[id] == NULL) return fallback;

    value = fallback;
    tags[0].ti_Tag = tag;
    tags[0].ti_Data = (ULONG)&value;
    tags[1].ti_Tag = TAG_END;
    tags[1].ti_Data = 0UL;
    if (GT_GetGadgetAttrsA(gui->gadgets[id], gui->window, NULL, tags) != 1L)
        return fallback;
    return value;
}

static STRPTR ap_gui_get_string(struct APGUI *gui, UWORD id)
{
    return (STRPTR)ap_gui_get_attr(gui, id, GTST_String, 0UL);
}

static void ap_gui_set_cycle(struct APGUI *gui, UWORD id, STRPTR *labels,
                             ULONG active, int disabled)
{
    struct TagItem tags[4];

    if (gui == NULL || gui->window == NULL || id >= GID_COUNT ||
        gui->gadgets[id] == NULL) return;

    tags[0].ti_Tag = GTCY_Labels;
    tags[0].ti_Data = (ULONG)labels;
    tags[1].ti_Tag = GTCY_Active;
    tags[1].ti_Data = active;
    tags[2].ti_Tag = GA_Disabled;
    tags[2].ti_Data = disabled ? TRUE : FALSE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    GT_SetGadgetAttrsA(gui->gadgets[id], gui->window, NULL, tags);
}

static void ap_gui_invalidate_query(struct APGUI *gui, const char *status)
{
    if (gui == NULL) return;
    gui->queried = 0;
    ap_gui_set_cycle(gui, GID_COLOR, g_not_queried_labels, 0UL, 1);
    ap_gui_set_cycle(gui, GID_QUALITY, g_not_queried_labels, 0UL, 1);
    ap_gui_set_cycle(gui, GID_MEDIA, g_not_queried_labels, 0UL, 1);
    if (status != NULL) ap_gui_set_status(gui, status);
}

static struct Gadget *ap_gui_add_gadget(struct APGUI *gui,
                                        struct Gadget *previous,
                                        ULONG kind,
                                        UWORD id,
                                        STRPTR label,
                                        UWORD left,
                                        UWORD top,
                                        UWORD width,
                                        UWORD height,
                                        UWORD flags,
                                        struct TagItem *tags)
{
    struct NewGadget ng;
    struct Gadget *gadget;

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = left;
    ng.ng_TopEdge = top;
    ng.ng_Width = width;
    ng.ng_Height = height;
    ng.ng_GadgetText = label;
    ng.ng_TextAttr = gui->screen->Font;
    ng.ng_GadgetID = id;
    ng.ng_Flags = flags;
    ng.ng_VisualInfo = gui->visual_info;
    ng.ng_UserData = NULL;

    gadget = CreateGadgetA(kind, previous, &ng, tags);
    if (gadget != NULL && id < GID_COUNT) gui->gadgets[id] = gadget;
    return gadget;
}

static unsigned int ap_gui_find_color(const struct APGUI *gui, const char *key)
{
    unsigned int i;
    for (i = 0U; i < gui->color_count; ++i) {
        if (strcmp(gui->color_keys[i], key) == 0) return i;
    }
    return 0U;
}

static unsigned int ap_gui_find_quality(const struct APGUI *gui, unsigned int quality)
{
    unsigned int i;
    for (i = 0U; i < gui->quality_count; ++i) {
        if (gui->quality_values[i] == quality) return i;
    }
    return 0U;
}

static unsigned int ap_gui_find_media(const struct APGUI *gui, const char *key)
{
    unsigned int i;
    for (i = 0U; i < gui->media_count; ++i) {
        if (strcmp(gui->media_keys[i], key) == 0) return i;
    }
    return 0U;
}

static void ap_gui_build_options(struct APGUI *gui)
{
    unsigned int i;

    gui->color_count = 0U;
    for (i = 0U; i < gui->caps.color_mode_count && i < AP_CAPS_MAX_COLOR_MODES; ++i) {
        gui->color_keys[gui->color_count] = gui->caps.color_modes[i];
        gui->color_labels[gui->color_count] = (STRPTR)ap_color_friendly(gui->caps.color_modes[i]);
        ++gui->color_count;
    }
    gui->color_labels[gui->color_count] = NULL;

    gui->quality_count = 0U;
    if (gui->caps.quality_draft) {
        gui->quality_values[gui->quality_count] = 3U;
        gui->quality_labels[gui->quality_count++] = (STRPTR)AP_TR(MSG_DRAFT, "Draft");
    }
    if (gui->caps.quality_normal) {
        gui->quality_values[gui->quality_count] = 4U;
        gui->quality_labels[gui->quality_count++] = (STRPTR)AP_TR(MSG_NORMAL, "Normal");
    }
    if (gui->caps.quality_high) {
        gui->quality_values[gui->quality_count] = 5U;
        gui->quality_labels[gui->quality_count++] = (STRPTR)AP_TR(MSG_HIGH, "High");
    }
    gui->quality_labels[gui->quality_count] = NULL;

    gui->media_count = 0U;
    for (i = 0U; i < gui->caps.media_count && i < AP_CAPS_MAX_MEDIA; ++i) {
        const char *friendly;
        if (strncmp(gui->caps.media[i], "custom_min_", 11U) == 0 ||
            strncmp(gui->caps.media[i], "custom_max_", 11U) == 0) continue;
        friendly = ap_locale_media_name(gui->caps.media[i]);
        snprintf(gui->media_label_storage[gui->media_count],
                 sizeof(gui->media_label_storage[0]), "%s", friendly);
        gui->media_keys[gui->media_count] = gui->caps.media[i];
        gui->media_labels[gui->media_count] =
            (STRPTR)gui->media_label_storage[gui->media_count];
        ++gui->media_count;
    }
    gui->media_labels[gui->media_count] = NULL;
}

static void ap_gui_format_ink(struct APGUI *gui)
{
    unsigned int i;
    size_t used;

    gui->ink_text[0] = '\0';
    used = 0U;
    for (i = 0U;
         i < gui->caps.marker_name_count && i < gui->caps.marker_level_count;
         ++i) {
        int written;
        written = snprintf(gui->ink_text + used,
                           sizeof(gui->ink_text) - used,
                           "%s%s: %d%%",
                           used != 0U ? ", " : "",
                           gui->caps.marker_names[i],
                           gui->caps.marker_levels[i]);
        if (written < 0 || (size_t)written >= sizeof(gui->ink_text) - used) break;
        used += (size_t)written;
    }
    if (gui->ink_text[0] == '\0')
        snprintf(gui->ink_text, sizeof(gui->ink_text), "%s", AP_TR(MSG_NOT_REPORTED, "Not reported"));
}

static void ap_gui_prepare_capability_text(struct APGUI *gui)
{
    snprintf(gui->model_text, sizeof(gui->model_text), "%s",
             gui->caps.model[0] != '\0' ? gui->caps.model : AP_TR(MSG_UNKNOWN_PRINTER, "Unknown printer"));

    snprintf(gui->status_text, sizeof(gui->status_text), "%s%s%s",
             ap_locale_state_name(gui->caps.printer_state),
             gui->caps.accepting_jobs == 0 ? AP_TR(MSG_SUFFIX_NOT_ACCEPTING, " - not accepting jobs") : "",
             gui->caps.state_reason[0] != '\0' && strcmp(gui->caps.state_reason, "none") != 0
                 ? AP_TR(MSG_SUFFIX_CHECK_STATUS, " - check printer status") : "");

    if (gui->caps.airprint_version[0] != '\0') {
        snprintf(gui->airprint_text, sizeof(gui->airprint_text),
                 AP_TR(MSG_FORMAT_AIRPRINT_IPP, "AirPrint %s / IPP %u.%u"),
                 gui->caps.airprint_version,
                 (unsigned int)gui->caps.ipp_version_major,
                 (unsigned int)gui->caps.ipp_version_minor);
    } else {
        snprintf(gui->airprint_text, sizeof(gui->airprint_text),
                 AP_TR(MSG_FORMAT_IPP, "IPP %u.%u"),
                 (unsigned int)gui->caps.ipp_version_major,
                 (unsigned int)gui->caps.ipp_version_minor);
    }

    if (gui->caps.resolution_default.x != 0U) {
        snprintf(gui->resolution_text, sizeof(gui->resolution_text),
                 AP_TR(MSG_FORMAT_DPI, "%lux%lu dpi"),
                 (unsigned long)gui->caps.resolution_default.x,
                 (unsigned long)gui->caps.resolution_default.y);
    } else if (gui->caps.resolution_count != 0U) {
        snprintf(gui->resolution_text, sizeof(gui->resolution_text),
                 AP_TR(MSG_FORMAT_DPI, "%lux%lu dpi"),
                 (unsigned long)gui->caps.resolutions[0].x,
                 (unsigned long)gui->caps.resolutions[0].y);
    } else {
        snprintf(gui->resolution_text, sizeof(gui->resolution_text), "%s", AP_TR(MSG_NOT_REPORTED, "Not reported"));
    }

    snprintf(gui->duplex_text, sizeof(gui->duplex_text), "%s",
             gui->caps.duplex_supported ? AP_TR(MSG_SUPPORTED, "Supported") : AP_TR(MSG_NOT_SUPPORTED, "Not supported"));
    ap_gui_format_ink(gui);
    ap_gui_build_options(gui);
}

static void ap_gui_update_capabilities(struct APGUI *gui)
{
    const char *preferred_color;
    const char *preferred_media;
    unsigned int preferred_quality;
    unsigned int color_selected;
    unsigned int quality_selected;
    unsigned int media_selected;

    ap_gui_prepare_capability_text(gui);

    preferred_color = gui->prefs.color_mode[0] != '\0'
        ? gui->prefs.color_mode : gui->caps.color_default;
    preferred_quality = gui->prefs.quality != 0U
        ? gui->prefs.quality : gui->caps.quality_default;
    preferred_media = gui->prefs.media[0] != '\0'
        ? gui->prefs.media : gui->caps.media_default;

    color_selected = ap_gui_find_color(gui, preferred_color);
    quality_selected = ap_gui_find_quality(gui, preferred_quality);
    media_selected = ap_gui_find_media(gui, preferred_media);

    ap_gui_set_text_gadget(gui, GID_MODEL, gui->model_text);
    ap_gui_set_text_gadget(gui, GID_STATUS, gui->status_text);
    ap_gui_set_text_gadget(gui, GID_AIRPRINT, gui->airprint_text);
    ap_gui_set_text_gadget(gui, GID_RESOLUTION, gui->resolution_text);
    ap_gui_set_text_gadget(gui, GID_DUPLEX, gui->duplex_text);
    ap_gui_set_text_gadget(gui, GID_INK, gui->ink_text);

    ap_gui_set_cycle(gui, GID_COLOR,
                     gui->color_count != 0U ? gui->color_labels : g_not_queried_labels,
                     color_selected, gui->color_count == 0U);
    ap_gui_set_cycle(gui, GID_QUALITY,
                     gui->quality_count != 0U ? gui->quality_labels : g_not_queried_labels,
                     quality_selected, gui->quality_count == 0U);
    ap_gui_set_cycle(gui, GID_MEDIA,
                     gui->media_count != 0U ? gui->media_labels : g_not_queried_labels,
                     media_selected, gui->media_count == 0U);
}

struct APGUIGeometry {
    UWORD full_x;
    UWORD full_w;
    UWORD left_x;
    UWORD left_w;
    UWORD right_x;
    UWORD right_w;
    UWORD query_w;
    UWORD query_x;
    UWORD search_w;
    UWORD search_x;
    UWORD path_w;
    UWORD port_x;
    UWORD host_w;
    UWORD test_w;
    UWORD save_w;
    UWORD cancel_w;
    UWORD test_x;
    UWORD save_x;
    UWORD cancel_x;
};

static ULONG ap_gui_text_width(const struct APGUI *gui, const char *text)
{
    struct IntuiText itext;
    LONG width;

    if (gui == NULL || gui->screen == NULL || gui->screen->Font == NULL ||
        text == NULL || text[0] == '\0') return 0UL;

    memset(&itext, 0, sizeof(itext));
    itext.ITextFont = gui->screen->Font;
    itext.IText = (UBYTE *)text;
    width = IntuiTextLength(&itext);
    return width > 0L ? (ULONG)width : 0UL;
}

static ULONG ap_gui_max_width(ULONG current, ULONG candidate)
{
    return candidate > current ? candidate : current;
}

static ULONG ap_gui_button_width(const struct APGUI *gui, const char *text,
                                 ULONG minimum)
{
    ULONG width;

    width = ap_gui_text_width(gui, text) + AP_BUTTON_TEXT_PADDING;
    return width > minimum ? width : minimum;
}

static void ap_gui_measure_labels(const struct APGUI *gui,
                                  ULONG *primary_label_width,
                                  ULONG *right_label_width,
                                  ULONG *port_label_width)
{
    ULONG primary;
    ULONG right;

    /*
     * GadTools places PLACETEXT_LEFT labels outside the gadget hit box.
     * Measure the actual localized strings in the public screen font so the
     * gadget column starts far enough to the right for every label.  Full-row
     * and left-column labels share one width to keep their fields aligned.
     */
    primary = 0UL;
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_PRINTER_IP, "Printer IP")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_IPP_PATH, "IPP path")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_PRINTER, "Printer")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_STATUS, "Status")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_PROTOCOL, "Protocol")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_DUPLEX, "Duplex")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_COLOR, "Color")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_PAPER, "Paper")));
    primary = ap_gui_max_width(primary, ap_gui_text_width(gui, AP_TR(MSG_LABEL_PAGE, "Page")));

    right = 0UL;
    right = ap_gui_max_width(right, ap_gui_text_width(gui, AP_TR(MSG_LABEL_DPI, "DPI")));
    right = ap_gui_max_width(right, ap_gui_text_width(gui, AP_TR(MSG_LABEL_INK, "Ink")));
    right = ap_gui_max_width(right, ap_gui_text_width(gui, AP_TR(MSG_LABEL_QUALITY, "Quality")));
    right = ap_gui_max_width(right, ap_gui_text_width(gui, AP_TR(MSG_LABEL_SCALE_PERCENT_CLASSIC, "Scale %")));

    *primary_label_width = primary;
    *right_label_width = right;
    *port_label_width = ap_gui_text_width(gui, AP_TR(MSG_LABEL_PORT, "Port"));
}

static ULONG ap_gui_required_window_width(const struct APGUI *gui)
{
    ULONG primary_label_width;
    ULONG right_label_width;
    ULONG port_label_width;
    ULONG query_width;
    ULONG search_width;
    ULONG test_width;
    ULONG save_width;
    ULONG cancel_width;
    ULONG required;
    ULONG candidate;

    ap_gui_measure_labels(gui, &primary_label_width, &right_label_width,
                          &port_label_width);
    query_width = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_QUERY_CLASSIC, "Query printer"), 126UL);
    search_width = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_SEARCH_CLASSIC, "Search..."), 96UL);
    test_width = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_TESTPAGE_CLASSIC, "Testpage"), 110UL);
    save_width = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_SAVE_CLASSIC, "Save"), 110UL);
    cancel_width = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_CANCEL_CLASSIC, "Cancel"), 110UL);

    required = AP_MIN_WINDOW_WIDTH;

    candidate = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                AP_MIN_FULL_GADGET_WIDTH + AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    candidate = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                AP_MIN_COLUMN_GADGET_WIDTH + AP_COLUMN_GAP +
                right_label_width + AP_LABEL_GAP +
                AP_MIN_COLUMN_GADGET_WIDTH + AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    candidate = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                AP_MIN_HOST_GADGET_WIDTH + AP_ROW_CONTROL_GAP +
                port_label_width + AP_LABEL_GAP + AP_PORT_GADGET_WIDTH +
                AP_ROW_CONTROL_GAP + query_width + AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    candidate = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                AP_MIN_FULL_GADGET_WIDTH + AP_ROW_CONTROL_GAP +
                search_width + AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    candidate = AP_EDGE_MARGIN + test_width + AP_BOTTOM_BUTTON_GAP +
                save_width + AP_BOTTOM_BUTTON_GAP + cancel_width +
                AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    candidate = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                18UL + AP_LABEL_GAP +
                ap_gui_text_width(gui, AP_TR(MSG_CENTER_ON_PAPER, "Center on paper")) +
                AP_EDGE_MARGIN;
    required = ap_gui_max_width(required, candidate);

    return required;
}

static int ap_gui_compute_geometry(const struct APGUI *gui,
                                   struct APGUIGeometry *geo)
{
    ULONG width;
    ULONG primary_label_width;
    ULONG right_label_width;
    ULONG port_label_width;
    ULONG left_x;
    ULONG left_w;
    ULONG right_x;
    ULONG right_w;
    ULONG two_column_fixed;
    ULONG two_column_flexible;
    ULONG query_w;
    ULONG query_x;
    ULONG search_w;
    ULONG search_x;
    ULONG path_w;
    ULONG port_x;
    ULONG host_right;
    ULONG host_w;
    ULONG test_w;
    ULONG save_w;
    ULONG cancel_w;
    ULONG button_total;
    ULONG button_x;

    if (gui == NULL || geo == NULL) return 0;
    width = gui->window_width;
    ap_gui_measure_labels(gui, &primary_label_width, &right_label_width,
                          &port_label_width);

    left_x = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP;
    if (left_x + AP_EDGE_MARGIN >= width) return 0;

    two_column_fixed = AP_EDGE_MARGIN + primary_label_width + AP_LABEL_GAP +
                       AP_COLUMN_GAP + right_label_width + AP_LABEL_GAP +
                       AP_EDGE_MARGIN;
    if (two_column_fixed >= width) return 0;
    two_column_flexible = width - two_column_fixed;
    if (two_column_flexible < 2UL * AP_MIN_COLUMN_GADGET_WIDTH) return 0;

    left_w = two_column_flexible / 2UL;
    right_w = two_column_flexible - left_w;
    right_x = left_x + left_w + AP_COLUMN_GAP +
              right_label_width + AP_LABEL_GAP;
    if (right_x + right_w + AP_EDGE_MARGIN != width) return 0;

    query_w = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_QUERY_CLASSIC, "Query printer"), 126UL);
    if (query_w + AP_EDGE_MARGIN >= width) return 0;
    query_x = width - AP_EDGE_MARGIN - query_w;
    if (query_x <= AP_ROW_CONTROL_GAP + AP_PORT_GADGET_WIDTH) return 0;
    port_x = query_x - AP_ROW_CONTROL_GAP - AP_PORT_GADGET_WIDTH;
    if (port_x <= port_label_width + AP_LABEL_GAP + AP_ROW_CONTROL_GAP) return 0;
    host_right = port_x - port_label_width - AP_LABEL_GAP - AP_ROW_CONTROL_GAP;
    if (host_right <= left_x) return 0;
    host_w = host_right - left_x;
    if (host_w < AP_MIN_HOST_GADGET_WIDTH) return 0;

    search_w = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_SEARCH_CLASSIC, "Search..."), 96UL);
    if (search_w + AP_EDGE_MARGIN >= width) return 0;
    search_x = width - AP_EDGE_MARGIN - search_w;
    if (search_x <= left_x + AP_ROW_CONTROL_GAP) return 0;
    path_w = search_x - AP_ROW_CONTROL_GAP - left_x;
    if (path_w < AP_MIN_FULL_GADGET_WIDTH) return 0;

    test_w = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_TESTPAGE_CLASSIC, "Testpage"), 110UL);
    save_w = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_SAVE_CLASSIC, "Save"), 110UL);
    cancel_w = ap_gui_button_width(gui,
        AP_TR(MSG_BUTTON_CANCEL_CLASSIC, "Cancel"), 110UL);
    button_total = test_w + AP_BOTTOM_BUTTON_GAP + save_w +
                   AP_BOTTOM_BUTTON_GAP + cancel_w;
    if (button_total + 2UL * AP_EDGE_MARGIN > width) return 0;
    button_x = (width - button_total) / 2UL;

    if (left_x > 65535UL || left_w > 65535UL || right_x > 65535UL ||
        right_w > 65535UL || query_w > 65535UL || query_x > 65535UL ||
        search_w > 65535UL || search_x > 65535UL || path_w > 65535UL ||
        port_x > 65535UL || host_w > 65535UL || test_w > 65535UL ||
        save_w > 65535UL || cancel_w > 65535UL || button_x > 65535UL)
        return 0;

    geo->full_x = (UWORD)left_x;
    geo->full_w = (UWORD)(width - left_x - AP_EDGE_MARGIN);
    geo->left_x = (UWORD)left_x;
    geo->left_w = (UWORD)left_w;
    geo->right_x = (UWORD)right_x;
    geo->right_w = (UWORD)right_w;
    geo->query_w = (UWORD)query_w;
    geo->query_x = (UWORD)query_x;
    geo->search_w = (UWORD)search_w;
    geo->search_x = (UWORD)search_x;
    geo->path_w = (UWORD)path_w;
    geo->port_x = (UWORD)port_x;
    geo->host_w = (UWORD)host_w;
    geo->test_w = (UWORD)test_w;
    geo->save_w = (UWORD)save_w;
    geo->cancel_w = (UWORD)cancel_w;
    geo->test_x = (UWORD)button_x;
    geo->save_x = (UWORD)(button_x + test_w + AP_BOTTOM_BUTTON_GAP);
    geo->cancel_x = (UWORD)(button_x + test_w + AP_BOTTOM_BUTTON_GAP +
                            save_w + AP_BOTTOM_BUTTON_GAP);
    return 1;
}

static int ap_gui_build_gadgets(struct APGUI *gui)
{
    struct Gadget *previous;
    struct TagItem tags[5];
    struct APGUIGeometry geo;
    UWORD y;
    UWORD h;
    STRPTR *color_labels;
    STRPTR *quality_labels;
    STRPTR *media_labels;
    ULONG color_active;
    ULONG quality_active;
    ULONG media_active;

    previous = CreateContext(&gui->glist);
    if (previous == NULL) return 0;
    if (!ap_gui_compute_geometry(gui, &geo)) return 0;

    h = (UWORD)(gui->row_height - 3U);
    y = gui->content_top;

    tags[0].ti_Tag = GTST_String;
    tags[0].ti_Data = (ULONG)gui->prefs.host;
    tags[1].ti_Tag = GTST_MaxChars;
    tags[1].ti_Data = AP_PREFS_HOST_LEN - 1U;
    tags[2].ti_Tag = GA_TabCycle;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, STRING_KIND, GID_HOST,
                                 (STRPTR)AP_TR(MSG_LABEL_PRINTER_IP, "Printer IP"),
                                 geo.full_x, y, geo.host_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = GTIN_Number;
    tags[0].ti_Data = gui->prefs.port;
    tags[1].ti_Tag = GTIN_MaxChars;
    tags[1].ti_Data = 5U;
    tags[2].ti_Tag = GA_TabCycle;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, INTEGER_KIND, GID_PORT,
                                 (STRPTR)AP_TR(MSG_LABEL_PORT, "Port"),
                                 geo.port_x, y, AP_PORT_GADGET_WIDTH, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = TAG_END;
    tags[0].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, BUTTON_KIND, GID_QUERY,
                                 (STRPTR)AP_TR(MSG_BUTTON_QUERY_CLASSIC, "Query printer"),
                                 geo.query_x, y, geo.query_w, h,
                                 PLACETEXT_IN, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTST_String;
    tags[0].ti_Data = (ULONG)gui->prefs.path;
    tags[1].ti_Tag = GTST_MaxChars;
    tags[1].ti_Data = AP_PREFS_PATH_LEN - 1U;
    tags[2].ti_Tag = GA_TabCycle;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, STRING_KIND, GID_PATH,
                                 (STRPTR)AP_TR(MSG_LABEL_IPP_PATH, "IPP path"),
                                 geo.full_x, y, geo.path_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = TAG_END;
    tags[0].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, BUTTON_KIND, GID_SEARCH,
                                 (STRPTR)AP_TR(MSG_BUTTON_SEARCH_CLASSIC, "Search..."),
                                 geo.search_x, y, geo.search_w, h,
                                 PLACETEXT_IN, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->model_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_MODEL,
                                 (STRPTR)AP_TR(MSG_LABEL_PRINTER, "Printer"),
                                 geo.full_x, y, geo.full_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->status_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_STATUS,
                                 (STRPTR)AP_TR(MSG_LABEL_STATUS, "Status"),
                                 geo.full_x, y, geo.full_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->airprint_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_AIRPRINT,
                                 (STRPTR)AP_TR(MSG_LABEL_PROTOCOL, "Protocol"),
                                 geo.left_x, y, geo.left_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->resolution_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_RESOLUTION,
                                 (STRPTR)AP_TR(MSG_LABEL_DPI, "DPI"),
                                 geo.right_x, y, geo.right_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->duplex_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_DUPLEX,
                                 (STRPTR)AP_TR(MSG_LABEL_DUPLEX, "Duplex"),
                                 geo.left_x, y, geo.left_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = GTTX_Text;
    tags[0].ti_Data = (ULONG)gui->ink_text;
    tags[1].ti_Tag = GTTX_Border;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, TEXT_KIND, GID_INK,
                                 (STRPTR)AP_TR(MSG_LABEL_INK, "Ink"),
                                 geo.right_x, y, geo.right_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    if (gui->queried) ap_gui_build_options(gui);
    color_labels = gui->queried && gui->color_count != 0U
        ? gui->color_labels : g_not_queried_labels;
    quality_labels = gui->queried && gui->quality_count != 0U
        ? gui->quality_labels : g_not_queried_labels;
    media_labels = gui->queried && gui->media_count != 0U
        ? gui->media_labels : g_not_queried_labels;
    color_active = gui->queried ? ap_gui_find_color(gui, gui->prefs.color_mode) : 0U;
    quality_active = gui->queried ? ap_gui_find_quality(gui, gui->prefs.quality) : 0U;
    media_active = gui->queried ? ap_gui_find_media(gui, gui->prefs.media) : 0U;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTCY_Labels;
    tags[0].ti_Data = (ULONG)color_labels;
    tags[1].ti_Tag = GTCY_Active;
    tags[1].ti_Data = color_active;
    tags[2].ti_Tag = GA_Disabled;
    tags[2].ti_Data = gui->queried && gui->color_count != 0U ? FALSE : TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, CYCLE_KIND, GID_COLOR,
                                 (STRPTR)AP_TR(MSG_LABEL_COLOR, "Color"),
                                 geo.left_x, y, geo.left_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = GTCY_Labels;
    tags[0].ti_Data = (ULONG)quality_labels;
    tags[1].ti_Tag = GTCY_Active;
    tags[1].ti_Data = quality_active;
    tags[2].ti_Tag = GA_Disabled;
    tags[2].ti_Data = gui->queried && gui->quality_count != 0U ? FALSE : TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, CYCLE_KIND, GID_QUALITY,
                                 (STRPTR)AP_TR(MSG_LABEL_QUALITY, "Quality"),
                                 geo.right_x, y, geo.right_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTCY_Labels;
    tags[0].ti_Data = (ULONG)media_labels;
    tags[1].ti_Tag = GTCY_Active;
    tags[1].ti_Data = media_active;
    tags[2].ti_Tag = GA_Disabled;
    tags[2].ti_Data = gui->queried && gui->media_count != 0U ? FALSE : TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, CYCLE_KIND, GID_MEDIA,
                                 (STRPTR)AP_TR(MSG_LABEL_PAPER, "Paper"),
                                 geo.full_x, y, geo.full_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTCY_Labels;
    tags[0].ti_Data = (ULONG)g_orientation_labels;
    tags[1].ti_Tag = GTCY_Active;
    tags[1].ti_Data = strcmp(gui->prefs.orientation, "landscape") == 0 ? 1U : 0U;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, CYCLE_KIND, GID_ORIENTATION,
                                 (STRPTR)AP_TR(MSG_LABEL_PAGE, "Page"),
                                 geo.left_x, y, geo.left_w, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    tags[0].ti_Tag = GTIN_Number;
    tags[0].ti_Data = gui->prefs.scale_percent;
    tags[1].ti_Tag = GTIN_MaxChars;
    tags[1].ti_Data = 3U;
    tags[2].ti_Tag = GA_TabCycle;
    tags[2].ti_Data = TRUE;
    tags[3].ti_Tag = TAG_END;
    tags[3].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, INTEGER_KIND, GID_SCALE,
                                 (STRPTR)AP_TR(MSG_LABEL_SCALE_PERCENT_CLASSIC, "Scale %"),
                                 geo.right_x, y, AP_SCALE_GADGET_WIDTH, h,
                                 PLACETEXT_LEFT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height);
    tags[0].ti_Tag = GTCB_Checked;
    tags[0].ti_Data = gui->prefs.center_on_paper ? TRUE : FALSE;
    tags[1].ti_Tag = GTCB_Scaled;
    tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, CHECKBOX_KIND, GID_CENTER,
                                 (STRPTR)AP_TR(MSG_CENTER_ON_PAPER, "Center on paper"),
                                 geo.left_x, y, 18U, h,
                                 PLACETEXT_RIGHT, tags);
    if (previous == NULL) return 0;

    y = (UWORD)(y + gui->row_height + 2U);
    tags[0].ti_Tag = TAG_END;
    tags[0].ti_Data = 0UL;
    previous = ap_gui_add_gadget(gui, previous, BUTTON_KIND, GID_TESTPAGE,
                                 (STRPTR)AP_TR(MSG_BUTTON_TESTPAGE_CLASSIC, "Testpage"),
                                 geo.test_x, y, geo.test_w, h,
                                 PLACETEXT_IN, tags);
    if (previous == NULL) return 0;
    previous = ap_gui_add_gadget(gui, previous, BUTTON_KIND, GID_SAVE,
                                 (STRPTR)AP_TR(MSG_BUTTON_SAVE_CLASSIC, "Save"),
                                 geo.save_x, y, geo.save_w, h,
                                 PLACETEXT_IN, tags);
    if (previous == NULL) return 0;
    previous = ap_gui_add_gadget(gui, previous, BUTTON_KIND, GID_CANCEL,
                                 (STRPTR)AP_TR(MSG_BUTTON_CANCEL_CLASSIC, "Cancel"),
                                 geo.cancel_x, y, geo.cancel_w, h,
                                 PLACETEXT_IN, tags);
    if (previous == NULL) return 0;

    return 1;
}

static int ap_gui_open_window(struct APGUI *gui)
{
    struct TagItem tags[14];
    LONG left;
    LONG top;
    ULONG screen_width;
    ULONG screen_height;
    ULONG available_height;
    ULONG required_width;
    ULONG target_width;
    ULONG maximum_width;
    UWORD desired_row_height;
    UWORD minimum_row_height;

    gui->screen = LockPubScreen(NULL);
    gui->screen_locked = gui->screen != NULL;
    if (gui->screen == NULL) return 0;

    /* Screen dimensions are signed WORDs in intuition/screens.h. */
    if (gui->screen->Width <= 8 || gui->screen->Height <= 16) {
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }
    screen_width = (ULONG)gui->screen->Width;
    screen_height = (ULONG)gui->screen->Height;

    /*
     * Preserve the familiar 620-pixel Classic window where possible, but
     * allow localized labels and button captions to request a wider window.
     * The screen edge remains the hard limit on a 640-pixel Workbench.
     */
    maximum_width = screen_width - 8UL;
    if (maximum_width < AP_MIN_WINDOW_WIDTH) {
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }
    required_width = ap_gui_required_window_width(gui);
    target_width = AP_MAX_WINDOW_WIDTH;
    if (target_width < AP_MIN_WINDOW_WIDTH) target_width = AP_MIN_WINDOW_WIDTH;
    if (required_width > target_width) target_width = required_width;
    if (target_width > maximum_width) target_width = maximum_width;
    gui->window_width = (UWORD)target_width;

    gui->content_top = (UWORD)(gui->screen->WBorTop + gui->screen->Font->ta_YSize + 5U);
    desired_row_height = (UWORD)(gui->screen->Font->ta_YSize + 9U);
    if (desired_row_height < 18U) desired_row_height = 18U;
    minimum_row_height = (UWORD)(gui->screen->Font->ta_YSize + 5U);
    if (minimum_row_height < 13U) minimum_row_height = 13U;

    /*
     * Eleven compact rows fit comfortably on PAL High Res.  On a 200-line
     * NTSC Workbench reduce only the inter-row padding, never the screen font.
     */
    if (screen_height <= (ULONG)gui->content_top + 16UL) {
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }
    available_height = (screen_height - 6UL -
                        (ULONG)gui->content_top - 10UL) / 11UL;
    if (available_height < (ULONG)minimum_row_height) {
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }
    gui->row_height = desired_row_height;
    if ((ULONG)gui->row_height > available_height)
        gui->row_height = (UWORD)available_height;

    gui->window_height = (UWORD)(gui->content_top + 11U * gui->row_height + 10U);
    if (gui->window_height < AP_MIN_WINDOW_HEIGHT) gui->window_height = AP_MIN_WINDOW_HEIGHT;
    if ((ULONG)gui->window_height > screen_height - 6UL)
        gui->window_height = (UWORD)(screen_height - 6UL);

    gui->visual_info = GetVisualInfoA(gui->screen, NULL);
    if (gui->visual_info == NULL || !ap_gui_build_gadgets(gui)) {
        if (gui->glist != NULL) FreeGadgets(gui->glist);
        gui->glist = NULL;
        if (gui->visual_info != NULL) FreeVisualInfo(gui->visual_info);
        gui->visual_info = NULL;
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }

    left = ((LONG)screen_width - (LONG)gui->window_width) / 2L;
    top = ((LONG)screen_height - (LONG)gui->window_height) / 2L;
    if (left < 0L) left = 0L;
    if (top < 0L) top = 0L;

    tags[0].ti_Tag = WA_Left;
    tags[0].ti_Data = (ULONG)left;
    tags[1].ti_Tag = WA_Top;
    tags[1].ti_Data = (ULONG)top;
    tags[2].ti_Tag = WA_Width;
    tags[2].ti_Data = gui->window_width;
    tags[3].ti_Tag = WA_Height;
    tags[3].ti_Data = gui->window_height;
    tags[4].ti_Tag = WA_Title;
    tags[4].ti_Data = (ULONG)AP_TR(MSG_WINDOW_TITLE_CLASSIC, "AmiAirPrint Prefs (Classic)");
    tags[5].ti_Tag = WA_Gadgets;
    tags[5].ti_Data = (ULONG)gui->glist;
    tags[6].ti_Tag = WA_IDCMP;
    tags[6].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW;
    tags[7].ti_Tag = WA_Flags;
    tags[7].ti_Data = WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET |
                      WFLG_DRAGBAR | WFLG_SMART_REFRESH;
    tags[8].ti_Tag = WA_PubScreen;
    tags[8].ti_Data = (ULONG)gui->screen;
    tags[9].ti_Tag = WA_AutoAdjust;
    tags[9].ti_Data = TRUE;
    tags[10].ti_Tag = TAG_END;
    tags[10].ti_Data = 0UL;

    gui->window = OpenWindowTagList(NULL, tags);
    if (gui->window == NULL) {
        FreeGadgets(gui->glist);
        gui->glist = NULL;
        FreeVisualInfo(gui->visual_info);
        gui->visual_info = NULL;
        UnlockPubScreen(NULL, gui->screen);
        gui->screen_locked = 0;
        gui->screen = NULL;
        return 0;
    }

    /*
     * The open window now acts as the public-screen lock.  Release the extra
     * LockPubScreen() lock immediately so an automatic public screen is not
     * pinned for the entire lifetime of the preferences program.
     */
    UnlockPubScreen(NULL, gui->screen);
    gui->screen_locked = 0;
    gui->screen = gui->window->WScreen;

    GT_RefreshWindow(gui->window, NULL);
    if (gui->queried) ap_gui_update_capabilities(gui);
    return 1;
}

static void ap_gui_close_window(struct APGUI *gui)
{
    /*
     * After a successful OpenWindow() the window itself keeps WScreen alive.
     * Remove the GadTools list while the window is still open, then free the
     * gadgets and VisualInfo before CloseWindow() releases that final screen
     * reference.  This follows both the Intuition public-screen and GadTools
     * VisualInfo lifetime rules.
     */
    if (gui->window != NULL && gui->glist != NULL)
        (void)RemoveGList(gui->window, gui->glist, -1L);

    if (gui->glist != NULL) FreeGadgets(gui->glist);
    gui->glist = NULL;
    if (gui->visual_info != NULL) FreeVisualInfo(gui->visual_info);
    gui->visual_info = NULL;

    if (gui->window != NULL) CloseWindow(gui->window);
    gui->window = NULL;

    /* Error/partial-open fallback: normally zero after successful open. */
    if (gui->screen_locked && gui->screen != NULL)
        UnlockPubScreen(NULL, gui->screen);
    gui->screen_locked = 0;
    gui->screen = NULL;
}

static void ap_gui_prepare_discovery_labels(struct APGUI *gui)
{
    unsigned int i;

    if (gui == NULL) return;
    for (i = 0U; i < gui->discovery.count && i < AP_DISCOVERY_MAX_PRINTERS; ++i) {
        const struct APDiscoveredPrinter *printer;
        printer = &gui->discovery.printers[i];
        snprintf(gui->discovery_label_storage[i],
                 sizeof(gui->discovery_label_storage[i]),
                 "%s  (%s:%u)",
                 printer->name[0] != '\0' ? printer->name : printer->host_name,
                 printer->address,
                 (unsigned int)printer->port);
        gui->discovery_labels[i] = gui->discovery_label_storage[i];
    }
    gui->discovery_labels[i] = NULL;
}

static int ap_gui_choose_discovered(struct APGUI *gui, unsigned int *selected_index)
{
    enum {
        GID_DISCOVERY_LIST = 1001,
        GID_DISCOVERY_USE,
        GID_DISCOVERY_CANCEL
    };
    struct Gadget *glist;
    struct Gadget *previous;
    struct Window *window;
    struct TagItem tags[6];
    struct TagItem window_tags[12];
    struct NewGadget ng;
    struct List printer_list;
    struct Node printer_nodes[AP_DISCOVERY_MAX_PRINTERS];
    ULONG screen_width;
    ULONG screen_height;
    UWORD width;
    UWORD height;
    UWORD gadget_height;
    UWORD list_height;
    LONG left;
    LONG top;
    unsigned int i;
    ULONG active;
    int done;
    int accepted;

    if (gui == NULL || selected_index == NULL || gui->discovery.count == 0U ||
        gui->screen == NULL || gui->visual_info == NULL)
        return 0;
    ap_gui_prepare_discovery_labels(gui);

    /* Initialize the Exec list locally. NewList() is an amiga.lib helper,
     * not an exec.library entry point on all classic SDK combinations. */
    printer_list.lh_Head = (struct Node *)&printer_list.lh_Tail;
    printer_list.lh_Tail = NULL;
    printer_list.lh_TailPred = (struct Node *)&printer_list.lh_Head;

    for (i = 0U; i < gui->discovery.count && i < AP_DISCOVERY_MAX_PRINTERS; ++i) {
        memset(&printer_nodes[i], 0, sizeof(printer_nodes[i]));
        printer_nodes[i].ln_Name = (char *)gui->discovery_labels[i];
        AddTail(&printer_list, &printer_nodes[i]);
    }

    screen_width = (ULONG)gui->screen->Width;
    screen_height = (ULONG)gui->screen->Height;
    width = (UWORD)(screen_width > 540UL ? 520UL : screen_width - 20UL);
    if (width < 300U) width = (UWORD)(screen_width - 8UL);
    height = (UWORD)(screen_height > 190UL ? 170UL : screen_height - 8UL);
    gadget_height = (UWORD)(gui->screen->Font->ta_YSize + 9U);
    if (gadget_height < 18U) gadget_height = 18U;
    if (height <= gadget_height + 62U) return 0;
    list_height = (UWORD)(height - gadget_height - 54U);

    glist = NULL;
    previous = CreateContext(&glist);
    if (previous == NULL) return 0;

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = 18;
    ng.ng_TopEdge = 28;
    ng.ng_Width = (WORD)(width - 36U);
    ng.ng_Height = list_height;
    ng.ng_GadgetText = (UBYTE *)AP_TR(MSG_LABEL_PRINTER, "Printer");
    ng.ng_TextAttr = gui->screen->Font;
    ng.ng_GadgetID = GID_DISCOVERY_LIST;
    ng.ng_Flags = PLACETEXT_ABOVE;
    ng.ng_VisualInfo = gui->visual_info;
    tags[0].ti_Tag = GTLV_Labels;
    tags[0].ti_Data = (ULONG)&printer_list;
    tags[1].ti_Tag = GTLV_ReadOnly;
    tags[1].ti_Data = FALSE;
    /* On GadTools V39+, GTLV_ShowSelected=NULL requests the normal
     * highlight bar in the list itself.  Select the first printer by
     * default so a single discovered printer is visibly selected too. */
    tags[2].ti_Tag = GTLV_ShowSelected;
    tags[2].ti_Data = 0UL;
    tags[3].ti_Tag = GTLV_Selected;
    tags[3].ti_Data = 0UL;
    tags[4].ti_Tag = TAG_END;
    tags[4].ti_Data = 0UL;
    previous = CreateGadgetA(LISTVIEW_KIND, previous, &ng, tags);
    if (previous == NULL) {
        FreeGadgets(glist);
        return 0;
    }

    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = (WORD)(width / 2U - 120U);
    ng.ng_TopEdge = (WORD)(height - gadget_height - 10U);
    ng.ng_Width = 105;
    ng.ng_Height = gadget_height;
    ng.ng_GadgetText = (UBYTE *)AP_TR(MSG_BUTTON_DISCOVERY_USE_CLASSIC, "Use");
    ng.ng_TextAttr = gui->screen->Font;
    ng.ng_GadgetID = GID_DISCOVERY_USE;
    ng.ng_Flags = PLACETEXT_IN;
    ng.ng_VisualInfo = gui->visual_info;
    tags[0].ti_Tag = TAG_END;
    tags[0].ti_Data = 0UL;
    previous = CreateGadgetA(BUTTON_KIND, previous, &ng, tags);
    if (previous == NULL) {
        FreeGadgets(glist);
        return 0;
    }

    ng.ng_LeftEdge = (WORD)(width / 2U + 15U);
    ng.ng_GadgetText = (UBYTE *)AP_TR(MSG_BUTTON_DISCOVERY_CANCEL_CLASSIC, "Cancel");
    ng.ng_GadgetID = GID_DISCOVERY_CANCEL;
    previous = CreateGadgetA(BUTTON_KIND, previous, &ng, tags);
    if (previous == NULL) {
        FreeGadgets(glist);
        return 0;
    }

    left = ((LONG)screen_width - (LONG)width) / 2L;
    top = ((LONG)screen_height - (LONG)height) / 2L;
    window_tags[0].ti_Tag = WA_Left;
    window_tags[0].ti_Data = (ULONG)left;
    window_tags[1].ti_Tag = WA_Top;
    window_tags[1].ti_Data = (ULONG)top;
    window_tags[2].ti_Tag = WA_Width;
    window_tags[2].ti_Data = width;
    window_tags[3].ti_Tag = WA_Height;
    window_tags[3].ti_Data = height;
    window_tags[4].ti_Tag = WA_Title;
    window_tags[4].ti_Data = (ULONG)AP_TR(MSG_DISCOVERY_WINDOW_TITLE, "Select printer");
    window_tags[5].ti_Tag = WA_Gadgets;
    window_tags[5].ti_Data = (ULONG)glist;
    window_tags[6].ti_Tag = WA_IDCMP;
    window_tags[6].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW;
    window_tags[7].ti_Tag = WA_Flags;
    window_tags[7].ti_Data = WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET |
                             WFLG_DRAGBAR | WFLG_SMART_REFRESH;
    window_tags[8].ti_Tag = WA_PubScreen;
    window_tags[8].ti_Data = (ULONG)gui->screen;
    window_tags[9].ti_Tag = WA_AutoAdjust;
    window_tags[9].ti_Data = TRUE;
    window_tags[10].ti_Tag = TAG_END;
    window_tags[10].ti_Data = 0UL;
    window = OpenWindowTagList(NULL, window_tags);
    if (window == NULL) {
        FreeGadgets(glist);
        return 0;
    }

    GT_RefreshWindow(window, NULL);
    active = 0UL;
    done = 0;
    accepted = 0;
    while (!done) {
        ULONG signal_mask;
        ULONG received;
        struct IntuiMessage *message;

        signal_mask = 1UL << window->UserPort->mp_SigBit;
        received = Wait(signal_mask | SIGBREAKF_CTRL_C);
        if ((received & SIGBREAKF_CTRL_C) != 0U) break;

        while ((message = GT_GetIMsg(window->UserPort)) != NULL) {
            ULONG msg_class;
            struct Gadget *gadget;
            UWORD id;
            UWORD msg_code;

            msg_class = message->Class;
            msg_code = message->Code;
            gadget = msg_class == IDCMP_GADGETUP
                ? (struct Gadget *)message->IAddress : NULL;
            id = gadget != NULL ? gadget->GadgetID : 0U;

            if (msg_class == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(window);
                GT_EndRefresh(window, TRUE);
                GT_ReplyIMsg(message);
                continue;
            }
            GT_ReplyIMsg(message);

            if (msg_class == IDCMP_CLOSEWINDOW) {
                done = 1;
            } else if (msg_class == IDCMP_GADGETUP) {
                if (id == GID_DISCOVERY_LIST) {
                    /* GadTools returns the selected list ordinal in Code.
                     * Mirror it back into GTLV_Selected so the highlight
                     * stays visibly attached to the active row. */
                    if ((ULONG)msg_code < gui->discovery.count) {
                        struct TagItem select_tags[2];
                        active = (ULONG)msg_code;
                        select_tags[0].ti_Tag = GTLV_Selected;
                        select_tags[0].ti_Data = active;
                        select_tags[1].ti_Tag = TAG_END;
                        select_tags[1].ti_Data = 0UL;
                        GT_SetGadgetAttrsA(gadget, window, NULL, select_tags);
                    }
                } else if (id == GID_DISCOVERY_USE) {
                    if (active < gui->discovery.count) {
                        *selected_index = (unsigned int)active;
                        accepted = 1;
                    }
                    done = 1;
                } else if (id == GID_DISCOVERY_CANCEL) {
                    done = 1;
                }
            }
        }
    }

    (void)RemoveGList(window, glist, -1L);
    FreeGadgets(glist);
    CloseWindow(window);
    return accepted;
}

static void ap_gui_apply_discovered(struct APGUI *gui,
                                    const struct APDiscoveredPrinter *printer)
{
    struct TagItem tags[2];

    if (gui == NULL || printer == NULL || gui->window == NULL) return;
    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", printer->address);
    gui->prefs.port = (unsigned int)printer->port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", printer->path);

    tags[0].ti_Tag = GTST_String;
    tags[0].ti_Data = (ULONG)gui->prefs.host;
    tags[1].ti_Tag = TAG_END;
    tags[1].ti_Data = 0UL;
    GT_SetGadgetAttrsA(gui->gadgets[GID_HOST], gui->window, NULL, tags);

    tags[0].ti_Tag = GTIN_Number;
    tags[0].ti_Data = gui->prefs.port;
    GT_SetGadgetAttrsA(gui->gadgets[GID_PORT], gui->window, NULL, tags);

    tags[0].ti_Tag = GTST_String;
    tags[0].ti_Data = (ULONG)gui->prefs.path;
    GT_SetGadgetAttrsA(gui->gadgets[GID_PATH], gui->window, NULL, tags);

    ap_gui_invalidate_query(gui,
        AP_TR(MSG_STATUS_PRINTER_SELECTED, "Printer selected - query printer"));
}

static int ap_gui_search(struct APGUI *gui)
{
    unsigned int selected;
    char message[192];

    if (gui == NULL) return 0;
    ap_gui_set_status(gui,
        AP_TR(MSG_STATUS_SEARCHING, "Searching network for IPP printers..."));

    if (!ap_discovery_search(&gui->discovery)) {
        snprintf(message, sizeof(message),
                 AP_TR(MSG_FORMAT_SEARCH_FAILED, "Search failed: %s"),
                 ap_discovery_last_error());
        ap_gui_set_status(gui, message);
        return 0;
    }
    if (gui->discovery.count == 0U) {
        ap_gui_set_status(gui,
            AP_TR(MSG_STATUS_SEARCH_NONE, "No IPP/AirPrint printers found"));
        return 0;
    }

    selected = 0U;
    if (!ap_gui_choose_discovered(gui, &selected)) {
        snprintf(message, sizeof(message),
                 AP_TR(MSG_FORMAT_SEARCH_FOUND, "%u printer(s) found"),
                 gui->discovery.count);
        ap_gui_set_status(gui, message);
        return 0;
    }
    ap_gui_apply_discovered(gui, &gui->discovery.printers[selected]);
    return 1;
}

static int ap_gui_read_address(struct APGUI *gui, char *host, size_t host_size,
                               char *path, size_t path_size, unsigned int *port)
{
    STRPTR host_value;
    STRPTR path_value;
    ULONG port_value;

    host_value = ap_gui_get_string(gui, GID_HOST);
    path_value = ap_gui_get_string(gui, GID_PATH);
    port_value = ap_gui_get_attr(gui, GID_PORT, GTIN_Number, 631U);

    if (host_value == NULL || host_value[0] == '\0') {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENTER_IPV4, "Enter the printer IPv4 address first"));
        return 0;
    }
    if (path_value == NULL || path_value[0] != '/') {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_IPP_PATH_SLASH, "IPP path must start with '/'"));
        return 0;
    }
    if (port_value == 0UL || port_value > 65535UL) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_INVALID_PORT, "Invalid TCP port"));
        return 0;
    }

    snprintf(host, host_size, "%s", host_value);
    snprintf(path, path_size, "%s", path_value);
    *port = (unsigned int)port_value;
    return 1;
}

static int ap_gui_query(struct APGUI *gui)
{
    char host[AP_PREFS_HOST_LEN];
    char path[AP_PREFS_PATH_LEN];
    unsigned int port;
    char error_text[160];

    if (!ap_gui_read_address(gui, host, sizeof(host), path, sizeof(path), &port))
        return 0;

    ap_gui_invalidate_query(gui, AP_TR(MSG_STATUS_QUERYING, "Querying printer..."));
    if (!ap_caps_query(host, (uint16_t)port, path, &gui->caps,
                       error_text, sizeof(error_text))) {
        char message[192];
        snprintf(message, sizeof(message), AP_TR(MSG_FORMAT_QUERY_FAILED, "Query failed: %s"), error_text);
        ap_gui_set_status(gui, message);
        return 0;
    }

    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", host);
    gui->prefs.port = port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", path);
    gui->queried = 1;
    ap_gui_update_capabilities(gui);
    return 1;
}

static int ap_gui_capture_selections(struct APGUI *gui)
{
    char host[AP_PREFS_HOST_LEN];
    char path[AP_PREFS_PATH_LEN];
    unsigned int port;
    ULONG selected;

    if (!ap_gui_read_address(gui, host, sizeof(host), path, sizeof(path), &port))
        return 0;

    if (gui->queried &&
        (strcmp(host, gui->prefs.host) != 0 ||
         port != gui->prefs.port ||
         strcmp(path, gui->prefs.path) != 0)) {
        ap_gui_invalidate_query(gui, AP_TR(MSG_STATUS_ADDRESS_CHANGED, "Address changed - query printer again"));
        return 0;
    }

    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", host);
    gui->prefs.port = port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", path);

    if (gui->color_count != 0U) {
        selected = ap_gui_get_attr(gui, GID_COLOR, GTCY_Active, 0UL);
        if (selected < gui->color_count)
            snprintf(gui->prefs.color_mode, sizeof(gui->prefs.color_mode), "%s",
                     gui->color_keys[selected]);
    }
    if (gui->quality_count != 0U) {
        selected = ap_gui_get_attr(gui, GID_QUALITY, GTCY_Active, 0UL);
        if (selected < gui->quality_count) gui->prefs.quality = gui->quality_values[selected];
    }
    if (gui->media_count != 0U) {
        selected = ap_gui_get_attr(gui, GID_MEDIA, GTCY_Active, 0UL);
        if (selected < gui->media_count)
            snprintf(gui->prefs.media, sizeof(gui->prefs.media), "%s",
                     gui->media_keys[selected]);
    }

    selected = ap_gui_get_attr(gui, GID_ORIENTATION, GTCY_Active, 0UL);
    snprintf(gui->prefs.orientation, sizeof(gui->prefs.orientation), "%s",
             selected == 1UL ? "landscape" : "portrait");

    selected = ap_gui_get_attr(gui, GID_SCALE, GTIN_Number, 100UL);
    if (selected < 10UL) selected = 10UL;
    if (selected > 100UL) selected = 100UL;
    gui->prefs.scale_percent = (unsigned int)selected;

    selected = ap_gui_get_attr(gui, GID_CENTER, GTCB_Checked, FALSE);
    gui->prefs.center_on_paper = selected != 0UL;
    return 1;
}

static int ap_gui_testpage(struct APGUI *gui)
{
    struct APPrintResult print_result;
    char error_text[160];
    char message[192];

    if (!gui->queried) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_QUERY_BEFORE_TEST, "Query the printer before printing a test page"));
        return 0;
    }
    if (!ap_gui_capture_selections(gui)) return 0;

    ap_gui_set_status(gui, AP_TR(MSG_STATUS_PRINTING_TEST, "Printing test page..."));
    if (!ap_print_document(&gui->prefs, &gui->caps, gui->queried,
                           g_airprint_testpage_jpeg, g_airprint_testpage_jpeg_len,
                           "image/jpeg", AP_TR(MSG_TESTPAGE_JOB_NAME, "AmigaOS AirPrint Test Page"),
                           &print_result, error_text, sizeof(error_text))) {
        snprintf(message, sizeof(message), AP_TR(MSG_FORMAT_TEST_FAILED, "Test page failed: %s"),
                 error_text[0] != '\0' ? error_text : AP_TR(MSG_UNKNOWN_PRINT_ERROR, "Unknown print error"));
        ap_gui_set_status(gui, message);
        return 0;
    }

    if (print_result.job_id != 0UL) {
        snprintf(message, sizeof(message), AP_TR(MSG_FORMAT_TEST_SUCCESS_JOB, "Test page sent successfully - job %lu"),
                 print_result.job_id);
        ap_gui_set_status(gui, message);
    } else {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_TEST_SUCCESS, "Test page sent successfully"));
    }
    return 1;
}

static int ap_gui_store(struct APGUI *gui)
{
    if (!gui->queried) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_QUERY_BEFORE_SAVE, "Query the printer before saving settings"));
        return 0;
    }
    if (!ap_gui_capture_selections(gui)) return 0;

    if (!ap_prefs_write_env(&gui->prefs, &gui->caps, gui->queried)) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENV_WRITE_FAILED, "Could not write ENV:AirPrint.prefs"));
        return 0;
    }
    if (!ap_prefs_write_envarc(&gui->prefs, &gui->caps, gui->queried)) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENVARC_WRITE_FAILED, "ENV updated, but ENVARC:AirPrint.prefs failed"));
        return 0;
    }
    return 1;
}

static int ap_gui_run(struct APGUI *gui)
{
    ULONG signal_mask;
    int done;

    signal_mask = 1UL << gui->window->UserPort->mp_SigBit;
    done = 0;

    while (!done) {
        ULONG received;
        struct IntuiMessage *message;

        received = Wait(signal_mask | SIGBREAKF_CTRL_C);
        if ((received & SIGBREAKF_CTRL_C) != 0U) break;

        while ((message = GT_GetIMsg(gui->window->UserPort)) != NULL) {
            ULONG msg_class;
            UWORD code;
            struct Gadget *gadget;
            UWORD id;

            msg_class = message->Class;
            code = message->Code;
            gadget = msg_class == IDCMP_GADGETUP
                ? (struct Gadget *)message->IAddress : NULL;
            id = gadget != NULL ? gadget->GadgetID : 0U;

            if (msg_class == IDCMP_REFRESHWINDOW) {
                /* GadTools requires its refresh pair before replying. */
                GT_BeginRefresh(gui->window);
                GT_EndRefresh(gui->window, TRUE);
                GT_ReplyIMsg(message);
                continue;
            }

            /* Do not retain Intuition-owned messages during network I/O. */
            GT_ReplyIMsg(message);

            if (msg_class == IDCMP_CLOSEWINDOW) {
                done = 1;
            } else if (msg_class == IDCMP_GADGETUP) {
                (void)code;
                switch (id) {
                    case GID_HOST:
                    case GID_PORT:
                    case GID_PATH:
                        ap_gui_invalidate_query(gui,
                            AP_TR(MSG_STATUS_ADDRESS_CHANGED, "Address changed - query printer again"));
                        break;
                    case GID_QUERY:
                        (void)ap_gui_query(gui);
                        break;
                    case GID_SEARCH:
                        (void)ap_gui_search(gui);
                        break;
                    case GID_TESTPAGE:
                        (void)ap_gui_testpage(gui);
                        break;
                    case GID_SAVE:
                        if (ap_gui_store(gui)) done = 1;
                        break;
                    case GID_CANCEL:
                        done = 1;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    return 0;
}

int main(void)
{
    struct APGUI *gui;
    int result;

    gui = &g_gui;
    memset(gui, 0, sizeof(*gui));

    ap_locale_open();
    ap_gui_init_locale_labels();
    ap_prefs_load(&gui->prefs, &gui->caps, &gui->queried);

    snprintf(gui->model_text, sizeof(gui->model_text), "%s", AP_TR(MSG_NOT_QUERIED, "Not queried"));
    snprintf(gui->status_text, sizeof(gui->status_text), "%s", AP_TR(MSG_STATUS_INITIAL, "Enter printer IP and query the printer"));
    snprintf(gui->airprint_text, sizeof(gui->airprint_text), "%s", AP_TR(MSG_NOT_QUERIED, "Not queried"));
    snprintf(gui->resolution_text, sizeof(gui->resolution_text), "%s", AP_TR(MSG_NOT_QUERIED, "Not queried"));
    snprintf(gui->duplex_text, sizeof(gui->duplex_text), "%s", AP_TR(MSG_NOT_QUERIED, "Not queried"));
    snprintf(gui->ink_text, sizeof(gui->ink_text), "%s", AP_TR(MSG_NOT_QUERIED, "Not queried"));
    if (gui->queried) ap_gui_prepare_capability_text(gui);

    if (!ap_gui_open_libraries()) {
        printf(AP_TR(MSG_CONSOLE_CLASSIC_REQUIRED,
                     "AmiAirPrintPrefsClassic %s requires AmigaOS 3.0+ (V39 GadTools)."),
               AIRPRINT_PREFS_CLASSIC_VERSION);
        printf("\n");
        ap_gui_close_libraries();
        ap_locale_close();
        return 20;
    }

    if (!ap_http_open()) {
        printf(AP_TR(MSG_CONSOLE_BSDSOCKET_FAILED,
                     "Could not open bsdsocket.library: %s"),
               ap_http_last_error());
        printf("\n");
        ap_gui_close_libraries();
        ap_locale_close();
        return 20;
    }

    if (!ap_gui_open_window(gui)) {
        printf("%s\n", AP_TR(MSG_CONSOLE_CLASSIC_WINDOW_FAILED,
                              "Could not create the GadTools preferences window."));
        ap_http_close();
        ap_gui_close_libraries();
        ap_locale_close();
        return 20;
    }

    result = ap_gui_run(gui);
    ap_gui_close_window(gui);
    ap_http_close();
    ap_gui_close_libraries();
    ap_locale_close();
    return result;
}
