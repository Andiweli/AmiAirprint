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
#include <intuition/intuition.h>
#include <intuition/classusr.h>
#include <libraries/gadtools.h>

#include <reaction/reaction.h>
#include <reaction/reaction_macros.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/string.h>
#include <gadgets/integer.h>
#include <gadgets/listbrowser.h>
#include <gadgets/chooser.h>
#include <gadgets/checkbox.h>
#include <images/label.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/string.h>
#include <proto/integer.h>
#include <proto/listbrowser.h>
#include <proto/chooser.h>
#include <proto/checkbox.h>
#include <proto/label.h>

#include <stdio.h>
#include <string.h>

#define AIRPRINT_PREFS_VERSION AMIAIRPRINT_PREFS_VERSION_TEXT
#define AP_GUI_MEDIA_LABEL_LEN 80

const char AmiAirPrintBrand[] __attribute__((used)) = AMIAIRPRINT_BRAND_TEXT;
const char AmiAirPrintVersionTag[] __attribute__((used)) =
    "$VER: AmiAirPrintPrefs " AMIAIRPRINT_PREFS_VERSION_TEXT " (" AMIAIRPRINT_PREFS_VERSION_DATE ")\r\n" AMIAIRPRINT_BRAND_TEXT;

struct IntuitionBase *IntuitionBase = NULL;
struct Library *WindowBase = NULL;
struct Library *LayoutBase = NULL;
struct Library *ButtonBase = NULL;
struct Library *StringBase = NULL;
struct Library *IntegerBase = NULL;
struct Library *ListBrowserBase = NULL;
struct Library *ChooserBase = NULL;
struct Library *CheckBoxBase = NULL;
struct Library *LabelBase = NULL;

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
    Object *window_object;
    struct Window *window;
    Object *gadgets[GID_COUNT];

    struct APPrefs prefs;
    struct APPrinterCapabilities caps;

    char model_text[AP_CAPS_TEXT_LEN];
    char status_text[128];
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

static void ap_gui_set_status(struct APGUI *gui, const char *text)
{
    if (gui == NULL) {
        return;
    }

    snprintf(gui->status_text, sizeof(gui->status_text), "%s", text != NULL ? text : "");

    if (gui->window != NULL && gui->gadgets[GID_STATUS] != NULL) {
        SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_STATUS],
                       gui->window,
                       NULL,
                       STRINGA_TextVal, gui->status_text,
                       TAG_END);
    }
}

static int ap_gui_open_classes(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 47L);
    if (IntuitionBase == NULL) return 0;

    WindowBase = OpenLibrary((CONST_STRPTR)"window.class", 47L);
    LayoutBase = OpenLibrary((CONST_STRPTR)"gadgets/layout.gadget", 47L);
    ButtonBase = OpenLibrary((CONST_STRPTR)"gadgets/button.gadget", 44L);
    StringBase = OpenLibrary((CONST_STRPTR)"gadgets/string.gadget", 44L);
    IntegerBase = OpenLibrary((CONST_STRPTR)"gadgets/integer.gadget", 44L);
    ListBrowserBase = OpenLibrary((CONST_STRPTR)"gadgets/listbrowser.gadget", 45L);
    ChooserBase = OpenLibrary((CONST_STRPTR)"gadgets/chooser.gadget", 45L);
    CheckBoxBase = OpenLibrary((CONST_STRPTR)"gadgets/checkbox.gadget", 44L);
    LabelBase = OpenLibrary((CONST_STRPTR)"images/label.image", 44L);

    return WindowBase != NULL &&
           LayoutBase != NULL &&
           ButtonBase != NULL &&
           StringBase != NULL &&
           IntegerBase != NULL &&
           ListBrowserBase != NULL &&
           ChooserBase != NULL &&
           CheckBoxBase != NULL &&
           LabelBase != NULL;
}

static void ap_gui_close_classes(void)
{
    if (LabelBase != NULL) CloseLibrary(LabelBase);
    if (CheckBoxBase != NULL) CloseLibrary(CheckBoxBase);
    if (ChooserBase != NULL) CloseLibrary(ChooserBase);
    if (ListBrowserBase != NULL) CloseLibrary(ListBrowserBase);
    if (IntegerBase != NULL) CloseLibrary(IntegerBase);
    if (StringBase != NULL) CloseLibrary(StringBase);
    if (ButtonBase != NULL) CloseLibrary(ButtonBase);
    if (LayoutBase != NULL) CloseLibrary(LayoutBase);
    if (WindowBase != NULL) CloseLibrary(WindowBase);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);

    LabelBase = NULL;
    CheckBoxBase = NULL;
    ChooserBase = NULL;
    ListBrowserBase = NULL;
    IntegerBase = NULL;
    StringBase = NULL;
    ButtonBase = NULL;
    LayoutBase = NULL;
    WindowBase = NULL;
    IntuitionBase = NULL;
}

static Object *ap_gui_readonly_string(ULONG id, const char *text, ULONG max_chars)
{
    return StringObject,
        GA_ID, id,
        GA_ReadOnly, TRUE,
        STRINGA_TextVal, (STRPTR)text,
        STRINGA_MaxChars, max_chars,
    StringEnd;
}

static Object *ap_gui_build_window(struct APGUI *gui)
{
    Object *window_object;

    /*
     * ReAction owns all normal inter-gadget and outer spacing here.
     * Do not add LAYOUT_InnerSpacing or fixed pixel spacer children: the
     * defaults are derived from ReAction.prefs and should follow the user's
     * configured GUI spacing.  Keeping all labelled rows in one VLayout also
     * lets layout.gadget align their label column consistently.
     */
    window_object = WindowObject,
        WA_Title, AP_TR(MSG_WINDOW_TITLE, "AmiAirPrint Prefs"),
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_DepthGadget, TRUE,
        WA_DragBar, TRUE,
        WA_SizeGadget, TRUE,
        WA_AutoAdjust, TRUE,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP,

        WINDOW_ParentGroup, VLayoutObject,
            LAYOUT_SpaceOuter, TRUE,
            LAYOUT_SpaceInner, TRUE,

            LAYOUT_AddChild, HLayoutObject,
                LAYOUT_SpaceInner, TRUE,

                LAYOUT_AddChild, gui->gadgets[GID_HOST] = StringObject,
                    GA_ID, GID_HOST,
                    GA_RelVerify, TRUE,
                    GA_TabCycle, TRUE,
                    STRINGA_TextVal, gui->prefs.host,
                    STRINGA_MaxChars, AP_PREFS_HOST_LEN - 1U,
                StringEnd,
                CHILD_NominalSize, TRUE,

                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_SpaceInner, TRUE,
                    LAYOUT_HorizAlignment, LALIGN_RIGHT,

                    LAYOUT_AddChild, gui->gadgets[GID_PORT] = IntegerObject,
                        GA_ID, GID_PORT,
                        GA_RelVerify, TRUE,
                        GA_TabCycle, TRUE,
                        INTEGER_Number, gui->prefs.port,
                        INTEGER_Minimum, 1,
                        INTEGER_Maximum, 65535,
                        INTEGER_MaxChars, 5,
                        INTEGER_Arrows, FALSE,
                    IntegerEnd,
                    CHILD_Label, LabelObject,
                        LABEL_Text, AP_TR(MSG_LABEL_PORT, "Port"),
                    LabelEnd,
                    CHILD_NominalSize, TRUE,
                LayoutEnd,
                CHILD_WeightedWidth, 0,

                LAYOUT_AddChild, gui->gadgets[GID_QUERY] = ButtonObject,
                    GA_ID, GID_QUERY,
                    GA_RelVerify, TRUE,
                    GA_TabCycle, TRUE,
                    GA_Text, AP_TR(MSG_BUTTON_QUERY_REACTION, "_Query printer"),
                    BUTTON_TextPadding, TRUE,
                ButtonEnd,
                CHILD_NominalSize, TRUE,
                CHILD_WeightedWidth, 0,
            LayoutEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_PRINTER_IP, "Printer IP"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, HLayoutObject,
                LAYOUT_SpaceInner, TRUE,

                LAYOUT_AddChild, gui->gadgets[GID_PATH] = StringObject,
                    GA_ID, GID_PATH,
                    GA_RelVerify, TRUE,
                    GA_TabCycle, TRUE,
                    STRINGA_TextVal, gui->prefs.path,
                    STRINGA_MaxChars, AP_PREFS_PATH_LEN - 1U,
                StringEnd,
                CHILD_NominalSize, TRUE,

                LAYOUT_AddChild, gui->gadgets[GID_SEARCH] = ButtonObject,
                    GA_ID, GID_SEARCH,
                    GA_RelVerify, TRUE,
                    GA_TabCycle, TRUE,
                    GA_Text, AP_TR(MSG_BUTTON_SEARCH_REACTION, "_Search..."),
                    BUTTON_TextPadding, TRUE,
                ButtonEnd,
                CHILD_NominalSize, TRUE,
                CHILD_WeightedWidth, 0,
            LayoutEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_IPP_PATH, "IPP path"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_MODEL] = ap_gui_readonly_string(
                GID_MODEL, gui->model_text, AP_CAPS_TEXT_LEN - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_PRINTER, "Printer"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_STATUS] = ap_gui_readonly_string(
                GID_STATUS, gui->status_text, sizeof(gui->status_text) - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_STATUS, "Status"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_AIRPRINT] = ap_gui_readonly_string(
                GID_AIRPRINT, gui->airprint_text, sizeof(gui->airprint_text) - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_PROTOCOL, "Protocol"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_RESOLUTION] = ap_gui_readonly_string(
                GID_RESOLUTION, gui->resolution_text, sizeof(gui->resolution_text) - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_RESOLUTION, "Resolution"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_DUPLEX] = ap_gui_readonly_string(
                GID_DUPLEX, gui->duplex_text, sizeof(gui->duplex_text) - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_DUPLEX, "Duplex"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_INK] = ap_gui_readonly_string(
                GID_INK, gui->ink_text, sizeof(gui->ink_text) - 1U),
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_INK, "Ink"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_COLOR] = ChooserObject,
                GA_ID, GID_COLOR,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                GA_Disabled, TRUE,
                CHOOSER_PopUp, TRUE,
                CHOOSER_LabelArray, g_not_queried_labels,
                CHOOSER_Selected, 0,
                CHOOSER_AutoFit, TRUE,
            ChooserEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_COLOR_MODE, "Color mode"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_QUALITY] = ChooserObject,
                GA_ID, GID_QUALITY,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                GA_Disabled, TRUE,
                CHOOSER_PopUp, TRUE,
                CHOOSER_LabelArray, g_not_queried_labels,
                CHOOSER_Selected, 0,
                CHOOSER_AutoFit, TRUE,
            ChooserEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_QUALITY, "Quality"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_MEDIA] = ChooserObject,
                GA_ID, GID_MEDIA,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                GA_Disabled, TRUE,
                CHOOSER_PopUp, TRUE,
                CHOOSER_LabelArray, g_not_queried_labels,
                CHOOSER_Selected, 0,
                CHOOSER_AutoFit, TRUE,
                CHOOSER_MaxLabels, 12,
            ChooserEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_PAPER, "Paper"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_ORIENTATION] = ChooserObject,
                GA_ID, GID_ORIENTATION,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                CHOOSER_PopUp, TRUE,
                CHOOSER_LabelArray, g_orientation_labels,
                CHOOSER_Selected, strcmp(gui->prefs.orientation, "landscape") == 0 ? 1 : 0,
                CHOOSER_AutoFit, TRUE,
            ChooserEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_ORIENTATION, "Orientation"),
            LabelEnd,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_SCALE] = IntegerObject,
                GA_ID, GID_SCALE,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                INTEGER_Number, gui->prefs.scale_percent,
                INTEGER_Minimum, 10,
                INTEGER_Maximum, 100,
                INTEGER_MaxChars, 3,
                INTEGER_Arrows, FALSE,
            IntegerEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_SCALE_PERCENT, "Scale (%)"),
            LabelEnd,
            CHILD_MinWidth, 56,
            CHILD_MaxWidth, 56,
            CHILD_WeightedWidth, 0,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            LAYOUT_AddChild, gui->gadgets[GID_CENTER] = CheckBoxObject,
                GA_ID, GID_CENTER,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                GA_Text, AP_TR(MSG_CENTER_ON_PAPER, "Center on paper"),
                CHECKBOX_TextPlace, PLACETEXT_RIGHT,
                CHECKBOX_Checked, gui->prefs.center_on_paper ? TRUE : FALSE,
            CheckBoxEnd,
            CHILD_Label, LCLABEL_NOLABEL,
            CHILD_NominalSize, TRUE,
            CHILD_WeightedHeight, 0,

            /* Action buttons use configured interspacing as well. */
            LAYOUT_AddChild, HLayoutObject,

                LAYOUT_AddChild, HLayoutObject,
                LayoutEnd,
                CHILD_WeightedWidth, 1,

                LAYOUT_AddChild, HLayoutObject,
                    LAYOUT_SpaceInner, TRUE,
                    LAYOUT_EvenSize, TRUE,

                    LAYOUT_AddChild, gui->gadgets[GID_TESTPAGE] = ButtonObject,
                        GA_ID, GID_TESTPAGE,
                        GA_RelVerify, TRUE,
                        GA_TabCycle, TRUE,
                        GA_Text, AP_TR(MSG_BUTTON_TESTPAGE_REACTION, "_Testpage"),
                        BUTTON_TextPadding, TRUE,
                    ButtonEnd,
                    CHILD_NominalSize, TRUE,
                    CHILD_WeightedWidth, 0,

                    LAYOUT_AddChild, gui->gadgets[GID_SAVE] = ButtonObject,
                        GA_ID, GID_SAVE,
                        GA_RelVerify, TRUE,
                        GA_TabCycle, TRUE,
                        GA_Text, AP_TR(MSG_BUTTON_SAVE_REACTION, "_Save"),
                        BUTTON_TextPadding, TRUE,
                    ButtonEnd,
                    CHILD_NominalSize, TRUE,
                    CHILD_WeightedWidth, 0,

                    LAYOUT_AddChild, gui->gadgets[GID_CANCEL] = ButtonObject,
                        GA_ID, GID_CANCEL,
                        GA_RelVerify, TRUE,
                        GA_TabCycle, TRUE,
                        GA_Text, AP_TR(MSG_BUTTON_CANCEL_REACTION, "_Cancel"),
                        BUTTON_TextPadding, TRUE,
                    ButtonEnd,
                    CHILD_NominalSize, TRUE,
                    CHILD_WeightedWidth, 0,
                LayoutEnd,
                CHILD_WeightedWidth, 0,

                LAYOUT_AddChild, HLayoutObject,
                LayoutEnd,
                CHILD_WeightedWidth, 1,
            LayoutEnd,
            CHILD_Label, LCLABEL_NOLABEL,
            CHILD_WeightedHeight, 0,
        LayoutEnd,
    WindowEnd;

    return window_object;
}

static unsigned int ap_gui_find_color(const struct APGUI *gui, const char *key)
{
    unsigned int i;

    for (i = 0U; i < gui->color_count; ++i) {
        if (strcmp(gui->color_keys[i], key) == 0) {
            return i;
        }
    }
    return 0U;
}

static unsigned int ap_gui_find_quality(const struct APGUI *gui, unsigned int quality)
{
    unsigned int i;

    for (i = 0U; i < gui->quality_count; ++i) {
        if (gui->quality_values[i] == quality) {
            return i;
        }
    }
    return 0U;
}

static unsigned int ap_gui_find_media(const struct APGUI *gui, const char *key)
{
    unsigned int i;

    for (i = 0U; i < gui->media_count; ++i) {
        if (strcmp(gui->media_keys[i], key) == 0) {
            return i;
        }
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
        /* Old capability snapshots may still contain custom range bounds. */
        if (strncmp(gui->caps.media[i], "custom_min_", 11U) == 0 ||
            strncmp(gui->caps.media[i], "custom_max_", 11U) == 0) {
            continue;
        }
        friendly = ap_locale_media_name(gui->caps.media[i]);
        snprintf(gui->media_label_storage[gui->media_count],
                 sizeof(gui->media_label_storage[0]),
                 "%s",
                 friendly);
        gui->media_keys[gui->media_count] = gui->caps.media[i];
        gui->media_labels[gui->media_count] = (STRPTR)gui->media_label_storage[gui->media_count];
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
        if (written < 0 || (size_t)written >= sizeof(gui->ink_text) - used) {
            break;
        }
        used += (size_t)written;
    }

    if (gui->ink_text[0] == '\0') {
        snprintf(gui->ink_text, sizeof(gui->ink_text), "%s", AP_TR(MSG_NOT_REPORTED, "Not reported"));
    }
}

static void ap_gui_update_capabilities(struct APGUI *gui)
{
    unsigned int color_selected;
    unsigned int quality_selected;
    unsigned int media_selected;
    const char *preferred_color;
    unsigned int preferred_quality;
    const char *preferred_media;

    snprintf(gui->model_text,
             sizeof(gui->model_text),
             "%s",
             gui->caps.model[0] != '\0' ? gui->caps.model : AP_TR(MSG_UNKNOWN_PRINTER, "Unknown printer"));

    snprintf(gui->status_text,
             sizeof(gui->status_text),
             "%s%s%s",
             ap_locale_state_name(gui->caps.printer_state),
             gui->caps.accepting_jobs == 0 ? AP_TR(MSG_SUFFIX_NOT_ACCEPTING, " - not accepting jobs") : "",
             gui->caps.state_reason[0] != '\0' && strcmp(gui->caps.state_reason, "none") != 0
                 ? AP_TR(MSG_SUFFIX_CHECK_STATUS, " - check printer status") : "");

    if (gui->caps.airprint_version[0] != '\0') {
        snprintf(gui->airprint_text,
                 sizeof(gui->airprint_text),
                 AP_TR(MSG_FORMAT_AIRPRINT_IPP, "AirPrint %s / IPP %u.%u"),
                 gui->caps.airprint_version,
                 (unsigned int)gui->caps.ipp_version_major,
                 (unsigned int)gui->caps.ipp_version_minor);
    } else {
        snprintf(gui->airprint_text,
                 sizeof(gui->airprint_text),
                 AP_TR(MSG_FORMAT_IPP, "IPP %u.%u"),
                 (unsigned int)gui->caps.ipp_version_major,
                 (unsigned int)gui->caps.ipp_version_minor);
    }

    if (gui->caps.resolution_default.x != 0U) {
        snprintf(gui->resolution_text,
                 sizeof(gui->resolution_text),
                 AP_TR(MSG_FORMAT_DPI, "%lux%lu dpi"),
                 (unsigned long)gui->caps.resolution_default.x,
                 (unsigned long)gui->caps.resolution_default.y);
    } else if (gui->caps.resolution_count != 0U) {
        snprintf(gui->resolution_text,
                 sizeof(gui->resolution_text),
                 AP_TR(MSG_FORMAT_DPI, "%lux%lu dpi"),
                 (unsigned long)gui->caps.resolutions[0].x,
                 (unsigned long)gui->caps.resolutions[0].y);
    } else {
        snprintf(gui->resolution_text, sizeof(gui->resolution_text), "%s", AP_TR(MSG_NOT_REPORTED, "Not reported"));
    }

    snprintf(gui->duplex_text,
             sizeof(gui->duplex_text),
             "%s",
             gui->caps.duplex_supported ? AP_TR(MSG_SUPPORTED, "Supported") : AP_TR(MSG_NOT_SUPPORTED, "Not supported"));

    ap_gui_format_ink(gui);
    ap_gui_build_options(gui);

    preferred_color = gui->prefs.color_mode[0] != '\0'
        ? gui->prefs.color_mode : gui->caps.color_default;
    preferred_quality = gui->prefs.quality != 0U
        ? gui->prefs.quality : gui->caps.quality_default;
    preferred_media = gui->prefs.media[0] != '\0'
        ? gui->prefs.media : gui->caps.media_default;

    color_selected = ap_gui_find_color(gui, preferred_color);
    quality_selected = ap_gui_find_quality(gui, preferred_quality);
    media_selected = ap_gui_find_media(gui, preferred_media);

    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_MODEL], gui->window, NULL,
                   STRINGA_TextVal, gui->model_text, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_STATUS], gui->window, NULL,
                   STRINGA_TextVal, gui->status_text, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_AIRPRINT], gui->window, NULL,
                   STRINGA_TextVal, gui->airprint_text, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_RESOLUTION], gui->window, NULL,
                   STRINGA_TextVal, gui->resolution_text, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_DUPLEX], gui->window, NULL,
                   STRINGA_TextVal, gui->duplex_text, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_INK], gui->window, NULL,
                   STRINGA_TextVal, gui->ink_text, TAG_END);

    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_COLOR], gui->window, NULL,
                   CHOOSER_LabelArray, gui->color_count != 0U ? gui->color_labels : g_not_queried_labels,
                   CHOOSER_Selected, color_selected,
                   GA_Disabled, gui->color_count == 0U,
                   TAG_END);

    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_QUALITY], gui->window, NULL,
                   CHOOSER_LabelArray, gui->quality_count != 0U ? gui->quality_labels : g_not_queried_labels,
                   CHOOSER_Selected, quality_selected,
                   GA_Disabled, gui->quality_count == 0U,
                   TAG_END);

    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_MEDIA], gui->window, NULL,
                   CHOOSER_LabelArray, gui->media_count != 0U ? gui->media_labels : g_not_queried_labels,
                   CHOOSER_Selected, media_selected,
                   GA_Disabled, gui->media_count == 0U,
                   TAG_END);
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

static ULONG ap_gui_discovery_text_width(const struct APGUI *gui, const char *text)
{
    struct IntuiText itext;
    LONG width;

    if (gui == NULL || gui->window == NULL || gui->window->WScreen == NULL ||
        gui->window->WScreen->Font == NULL || text == NULL || text[0] == '\0')
        return 0UL;

    memset(&itext, 0, sizeof(itext));
    itext.ITextFont = gui->window->WScreen->Font;
    itext.IText = (UBYTE *)text;
    width = IntuiTextLength(&itext);
    return width > 0L ? (ULONG)width : 0UL;
}

static int ap_gui_choose_discovered(struct APGUI *gui, unsigned int *selected_index)
{
    enum {
        GID_DISCOVERY_LIST = 1001,
        GID_DISCOVERY_USE,
        GID_DISCOVERY_CANCEL
    };
    Object *window_object;
    Object *listbrowser;
    struct Window *window;
    struct List printer_list;
    ULONG window_signal;
    ULONG chooser_width;
    ULONG longest_entry_width;
    ULONG screen_width;
    unsigned int i;
    int done;
    int accepted;

    if (gui == NULL || selected_index == NULL || gui->discovery.count == 0U)
        return 0;

    ap_gui_prepare_discovery_labels(gui);

    /* Size the requester from the longest visible printer entry instead of
     * from the main-window width.  The reserve covers the left "Printer"
     * label, listbrowser borders/scrollbar and the window's inner spacing. */
    longest_entry_width = 0UL;
    for (i = 0U; i < gui->discovery.count && i < AP_DISCOVERY_MAX_PRINTERS; ++i) {
        ULONG entry_width;
        entry_width = ap_gui_discovery_text_width(gui, gui->discovery_labels[i]);
        if (entry_width > longest_entry_width) longest_entry_width = entry_width;
    }

    chooser_width = longest_entry_width + 120UL;
    if (chooser_width < 300UL) chooser_width = 300UL;

    screen_width = 0UL;
    if (gui->window != NULL && gui->window->WScreen != NULL)
        screen_width = (ULONG)gui->window->WScreen->Width;
    if (screen_width != 0UL) {
        ULONG screen_limit;
        screen_limit = screen_width > 40UL ? screen_width - 20UL : screen_width;
        if (chooser_width > screen_limit) chooser_width = screen_limit;
    }

    printer_list.lh_Head = (struct Node *)&printer_list.lh_Tail;
    printer_list.lh_Tail = NULL;
    printer_list.lh_TailPred = (struct Node *)&printer_list.lh_Head;

    for (i = 0U; i < gui->discovery.count && i < AP_DISCOVERY_MAX_PRINTERS; ++i) {
        struct TagItem node_tags[4];
        struct Node *node;

        node_tags[0].ti_Tag = LBNA_Column;
        node_tags[0].ti_Data = 0UL;
        node_tags[1].ti_Tag = LBNCA_Text;
        node_tags[1].ti_Data = (ULONG)gui->discovery_labels[i];
        node_tags[2].ti_Tag = LBNCA_CopyText;
        node_tags[2].ti_Data = TRUE;
        node_tags[3].ti_Tag = TAG_END;
        node_tags[3].ti_Data = 0UL;

        node = AllocListBrowserNodeA(1U, node_tags);
        if (node == NULL) {
            FreeListBrowserList(&printer_list);
            return 0;
        }
        node->ln_Pri = (BYTE)i;
        AddTail(&printer_list, node);
    }

    listbrowser = NULL;
    window_object = WindowObject,
        WA_Title, AP_TR(MSG_DISCOVERY_WINDOW_TITLE, "Select printer"),
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_DepthGadget, TRUE,
        WA_DragBar, TRUE,
        WA_SizeGadget, TRUE,
        WA_AutoAdjust, TRUE,
        WA_Width, chooser_width,
        WINDOW_Position, WPOS_CENTERSCREEN,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP,

        WINDOW_ParentGroup, VLayoutObject,
            LAYOUT_SpaceOuter, TRUE,
            LAYOUT_SpaceInner, TRUE,

            LAYOUT_AddChild, listbrowser = ListBrowserObject,
                GA_ID, GID_DISCOVERY_LIST,
                GA_RelVerify, TRUE,
                GA_TabCycle, TRUE,
                GA_ReadOnly, FALSE,
                LISTBROWSER_Labels, &printer_list,
                LISTBROWSER_Selected, 0,
                LISTBROWSER_ShowSelected, TRUE,
                LISTBROWSER_MinVisible, 4,
            ListBrowserEnd,
            CHILD_Label, LabelObject,
                LABEL_Text, AP_TR(MSG_LABEL_PRINTER, "Printer"),
            LabelEnd,

            LAYOUT_AddChild, HLayoutObject,
                LAYOUT_SpaceInner, TRUE,
                LAYOUT_AddChild, ButtonObject,
                    GA_ID, GID_DISCOVERY_USE,
                    GA_RelVerify, TRUE,
                    GA_Text, AP_TR(MSG_BUTTON_DISCOVERY_USE_REACTION, "_Use"),
                    BUTTON_TextPadding, TRUE,
                ButtonEnd,
                LAYOUT_AddChild, ButtonObject,
                    GA_ID, GID_DISCOVERY_CANCEL,
                    GA_RelVerify, TRUE,
                    GA_Text, AP_TR(MSG_BUTTON_DISCOVERY_CANCEL_REACTION, "_Cancel"),
                    BUTTON_TextPadding, TRUE,
                ButtonEnd,
            LayoutEnd,
            CHILD_WeightedHeight, 0,
        LayoutEnd,
    WindowEnd;

    if (window_object == NULL) {
        FreeListBrowserList(&printer_list);
        return 0;
    }

    window = RA_OpenWindow(window_object);
    if (window == NULL) {
        SetAttrs(listbrowser, LISTBROWSER_Labels, ~0UL, TAG_END);
        DisposeObject(window_object);
        FreeListBrowserList(&printer_list);
        return 0;
    }

    window_signal = 0U;
    GetAttr(WINDOW_SigMask, window_object, &window_signal);
    done = 0;
    accepted = 0;
    while (!done) {
        ULONG received;
        ULONG result;
        UWORD code;

        received = Wait(window_signal | SIGBREAKF_CTRL_C);
        if ((received & SIGBREAKF_CTRL_C) != 0U) break;

        while ((result = RA_HandleInput(window_object, &code)) != WMHI_LASTMSG) {
            switch (result & WMHI_CLASSMASK) {
                case WMHI_CLOSEWINDOW:
                    done = 1;
                    break;
                case WMHI_GADGETUP:
                    switch (result & WMHI_GADGETMASK) {
                        case GID_DISCOVERY_LIST:
                            /* LISTBROWSER_Selected is also returned in code. */
                            (void)code;
                            break;
                        case GID_DISCOVERY_USE:
                        {
                            ULONG selected;
                            selected = 0U;
                            GetAttr(LISTBROWSER_Selected, listbrowser, &selected);
                            if (selected < gui->discovery.count) {
                                *selected_index = (unsigned int)selected;
                                accepted = 1;
                            }
                            done = 1;
                            break;
                        }
                        case GID_DISCOVERY_CANCEL:
                            done = 1;
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    RA_CloseWindow(window_object);
    SetAttrs(listbrowser, LISTBROWSER_Labels, ~0UL, TAG_END);
    DisposeObject(window_object);
    FreeListBrowserList(&printer_list);
    return accepted;
}

static void ap_gui_apply_discovered(struct APGUI *gui,
                                    const struct APDiscoveredPrinter *printer)
{
    if (gui == NULL || printer == NULL) return;

    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", printer->address);
    gui->prefs.port = (unsigned int)printer->port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", printer->path);
    gui->queried = 0;

    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_HOST], gui->window, NULL,
                   STRINGA_TextVal, gui->prefs.host, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_PORT], gui->window, NULL,
                   INTEGER_Number, gui->prefs.port, TAG_END);
    SetGadgetAttrs((struct Gadget *)gui->gadgets[GID_PATH], gui->window, NULL,
                   STRINGA_TextVal, gui->prefs.path, TAG_END);
    ap_gui_set_status(gui,
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

    host_value = NULL;
    path_value = NULL;
    port_value = 631U;

    GetAttr(STRINGA_TextVal, gui->gadgets[GID_HOST], (ULONG *)&host_value);
    GetAttr(STRINGA_TextVal, gui->gadgets[GID_PATH], (ULONG *)&path_value);
    GetAttr(INTEGER_Number, gui->gadgets[GID_PORT], &port_value);

    if (host_value == NULL || host_value[0] == '\0') {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENTER_IPV4, "Enter the printer IPv4 address first"));
        return 0;
    }
    if (path_value == NULL || path_value[0] != '/') {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_IPP_PATH_SLASH, "IPP path must start with '/'"));
        return 0;
    }
    if (port_value == 0U || port_value > 65535U) {
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

    if (!ap_gui_read_address(gui, host, sizeof(host), path, sizeof(path), &port)) {
        return 0;
    }

    ap_gui_set_status(gui, AP_TR(MSG_STATUS_QUERYING, "Querying printer..."));

    if (!ap_caps_query(host,
                       (uint16_t)port,
                       path,
                       &gui->caps,
                       error_text,
                       sizeof(error_text))) {
        char message[192];
        snprintf(message, sizeof(message), AP_TR(MSG_FORMAT_QUERY_FAILED, "Query failed: %s"), error_text);
        ap_gui_set_status(gui, message);
        return 0;
    }

    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", host);
    gui->prefs.port = port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", path);

    ap_gui_update_capabilities(gui);
    gui->queried = 1;
    return 1;
}

static int ap_gui_capture_selections(struct APGUI *gui)
{
    char host[AP_PREFS_HOST_LEN];
    char path[AP_PREFS_PATH_LEN];
    unsigned int port;
    ULONG selected;

    if (!ap_gui_read_address(gui, host, sizeof(host), path, sizeof(path), &port)) {
        return 0;
    }

    snprintf(gui->prefs.host, sizeof(gui->prefs.host), "%s", host);
    gui->prefs.port = port;
    snprintf(gui->prefs.path, sizeof(gui->prefs.path), "%s", path);

    if (gui->color_count != 0U) {
        selected = 0U;
        GetAttr(CHOOSER_Selected, gui->gadgets[GID_COLOR], &selected);
        if (selected < gui->color_count) {
            snprintf(gui->prefs.color_mode,
                     sizeof(gui->prefs.color_mode),
                     "%s",
                     gui->color_keys[selected]);
        }
    }

    if (gui->quality_count != 0U) {
        selected = 0U;
        GetAttr(CHOOSER_Selected, gui->gadgets[GID_QUALITY], &selected);
        if (selected < gui->quality_count) {
            gui->prefs.quality = gui->quality_values[selected];
        }
    }

    if (gui->media_count != 0U) {
        selected = 0U;
        GetAttr(CHOOSER_Selected, gui->gadgets[GID_MEDIA], &selected);
        if (selected < gui->media_count) {
            snprintf(gui->prefs.media,
                     sizeof(gui->prefs.media),
                     "%s",
                     gui->media_keys[selected]);
        }
    }

    selected = 0U;
    GetAttr(CHOOSER_Selected, gui->gadgets[GID_ORIENTATION], &selected);
    snprintf(gui->prefs.orientation,
             sizeof(gui->prefs.orientation),
             "%s",
             selected == 1U ? "landscape" : "portrait");

    selected = 100U;
    GetAttr(INTEGER_Number, gui->gadgets[GID_SCALE], &selected);
    if (selected < 10U) selected = 10U;
    if (selected > 100U) selected = 100U;
    gui->prefs.scale_percent = (unsigned int)selected;

    selected = 0U;
    GetAttr(CHECKBOX_Checked, gui->gadgets[GID_CENTER], &selected);
    gui->prefs.center_on_paper = selected != 0U;

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

    if (!ap_gui_capture_selections(gui)) {
        return 0;
    }

    ap_gui_set_status(gui, AP_TR(MSG_STATUS_PRINTING_TEST, "Printing test page..."));

    if (!ap_print_document(&gui->prefs,
                           &gui->caps,
                           gui->queried,
                           g_airprint_testpage_jpeg,
                           g_airprint_testpage_jpeg_len,
                           "image/jpeg",
                           AP_TR(MSG_TESTPAGE_JOB_NAME, "AmigaOS AirPrint Test Page"),
                           &print_result,
                           error_text,
                           sizeof(error_text))) {
        snprintf(message,
                 sizeof(message),
                 AP_TR(MSG_FORMAT_TEST_FAILED, "Test page failed: %s"),
                 error_text[0] != '\0' ? error_text : AP_TR(MSG_UNKNOWN_PRINT_ERROR, "Unknown print error"));
        ap_gui_set_status(gui, message);
        return 0;
    }

    if (print_result.job_id != 0UL) {
        snprintf(message,
                 sizeof(message),
                 AP_TR(MSG_FORMAT_TEST_SUCCESS_JOB, "Test page sent successfully - job %lu"),
                 print_result.job_id);
        ap_gui_set_status(gui, message);
    } else {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_TEST_SUCCESS, "Test page sent successfully"));
    }

    return 1;
}

static int ap_gui_store(struct APGUI *gui, int permanent)
{
    if (!gui->queried) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_QUERY_BEFORE_SAVE, "Query the printer before saving settings"));
        return 0;
    }

    if (!ap_gui_capture_selections(gui)) {
        return 0;
    }

    if (!ap_prefs_write_env(&gui->prefs, &gui->caps, gui->queried)) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENV_WRITE_FAILED, "Could not write ENV:AirPrint.prefs"));
        return 0;
    }

    if (permanent && !ap_prefs_write_envarc(&gui->prefs, &gui->caps, gui->queried)) {
        ap_gui_set_status(gui, AP_TR(MSG_STATUS_ENVARC_WRITE_FAILED, "ENV updated, but ENVARC:AirPrint.prefs failed"));
        return 0;
    }

    return 1;
}

static int ap_gui_run(struct APGUI *gui)
{
    ULONG window_signal;
    int done;
    int result_code;

    gui->window = RA_OpenWindow(gui->window_object);
    if (gui->window == NULL) {
        return 20;
    }

    /*
     * Use the natural size calculated by layout.gadget as the minimum.
     * This lets localized labels and the spacing selected in ReAction.prefs
     * determine the initial geometry.  The window may be widened by roughly
     * one third; height remains fixed at the natural layout height so vertical
     * resizing cannot create empty space below the action buttons.
     * WindowLimits() uses outer dimensions.
     */
    {
        ULONG natural_width;
        ULONG natural_height;
        ULONG max_width;

        natural_width = (ULONG)gui->window->Width;
        natural_height = (ULONG)gui->window->Height;
        max_width = (natural_width * 4U) / 3U;

        WindowLimits(gui->window,
                     natural_width,
                     natural_height,
                     max_width,
                     natural_height);
    }

    window_signal = 0U;
    GetAttr(WINDOW_SigMask, gui->window_object, &window_signal);

    if (gui->queried) {
        ap_gui_update_capabilities(gui);
    }

    done = 0;
    result_code = 0;

    while (!done) {
        ULONG received;
        ULONG result;
        UWORD code;

        received = Wait(window_signal | SIGBREAKF_CTRL_C);
        if ((received & SIGBREAKF_CTRL_C) != 0U) {
            break;
        }

        while ((result = RA_HandleInput(gui->window_object, &code)) != WMHI_LASTMSG) {
            switch (result & WMHI_CLASSMASK) {
                case WMHI_CLOSEWINDOW:
                    done = 1;
                    break;

                case WMHI_GADGETUP:
                    switch (result & WMHI_GADGETMASK) {
                        case GID_HOST:
                        case GID_PORT:
                        case GID_PATH:
                            gui->queried = 0;
                            ap_gui_set_status(gui, AP_TR(MSG_STATUS_ADDRESS_CHANGED, "Address changed - query printer again"));
                            break;

                        case GID_QUERY:
                            ap_gui_query(gui);
                            break;

                        case GID_SEARCH:
                            (void)ap_gui_search(gui);
                            break;

                        case GID_TESTPAGE:
                            (void)ap_gui_testpage(gui);
                            break;

                        case GID_SAVE:
                            if (ap_gui_store(gui, 1)) {
                                done = 1;
                            }
                            break;

                        case GID_CANCEL:
                            done = 1;
                            break;

                        default:
                            break;
                    }
                    break;

                default:
                    break;
            }
        }
    }

    RA_CloseWindow(gui->window_object);
    gui->window = NULL;
    return result_code;
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

    if (!ap_gui_open_classes()) {
        printf(AP_TR(MSG_CONSOLE_REACTION_REQUIRED,
                     "AmiAirPrintPrefs %s requires AmigaOS 3.2 ReAction classes."),
               AIRPRINT_PREFS_VERSION);
        printf("\n");
        ap_gui_close_classes();
        ap_locale_close();
        return 20;
    }

    if (!ap_http_open()) {
        printf(AP_TR(MSG_CONSOLE_BSDSOCKET_FAILED,
                     "Could not open bsdsocket.library: %s"),
               ap_http_last_error());
        printf("\n");
        ap_gui_close_classes();
        ap_locale_close();
        return 20;
    }

    gui->window_object = ap_gui_build_window(gui);
    if (gui->window_object == NULL) {
        printf("%s\n", AP_TR(MSG_CONSOLE_REACTION_WINDOW_FAILED,
                              "Could not create ReAction window."));
        ap_http_close();
        ap_gui_close_classes();
        ap_locale_close();
        return 20;
    }

    result = ap_gui_run(gui);

    DisposeObject(gui->window_object);
    ap_http_close();
    ap_gui_close_classes();
    ap_locale_close();

    return result;
}
