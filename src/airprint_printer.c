#include "airprint_device.h"
#include "airprint_document.h"
#include "airprint_text_font.h"
#include "ami_airprint_brand.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/preferences.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <devices/prtgfx.h>
#include <proto/exec.h>
#include <proto/dos.h>

/*
 * AmiAirPrint printer.device driver, release 1.2 (segment 43.43).
 *
 * printer.device performs the Amiga-side source decoding, scaling and color
 * conversion and calls AP_Render() once per destination row. This driver
 * turns those prepared rows into PWG Raster and sends them directly to
 * airprint.device through Exec device I/O.  The classic parallel/serial
 * printer port is intentionally not used for AirPrint payload data.
 *
 * v0.9.16 fixes the freestanding 68000 __mulsi3 dependency in the variable
 * pixel-size PWG encoder and adds driver-side Scale (10..100 percent) plus
 * optional horizontal/vertical page centering. The verified media, orientation
 * and color-mode paths remain otherwise unchanged.
 * v0.9.17 is a build-only ReAction/GadTools compatibility hotfix; raster
 * behavior is unchanged.
 * v0.9.19 adds a local freestanding 68000 __mulsi3 runtime primitive so GCC
 * cannot introduce an unresolved libgcc multiplication dependency.
 * v0.9.20 refines Center on paper: horizontal placement is calculated from
 * each rendered row's actual pi_ScaleX pixel width instead of the Case-0
 * nominal picture width. This removes the small short-edge centering error
 * observed with ASPECT_VERT landscape output while leaving the verified
 * media/orientation/scale/color paths unchanged.
 * v0.9.21 opens airprint.device directly from the printer driver, removing
 * the dependency on the AmigaOS 3.2 custom printer-port feature.  This makes
 * the same driver transport usable on AmigaOS 3.0/3.1 (with Parallel selected
 * as an unused compatibility port) and fixes sGray output by using PCC_BGR's
 * PCMWHITE component without an erroneous second inversion.
 * v0.9.22 adds native processed text printing for PRT:/CMD_WRITE.  The classic
 * printer.device ConvFunc/DoSpecial hooks consume the alphanumeric stream,
 * rasterize it with an embedded Public Domain Latin-1 bitmap font, and submit
 * page-oriented PWG Raster jobs through the same direct airprint.device path.
 * v0.9.23 fixes ANSI/Amiga printer escape handling: ESC (0x1b) and CSI (0x9b)
 * are deliberately returned to printer.device so its command parser can
 * consume the complete sequence and dispatch the resulting command through
 * DoSpecial().  Printable text remains consumed by ConvFunc and therefore
 * never reaches the unused compatibility printer port.
 *
 * Release 1.2 / 43.43 fixes page-boundary handling after a graphics dump
 * closed with SPECIAL_NOFORMFEED.  A later form feed/reset now closes that
 * pending physical graphics page even when no text was added in between.
 * This preserves FinalWriter strip continuation while allowing applications
 * such as Wordworth to delimit consecutive full-page graphics dumps.
 *
 * Release 1.0 PersonalPaint compatibility implements the documented Case-5
 * SetDensity behavior (100/150/200/300/600 dpi logical rasters expanded to
 * the fixed 600-dpi PWG page) and isolates printer.device's unused primitive
 * serial/parallel transport while AmiAirPrint is open.  This prevents legacy
 * applications from timing out on a physical dummy port before PRD_DUMPRPORT
 * reaches the driver's Render() callback.
 */

#define AIRPRINT_DRIVER_VERSION 43
#define AIRPRINT_DRIVER_REVISION 43

#define AP_PWG_HEADER_SIZE 1796UL
#define AP_PWG_PREFIX_SIZE (4UL + AP_PWG_HEADER_SIZE)
#define AP_MEDIA_KEY_LEN 64UL
#define AP_DEFAULT_MEDIA "iso_a4_210x297mm"
#define AP_DEFAULT_WIDTH_PIXELS 4960UL
#define AP_DEFAULT_HEIGHT_PIXELS 7015UL
#define AP_DEFAULT_WIDTH_POINTS 595UL
#define AP_DEFAULT_HEIGHT_POINTS 842UL

#define AP_PWG_COLORSPACE_SW   18UL
#define AP_PWG_COLORSPACE_SRGB 19UL
#define AP_PWG_COLOR_ORDER_CHUNKED 0UL

#define AP_TEXT_LEFT_EDGE 60UL
#define AP_TEXT_TOP_EDGE 100UL
#define AP_TEXT_BOTTOM_EDGE 100UL
#define AP_TEXT_MAX_COLUMNS 255U

#define AP_TEXT_STYLE_BOLD      0x01U
#define AP_TEXT_STYLE_UNDERLINE 0x02U
#define AP_TEXT_STYLE_ITALIC    0x04U

struct PrinterData *AP_PD __asm__("AP_PD");
struct ExecBase *SysBase __asm__("SysBase");
struct DosLibrary *DOSBase;
extern struct PrinterExtendedData AP_PEDData __asm__("AP_PEDData");

char AP_PrinterName[] __asm__("AP_PrinterName") = "AirPrint";

/* Keep the requested ownership string in the C object as well as printertag.s. */
const char AP_BrandText[] __attribute__((used)) = AMIAIRPRINT_BRAND_TEXT;

static const char AP_Unsupported[] = "\377";

/*
 * printer.device has 77 standard command slots (aRIS .. aRAW).  0xff is the
 * documented marker for commands that require DoSpecial processing (or are
 * unsupported).  AmiAirPrint keeps every entry on that path so no translated
 * printer escape sequence can leak to the unused compatibility printer port.
 */
const char *AP_Commands[77] __asm__("AP_Commands") = {
    AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported, AP_Unsupported,
    AP_Unsupported, AP_Unsupported, AP_Unsupported
};

/* Init/Open/Close/Expunge/DoSpecial entry glue is provided by printerglue.s. */
static ULONG ap_udiv_small(ULONG value, UWORD divisor);
static void ap_apply_page_prefs(void);
static void ap_release_buffer(void);
static void ap_text_update_geometry(void);
static void ap_text_reset_state(void);
static LONG ap_text_finish_page(int force_blank);
static LONG ap_primitive_write_sink(APTR data, LONG length);
static LONG ap_primitive_bothready_sink(void);
static void ap_install_primitive_io_sinks(void);
static void ap_restore_primitive_io(void);
static void ap_abort_active_job(void);
static LONG ap_finish_current_page(void);
static LONG ap_send_blank_rows(ULONG count);
static LONG ap_start_raster(void);
LONG AP_OpenSetup(void) __asm__("AP_OpenSetup");
void AP_CloseSetup(void) __asm__("AP_CloseSetup");
LONG AP_Render(LONG ct, LONG x, LONG y, LONG status) __asm__("AP_Render");
LONG AP_ConvFuncC(APTR output_buffer, LONG c, LONG crlf_flag) __asm__("AP_ConvFuncC");
LONG AP_DoSpecialC(UWORD *command, UBYTE *output_buffer, BYTE *vline,
                   BYTE *current_vmi, BYTE *crlf_flag, UBYTE *parms)
                   __asm__("AP_DoSpecialC");

static struct MsgPort *g_apdev_port;
static struct IOStdReq *g_apdev_io;
static int g_apdev_open;

static ULONG g_rows_sent;
static ULONG g_page_width;
static ULONG g_page_height;
static ULONG g_row_bytes;
static ULONG g_encoded_max;
static ULONG g_bytes_per_pixel;
static ULONG g_color_space;
static ULONG g_num_colors;
static int g_landscape;
static int g_monochrome;
static int g_sgray_supported;
static UWORD g_scale_percent = 100U;
static UWORD g_job_scale_percent = 100U;
static UWORD g_output_dpi = 600U;
static UWORD g_frontend_dpi = 600U;
static ULONG g_frontend_page_width;
static ULONG g_frontend_page_height;
static int g_center_on_paper;
static ULONG g_picture_width;
static ULONG g_picture_height;
static ULONG g_scaled_picture_width;
static ULONG g_scaled_picture_height;
static ULONG g_top_padding;
static ULONG g_vertical_dpi_accum;
static UWORD g_vertical_scale_accum;
static UWORD g_emit_current_rows;
static ULONG g_page_width_points;
static ULONG g_page_height_points;
static char g_media_keyword[AP_MEDIA_KEY_LEN] = AP_DEFAULT_MEDIA;
static int g_raster_started;
static UBYTE *g_raw_row;
static UBYTE *g_encoded_row;
static ULONG g_raw_alloc_size;
static ULONG g_encoded_alloc_size;
static UBYTE g_pwg_prefix[AP_PWG_PREFIX_SIZE];

enum {
    AP_ENGINE_PWG = 0,
    AP_ENGINE_PDF = 1,
    AP_ENGINE_POSTSCRIPT = 2
};
static UWORD g_engine = AP_ENGINE_PWG;
enum {
    AP_DUPLEX_ONE_SIDED = 0,
    AP_DUPLEX_LONG_EDGE = 1,
    AP_DUPLEX_SHORT_EDGE = 2
};
enum {
    AP_SHEET_BACK_NORMAL = 0,
    AP_SHEET_BACK_FLIPPED = 1,
    AP_SHEET_BACK_ROTATED = 2,
    AP_SHEET_BACK_MANUAL_TUMBLE = 3
};
static UWORD g_duplex_mode;
static int g_duplex_requested;
static int g_duplex_document_open;
/* A graphics dump closed with SPECIAL_NOFORMFEED is only one strip of the
 * current physical page.  Keep the document/page stream open between dump
 * requests and resume it at the next PRS_INIT. */
static int g_graphics_continuation_open;
static UWORD g_sheet_back = AP_SHEET_BACK_NORMAL;
static ULONG g_pwg_page_number;
static struct APDocumentWriter g_document_writer;

static UBYTE g_prefs_input[256];
static char g_prefs_line[128];
static LONG (*g_original_pwrite)();
static LONG (*g_original_pbothready)();
static int g_primitive_io_hooked;

/* Native alphanumeric page state.  printer.device is exclusive, so one
 * driver instance has only one active text stream at a time. */
static UBYTE g_text_cells[AP_TEXT_MAX_COLUMNS];
static UBYTE g_text_styles[AP_TEXT_MAX_COLUMNS];
static UWORD g_text_max_columns;
static UWORD g_text_left_margin;
static UWORD g_text_right_margin;
static UWORD g_text_column;
static UWORD g_text_cell_width;
static UWORD g_text_line_height;
static UWORD g_text_glyph_scale_x;
static UWORD g_text_glyph_scale_y;
static UWORD g_text_lines_per_page;
static UWORD g_text_line_on_page;
static UBYTE g_text_style_flags;
static int g_text_line_has_data;
static int g_text_page_touched;
static LONG g_text_error;

static void ap_memzero(UBYTE *p, ULONG count)
{
    while (count-- != 0UL) *p++ = 0U;
}

static void ap_memwhite(UBYTE *p, ULONG count)
{
    while (count-- != 0UL) *p++ = 0xFFU;
}

static void ap_copy_text(UBYTE *dst, ULONG capacity, const char *src)
{
    ULONG i = 0UL;
    if (capacity == 0UL) return;
    while (i + 1UL < capacity && src[i] != '\0') {
        dst[i] = (UBYTE)src[i];
        ++i;
    }
    dst[i] = 0U;
}

static void ap_put_be32(UBYTE *dst, ULONG value)
{
    dst[0] = (UBYTE)(value >> 24);
    dst[1] = (UBYTE)(value >> 16);
    dst[2] = (UBYTE)(value >> 8);
    dst[3] = (UBYTE)value;
}

static const UBYTE *ap_next_const_pixel(const UBYTE *pixel)
{
    return pixel + (g_bytes_per_pixel == 1UL ? 1UL : 3UL);
}

static UBYTE *ap_next_pixel(UBYTE *pixel)
{
    return pixel + (g_bytes_per_pixel == 1UL ? 1UL : 3UL);
}

static int ap_pixel_equal(const UBYTE *a, const UBYTE *b)
{
    if (g_bytes_per_pixel == 1UL) return a[0] == b[0];
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static ULONG ap_mul_small(ULONG value, UWORD factor)
{
    ULONG result = 0UL;
    while (factor != 0U) {
        if ((factor & 1U) != 0U) result += value;
        value <<= 1;
        factor >>= 1;
    }
    return result;
}

static ULONG ap_scale_dimension(ULONG value)
{
    ULONG scaled;
    if (g_job_scale_percent >= 100U) return value;
    scaled = ap_udiv_small(ap_mul_small(value, g_job_scale_percent) + 99UL, 100U);
    return scaled;
}

/*
 * PRS_PREINIT (Case 5) is the classic printer-driver SetDensity hook.
 * printer.device uses these PED values to compute the logical destination
 * raster before PRS_INIT.  AmiAirPrint always emits PWG Raster at 600 dpi,
 * therefore lower classic densities are expanded by an exact integer factor
 * when the prepared rows are copied into the PWG page.
 */
static UWORD ap_density_index(UWORD special)
{
    UWORD bits = (UWORD)(special & SPECIAL_DENSITYMASK);

    switch (bits) {
        case SPECIAL_DENSITY1: return 1U;
        case SPECIAL_DENSITY2: return 2U;
        case SPECIAL_DENSITY3: return 3U;
        case SPECIAL_DENSITY4: return 4U;
        case SPECIAL_DENSITY5: return 5U;
        case SPECIAL_DENSITY6: return 6U;
        case SPECIAL_DENSITY7: return 7U;
        default:
            if (AP_PD != NULL && AP_PD->pd_Preferences.PrintDensity >= 1U &&
                AP_PD->pd_Preferences.PrintDensity <= 7U)
                return AP_PD->pd_Preferences.PrintDensity;
            return 1U;
    }
}

static void ap_set_graphics_density(UWORD special)
{
    static const UWORD dpi_table[8] = {
        100U, 100U, 150U, 200U, 300U, 300U, 600U, 600U
    };
    UWORD dpi = dpi_table[ap_density_index(special)];

    g_frontend_dpi = dpi;
    g_frontend_page_width = ap_udiv_small(
        ap_mul_small(g_page_width, g_frontend_dpi) + (ULONG)(g_output_dpi >> 1),
        g_output_dpi);
    g_frontend_page_height = ap_udiv_small(
        ap_mul_small(g_page_height, g_frontend_dpi) + (ULONG)(g_output_dpi >> 1),
        g_output_dpi);
    if (g_frontend_page_width == 0UL) g_frontend_page_width = 1UL;
    if (g_frontend_page_height == 0UL) g_frontend_page_height = 1UL;

    AP_PEDData.ped_MaxXDots = g_frontend_page_width;
    AP_PEDData.ped_MaxYDots = g_frontend_page_height;
    AP_PEDData.ped_XDotsInch = dpi;
    AP_PEDData.ped_YDotsInch = dpi;
}

static ULONG ap_frontend_to_pwg(ULONG value)
{
    if (g_frontend_dpi == 0U) return value;
    return ap_udiv_small(ap_mul_small(value, g_output_dpi) +
                         (ULONG)(g_frontend_dpi >> 1), g_frontend_dpi);
}

static UWORD ap_job_fit_percent(ULONG width, ULONG height)
{
    ULONG fit = 100UL;
    ULONG value;

    if (width > g_page_width && width != 0UL) {
        value = ap_udiv_small(ap_mul_small(g_page_width, 100U), (UWORD)width);
        if (value < fit) fit = value;
    }
    if (height > g_page_height && height != 0UL) {
        value = ap_udiv_small(ap_mul_small(g_page_height, 100U), (UWORD)height);
        if (value < fit) fit = value;
    }
    if (fit == 0UL) fit = 1UL;
    if (fit > 100UL) fit = 100UL;
    return (UWORD)fit;
}

static void ap_transport_close(void)
{
    if (SysBase == NULL) {
        g_apdev_open = 0;
        g_apdev_io = NULL;
        g_apdev_port = NULL;
        return;
    }

    if (g_apdev_io != NULL) {
        if (g_apdev_open)
            CloseDevice((struct IORequest *)g_apdev_io);
        g_apdev_open = 0;
        DeleteIORequest((struct IORequest *)g_apdev_io);
        g_apdev_io = NULL;
    }
    if (g_apdev_port != NULL) {
        DeleteMsgPort(g_apdev_port);
        g_apdev_port = NULL;
    }
}

static LONG ap_transport_open(void)
{
    LONG error;

    if (SysBase == NULL) return PDERR_INTERNALMEMORY;
    if (g_apdev_open && g_apdev_io != NULL && g_apdev_port != NULL)
        return PDERR_NOERR;

    ap_transport_close();

    g_apdev_port = CreateMsgPort();
    if (g_apdev_port == NULL) return PDERR_INTERNALMEMORY;

    g_apdev_io = (struct IOStdReq *)CreateIORequest(g_apdev_port,
                                                     (LONG)sizeof(struct IOStdReq));
    if (g_apdev_io == NULL) {
        ap_transport_close();
        return PDERR_INTERNALMEMORY;
    }

    error = (LONG)OpenDevice((CONST_STRPTR)APDEV_NAME, 0UL,
                             (struct IORequest *)g_apdev_io, 0UL);
    if (error != 0L) {
        ap_transport_close();
        return PDERR_CANCEL;
    }
    g_apdev_open = 1;

    return PDERR_NOERR;
}

static LONG ap_pwrite(const void *data, ULONG length)
{
    LONG error;

    if (!g_apdev_open || g_apdev_io == NULL ||
        data == NULL || length == 0UL) return PDERR_CANCEL;

    g_apdev_io->io_Command = CMD_WRITE;
    g_apdev_io->io_Flags = 0U;
    g_apdev_io->io_Data = (APTR)data;
    g_apdev_io->io_Length = length;
    g_apdev_io->io_Actual = 0UL;
    g_apdev_io->io_Error = 0;

    error = (LONG)DoIO((struct IORequest *)g_apdev_io);
    if (error != 0L || g_apdev_io->io_Error != 0 ||
        g_apdev_io->io_Actual != length) return PDERR_CANCEL;

    return PDERR_NOERR;
}

static LONG ap_control(UWORD command, UWORD format)
{
    struct APDeviceControl control;
    control.magic = APDEV_CONTROL_MAGIC;
    control.version = APDEV_CONTROL_VERSION;
    control.command = command;
    control.format = format;
    control.reserved = 0U;
    return ap_pwrite(&control, (ULONG)sizeof(control));
}

static int ap_text_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ap_text_starts_with(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL) return 0;
    while (*prefix != '\0') {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static void ap_copy_string(char *dst, ULONG capacity, const char *src)
{
    ULONG i = 0UL;
    if (dst == NULL || capacity == 0UL) return;
    if (src == NULL) src = "";
    while (i + 1UL < capacity && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/*
 * Small unsigned 32/16 divider used by the freestanding printer driver.
 * Keeping this local avoids pulling a libgcc 32-bit division helper into the
 * LoadSeg printer driver.  The quotients used here are all well below 65536,
 * but this routine deliberately returns a full ULONG.
 */
static ULONG ap_udiv_small(ULONG value, UWORD divisor)
{
    ULONG quotient = 0UL;
    ULONG remainder = 0UL;
    int bit;

    if (divisor == 0U) return 0UL;
    for (bit = 31; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((value >> bit) & 1UL);
        if (remainder >= (ULONG)divisor) {
            remainder -= (ULONG)divisor;
            quotient |= (1UL << bit);
        }
    }
    return quotient;
}

/* Parse a positive decimal number and return thousandths of the unit. */
static int ap_parse_decimal_1000(const char **cursor, char stop, ULONG *value)
{
    const char *p;
    ULONG whole = 0UL;
    ULONG fraction = 0UL;
    unsigned int fraction_digits = 0U;
    int saw_digit = 0;

    if (cursor == NULL || *cursor == NULL || value == NULL) return 0;
    p = *cursor;

    while (*p >= '0' && *p <= '9') {
        whole = (whole << 3) + (whole << 1) + (ULONG)(*p - '0');
        ++p;
        saw_digit = 1;
    }
    if (!saw_digit) return 0;

    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (fraction_digits < 3U) {
                fraction = (fraction << 3) + (fraction << 1) +
                           (ULONG)(*p - '0');
            }
            ++fraction_digits;
            ++p;
        }
    }

    if (*p != stop) return 0;
    while (fraction_digits < 3U) {
        fraction = (fraction << 3) + (fraction << 1);
        ++fraction_digits;
    }

    *value = (whole << 10) - (whole << 4) - (whole << 3) + fraction;
    *cursor = p + 1;
    return 1;
}

/*
 * Decode the dimensional tail of a standard PWG media keyword.
 * Examples: iso_a4_210x297mm, na_letter_8.5x11in, na_5x7_5x7in.
 * custom_min/custom_max describe capability bounds, not selectable sheets.
 */
static int ap_media_geometry(const char *keyword, UWORD output_dpi,
                             ULONG *width_pixels,
                             ULONG *height_pixels,
                             ULONG *width_points,
                             ULONG *height_points)
{
    const char *p;
    const char *dims;
    ULONG w1000;
    ULONG h1000;
    ULONG wp;
    ULONG hp;
    ULONG wpt;
    ULONG hpt;
    int unit_mm;

    if (keyword == NULL || output_dpi == 0U ||
        width_pixels == NULL || height_pixels == NULL ||
        width_points == NULL || height_points == NULL) return 0;
    if (ap_text_starts_with(keyword, "custom_min_") ||
        ap_text_starts_with(keyword, "custom_max_")) return 0;

    dims = keyword;
    for (p = keyword; *p != '\0'; ++p) {
        if (*p == '_') dims = p + 1;
    }
    if (dims == keyword || *dims == '\0') return 0;

    p = dims;
    if (!ap_parse_decimal_1000(&p, 'x', &w1000)) return 0;

    {
        const char *q = p;
        while (*q >= '0' && *q <= '9') ++q;
        if (*q == '.') {
            ++q;
            while (*q >= '0' && *q <= '9') ++q;
        }
        if (*q == 'm' && q[1] == 'm' && q[2] == '\0') unit_mm = 1;
        else if (*q == 'i' && q[1] == 'n' && q[2] == '\0') unit_mm = 0;
        else return 0;
    }

    if (!ap_parse_decimal_1000(&p, unit_mm ? 'm' : 'i', &h1000)) return 0;
    if (unit_mm) {
        if (p[0] != 'm' || p[1] != '\0') return 0;
        /* thousandths mm -> pixels at selected dpi: value*dpi/25400. */
        wp = ap_udiv_small(ap_mul_small(w1000, output_dpi) + 12700UL, 25400U);
        hp = ap_udiv_small(ap_mul_small(h1000, output_dpi) + 12700UL, 25400U);
        wpt = ap_udiv_small((w1000 << 5) + (w1000 << 2) + 6350UL, 12700U);
        hpt = ap_udiv_small((h1000 << 5) + (h1000 << 2) + 6350UL, 12700U);
    } else {
        if (p[0] != 'n' || p[1] != '\0') return 0;
        /* thousandths inch -> selected-dpi pixels / PostScript points. */
        wp = ap_udiv_small(ap_mul_small(w1000, output_dpi) + 500UL, 1000U);
        hp = ap_udiv_small(ap_mul_small(h1000, output_dpi) + 500UL, 1000U);
        wpt = ap_udiv_small((w1000 << 3) + w1000 + 62UL, 125U);
        hpt = ap_udiv_small((h1000 << 3) + h1000 + 62UL, 125U);
    }

    if (wp == 0UL || hp == 0UL || wp > 40000UL || hp > 40000UL ||
        wpt == 0UL || hpt == 0UL || wpt > 2400UL || hpt > 2400UL) return 0;

    *width_pixels = wp;
    *height_pixels = hp;
    *width_points = wpt;
    *height_points = hpt;
    return 1;
}

static int ap_parse_resolution(const char *text, UWORD *dpi)
{
    ULONG x = 0UL;
    ULONG y = 0UL;
    ULONG units = 0UL;
    int part = 0;
    int digits = 0;

    if (text == NULL || dpi == NULL) return 0;
    while (*text != '\0') {
        if (*text >= '0' && *text <= '9') {
            ULONG *target = part == 0 ? &x : (part == 1 ? &y : &units);
            *target = (*target << 3) + (*target << 1) + (ULONG)(*text - '0');
            digits = 1;
        } else if (*text == ',' && part < 2 && digits) {
            ++part;
            digits = 0;
        } else {
            return 0;
        }
        ++text;
    }
    if (part != 2 || !digits || units != 3UL || x != y ||
        x < 72UL || x > 2400UL) return 0;
    *dpi = (UWORD)x;
    return 1;
}

static void ap_parse_prefs_line(char *line, int *landscape, int *monochrome,
                                int *sgray_supported, UWORD *scale_percent,
                                int *center_on_paper, UWORD *engine,
                                UWORD *duplex_mode, UWORD *sheet_back,
                                UWORD *output_dpi,
                                char *media, ULONG media_capacity)
{
    static const char orientation_key[] = "ORIENTATION=";
    static const char media_key[] = "MEDIA=";
    static const char color_key[] = "COLOR=";
    static const char scale_key[] = "SCALE=";
    static const char center_key[] = "CENTER_ON_PAPER=";
    static const char sgray_key[] = "CAP_PWG_SGRAY8_SUPPORTED=";
    static const char engine_key[] = "ENGINE=";
    static const char sides_key[] = "SIDES=";
    static const char sheet_back_key[] = "CAP_PWG_SHEET_BACK=";
    static const char resolution_key[] = "RESOLUTION=";
    ULONG i;

    if (line == NULL || landscape == NULL || monochrome == NULL ||
        sgray_supported == NULL || scale_percent == NULL ||
        center_on_paper == NULL || engine == NULL ||
        duplex_mode == NULL || sheet_back == NULL || output_dpi == NULL ||
        media == NULL) return;

    for (i = 0UL; orientation_key[i] != '\0'; ++i) if (line[i] != orientation_key[i]) break;
    if (orientation_key[i] == '\0') {
        *landscape = ap_text_equals(line + i, "landscape") ? 1 : 0;
        return;
    }
    for (i = 0UL; color_key[i] != '\0'; ++i) if (line[i] != color_key[i]) break;
    if (color_key[i] == '\0') {
        *monochrome = (ap_text_equals(line + i, "monochrome") ||
                       ap_text_equals(line + i, "auto-monochrome")) ? 1 : 0;
        return;
    }
    for (i = 0UL; scale_key[i] != '\0'; ++i) if (line[i] != scale_key[i]) break;
    if (scale_key[i] == '\0') {
        const char *p = line + i;
        ULONG value = 0UL;
        int have_digit = 0;
        while (*p >= '0' && *p <= '9') {
            value = (value << 3) + (value << 1) + (ULONG)(*p - '0');
            have_digit = 1;
            ++p;
        }
        if (have_digit && *p == '\0' && value >= 10UL && value <= 100UL)
            *scale_percent = (UWORD)value;
        return;
    }
    for (i = 0UL; center_key[i] != '\0'; ++i) if (line[i] != center_key[i]) break;
    if (center_key[i] == '\0') {
        *center_on_paper = ap_text_equals(line + i, "1") ? 1 : 0;
        return;
    }
    for (i = 0UL; sgray_key[i] != '\0'; ++i) if (line[i] != sgray_key[i]) break;
    if (sgray_key[i] == '\0') {
        *sgray_supported = ap_text_equals(line + i, "1") ? 1 : 0;
        return;
    }
    for (i = 0UL; engine_key[i] != '\0'; ++i) if (line[i] != engine_key[i]) break;
    if (engine_key[i] == '\0') {
        if (ap_text_equals(line + i, "pdf")) *engine = AP_ENGINE_PDF;
        else if (ap_text_equals(line + i, "postscript")) *engine = AP_ENGINE_POSTSCRIPT;
        else *engine = AP_ENGINE_PWG;
        return;
    }
    for (i = 0UL; sides_key[i] != '\0'; ++i) if (line[i] != sides_key[i]) break;
    if (sides_key[i] == '\0') {
        if (ap_text_equals(line + i, "two-sided-long-edge"))
            *duplex_mode = AP_DUPLEX_LONG_EDGE;
        else if (ap_text_equals(line + i, "two-sided-short-edge"))
            *duplex_mode = AP_DUPLEX_SHORT_EDGE;
        else
            *duplex_mode = AP_DUPLEX_ONE_SIDED;
        return;
    }
    for (i = 0UL; sheet_back_key[i] != '\0'; ++i)
        if (line[i] != sheet_back_key[i]) break;
    if (sheet_back_key[i] == '\0') {
        if (ap_text_equals(line + i, "flipped"))
            *sheet_back = AP_SHEET_BACK_FLIPPED;
        else if (ap_text_equals(line + i, "rotated"))
            *sheet_back = AP_SHEET_BACK_ROTATED;
        else if (ap_text_equals(line + i, "manual-tumble"))
            *sheet_back = AP_SHEET_BACK_MANUAL_TUMBLE;
        else
            *sheet_back = AP_SHEET_BACK_NORMAL;
        return;
    }
    for (i = 0UL; resolution_key[i] != '\0'; ++i) if (line[i] != resolution_key[i]) break;
    if (resolution_key[i] == '\0') {
        UWORD dpi;
        if (ap_parse_resolution(line + i, &dpi)) *output_dpi = dpi;
        return;
    }
    for (i = 0UL; media_key[i] != '\0'; ++i) if (line[i] != media_key[i]) return;
    if (line[i] != '\0' &&
        !ap_text_starts_with(line + i, "custom_min_") &&
        !ap_text_starts_with(line + i, "custom_max_"))
        ap_copy_string(media, media_capacity, line + i);
}

static void ap_scan_prefs_file(CONST_STRPTR path, int *landscape, int *monochrome,
                               int *sgray_supported, UWORD *scale_percent,
                               int *center_on_paper, UWORD *engine,
                               UWORD *duplex_mode, UWORD *sheet_back,
                               UWORD *output_dpi,
                               char *media, ULONG media_capacity)
{
    BPTR file;
    LONG got;
    ULONG line_len;

    if (path == NULL || landscape == NULL || monochrome == NULL ||
        sgray_supported == NULL || scale_percent == NULL ||
        center_on_paper == NULL || engine == NULL ||
        duplex_mode == NULL || sheet_back == NULL || output_dpi == NULL ||
        media == NULL || DOSBase == NULL) return;
    file = Open(path, MODE_OLDFILE);
    if (file == 0) return;

    line_len = 0UL;
    for (;;) {
        LONG i;
        got = Read(file, g_prefs_input, (LONG)sizeof(g_prefs_input));
        if (got <= 0) break;
        for (i = 0; i < got; ++i) {
            UBYTE c = g_prefs_input[i];
            if (c == '\r') continue;
            if (c == '\n') {
                g_prefs_line[line_len] = '\0';
                ap_parse_prefs_line(g_prefs_line, landscape, monochrome,
                                    sgray_supported, scale_percent, center_on_paper,
                                    engine, duplex_mode, sheet_back, output_dpi,
                                    media, media_capacity);
                line_len = 0UL;
            } else if (line_len + 1UL < (ULONG)sizeof(g_prefs_line)) {
                g_prefs_line[line_len++] = (char)c;
            }
        }
    }
    if (line_len != 0UL) {
        g_prefs_line[line_len] = '\0';
        ap_parse_prefs_line(g_prefs_line, landscape, monochrome,
                            sgray_supported, scale_percent, center_on_paper,
                            engine, duplex_mode, sheet_back, output_dpi,
                            media, media_capacity);
    }
    Close(file);
}

static void ap_load_page_prefs(int *landscape, int *monochrome, int *sgray_supported,
                               UWORD *scale_percent, int *center_on_paper,
                               UWORD *engine, UWORD *duplex_mode,
                               UWORD *sheet_back, UWORD *output_dpi,
                               char *media, ULONG media_capacity)
{
    if (landscape == NULL || monochrome == NULL || sgray_supported == NULL ||
        scale_percent == NULL || center_on_paper == NULL ||
        engine == NULL || duplex_mode == NULL || sheet_back == NULL || output_dpi == NULL ||
        media == NULL || media_capacity == 0UL) return;

    *landscape = 0;
    *monochrome = 0;
    *sgray_supported = 0;
    *scale_percent = 100U;
    *center_on_paper = 0;
    *engine = AP_ENGINE_PWG;
    *duplex_mode = AP_DUPLEX_ONE_SIDED;
    *sheet_back = AP_SHEET_BACK_NORMAL;
    *output_dpi = 600U;
    ap_copy_string(media, media_capacity, AP_DEFAULT_MEDIA);

    if (SysBase == NULL) return;
    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 39L);
    if (DOSBase == NULL) return;

    ap_scan_prefs_file((CONST_STRPTR)"ENVARC:AirPrint.prefs",
                       landscape, monochrome, sgray_supported, scale_percent,
                       center_on_paper, engine, duplex_mode, sheet_back, output_dpi,
                       media, media_capacity);
    ap_scan_prefs_file((CONST_STRPTR)"ENV:AirPrint.prefs",
                       landscape, monochrome, sgray_supported, scale_percent,
                       center_on_paper, engine, duplex_mode, sheet_back, output_dpi,
                       media, media_capacity);

    CloseLibrary((struct Library *)DOSBase);
    DOSBase = NULL;
}

static void ap_apply_page_prefs(void)
{
    char media[AP_MEDIA_KEY_LEN];
    ULONG width_pixels;
    ULONG height_pixels;
    ULONG width_points;
    ULONG height_points;

    ap_load_page_prefs(&g_landscape, &g_monochrome, &g_sgray_supported,
                       &g_scale_percent, &g_center_on_paper,
                       &g_engine, &g_duplex_mode, &g_sheet_back, &g_output_dpi,
                       media, (ULONG)sizeof(media));
    if (g_engine != AP_ENGINE_PWG) g_duplex_mode = AP_DUPLEX_ONE_SIDED;
    g_duplex_requested = g_duplex_mode != AP_DUPLEX_ONE_SIDED;

    if (!ap_media_geometry(media, g_output_dpi,
                           &width_pixels, &height_pixels,
                           &width_points, &height_points)) {
        ap_copy_string(media, (ULONG)sizeof(media), AP_DEFAULT_MEDIA);
        /* Static fallback geometry is defined at 600 dpi. */
        width_pixels = ap_udiv_small(ap_mul_small(AP_DEFAULT_WIDTH_PIXELS, g_output_dpi), 600U);
        height_pixels = ap_udiv_small(ap_mul_small(AP_DEFAULT_HEIGHT_PIXELS, g_output_dpi), 600U);
        width_points = AP_DEFAULT_WIDTH_POINTS;
        height_points = AP_DEFAULT_HEIGHT_POINTS;
    }

    ap_copy_string(g_media_keyword, (ULONG)sizeof(g_media_keyword), media);
    g_page_width = width_pixels;
    g_page_height = height_pixels;
    g_page_width_points = width_points;
    g_page_height_points = height_points;
    if (g_monochrome && g_sgray_supported) {
        g_bytes_per_pixel = 1UL;
        g_color_space = AP_PWG_COLORSPACE_SW;
        g_num_colors = 1UL;
    } else {
        g_bytes_per_pixel = 3UL;
        g_color_space = AP_PWG_COLORSPACE_SRGB;
        g_num_colors = 3UL;
    }
    g_row_bytes = g_bytes_per_pixel == 1UL ? g_page_width :
                  (g_page_width << 1) + g_page_width;
    g_encoded_max = 1UL + g_row_bytes + ((g_page_width + 127UL) >> 7);
    {
        ULONG document_scratch = ap_document_scratch_size(g_row_bytes);
        if (document_scratch > g_encoded_max) g_encoded_max = document_scratch;
    }

    if (AP_PD != NULL) {
        AP_PD->pd_Preferences.PrintAspect =
            g_landscape ? ASPECT_VERT : ASPECT_HORIZ;
        AP_PD->pd_Preferences.PrintShade =
            g_monochrome ? SHADE_GREYSCALE : SHADE_COLOR;
    }

    /* Text metadata follows the selected classic Printer preferences while
     * graphics geometry remains the full selected PWG media raster. */
    ap_text_update_geometry();
    AP_PEDData.ped_NumCharSets = 1U;
    g_frontend_dpi = g_output_dpi;
    g_frontend_page_width = g_page_width;
    g_frontend_page_height = g_page_height;
    AP_PEDData.ped_MaxXDots = g_frontend_page_width;
    AP_PEDData.ped_MaxYDots = g_frontend_page_height;
    AP_PEDData.ped_XDotsInch = g_frontend_dpi;
    AP_PEDData.ped_YDotsInch = g_frontend_dpi;
}

/*
 * AmiAirPrint owns its network transport and never uses printer.device's
 * primitive serial/parallel writer for payload data.  Some legacy programs
 * still issue setup/raw writes before PRD_DUMPRPORT.  Sending those bytes to
 * an unconnected compatibility port can block until printer.device displays
 * its timeout/paper requester, so consume them successfully while this driver
 * is open.  printer.device is exclusive; the original callbacks are restored
 * at Close.
 */
static LONG ap_primitive_write_sink(APTR data, LONG length)
{
    (void)data;
    (void)length;
    return PDERR_NOERR;
}

static LONG ap_primitive_bothready_sink(void)
{
    return PDERR_NOERR;
}

static void ap_install_primitive_io_sinks(void)
{
    if (AP_PD == NULL || g_primitive_io_hooked) return;

    g_original_pwrite = AP_PD->pd_PWrite;
    g_original_pbothready = AP_PD->pd_PBothReady;
    AP_PD->pd_PWrite = ap_primitive_write_sink;
    AP_PD->pd_PBothReady = ap_primitive_bothready_sink;
    g_primitive_io_hooked = 1;
}

static void ap_restore_primitive_io(void)
{
    if (AP_PD != NULL && g_primitive_io_hooked) {
        AP_PD->pd_PWrite = g_original_pwrite;
        AP_PD->pd_PBothReady = g_original_pbothready;
    }
    g_original_pwrite = NULL;
    g_original_pbothready = NULL;
    g_primitive_io_hooked = 0;
}

/*
 * Called from the assembly AP_Open callback.  Dynamic page geometry must be
 * available as soon as OpenDevice("printer.device") returns: applications
 * (and our diagnostic STRETCH mode) are allowed to inspect ped_MaxXDots/
 * ped_MaxYDots before submitting PRD_DUMPRPORT.  In v0.9.13 these values were
 * only refreshed in PRS_PREINIT, which was too late for callers that had
 * already copied the static A4 fallback into io_DestCols/io_DestRows.
 */
LONG AP_OpenSetup(void)
{
    LONG err;

    g_job_scale_percent = g_scale_percent;
    err = ap_transport_open();
    if (err != PDERR_NOERR) return err;

    ap_install_primitive_io_sinks();
    g_duplex_document_open = 0;
    g_graphics_continuation_open = 0;
    g_pwg_page_number = 0UL;
    ap_apply_page_prefs();
    ap_text_reset_state();
    AP_PEDData.ped_PrintMode = 0L;
    return PDERR_NOERR;
}

static void ap_release_buffer(void)
{
    if (SysBase != NULL) {
        if (g_encoded_row != NULL && g_encoded_alloc_size != 0UL) {
            FreeMem(g_encoded_row, g_encoded_alloc_size);
        }
        if (AP_PD != NULL && AP_PD->pd_PrintBuf != NULL &&
            g_raw_row == AP_PD->pd_PrintBuf && g_raw_alloc_size != 0UL) {
            FreeMem(AP_PD->pd_PrintBuf, g_raw_alloc_size);
            AP_PD->pd_PrintBuf = NULL;
        }
    }
    g_raw_row = NULL;
    g_encoded_row = NULL;
    g_raw_alloc_size = 0UL;
    g_encoded_alloc_size = 0UL;
    g_emit_current_rows = 0U;
    g_raster_started = 0;
}

void AP_CloseSetup(void)
{
    LONG close_err = PDERR_NOERR;

    /* Page-oriented text printers eject an outstanding text page at Close. */
    (void)ap_text_finish_page(0);

    /* A final SPECIAL_NOFORMFEED dump can legitimately remain open until the
     * application closes printer.device.  Complete that physical page instead
     * of aborting it. */
    if (g_graphics_continuation_open && !g_raster_started && g_apdev_open) {
        close_err = ap_start_raster();
        if (close_err == PDERR_NOERR && g_rows_sent < g_page_height)
            close_err = ap_send_blank_rows(g_page_height - g_rows_sent);
        if (close_err == PDERR_NOERR) close_err = ap_finish_current_page();
        g_graphics_continuation_open = 0;
        ap_release_buffer();
    }

    if (close_err != PDERR_NOERR && g_apdev_open) {
        ap_abort_active_job();
    } else if (g_raster_started && g_apdev_open) {
        ap_abort_active_job();
    } else if (g_duplex_document_open && g_apdev_open) {
        if (ap_control(APDEV_CTL_END, APDEV_FORMAT_PWG_RASTER) != PDERR_NOERR)
            ap_abort_active_job();
        g_duplex_document_open = 0;
    }

    ap_release_buffer();
    ap_text_reset_state();

    /* Leave the shared PED and primitive I/O callbacks exactly as they were
     * before this driver instance was opened. */
    if (g_page_width != 0UL && g_page_height != 0UL) {
        g_frontend_dpi = g_output_dpi;
        g_frontend_page_width = g_page_width;
        g_frontend_page_height = g_page_height;
        AP_PEDData.ped_MaxXDots = g_frontend_page_width;
        AP_PEDData.ped_MaxYDots = g_frontend_page_height;
        AP_PEDData.ped_XDotsInch = g_frontend_dpi;
        AP_PEDData.ped_YDotsInch = g_frontend_dpi;
    }
    g_job_scale_percent = g_scale_percent;
    ap_restore_primitive_io();
    ap_transport_close();
}

static void ap_build_pwg_prefix(void)
{
    UBYTE *h;

    ap_memzero(g_pwg_prefix, AP_PWG_PREFIX_SIZE);

    /* PWG Raster v2 synchronization word, always network byte order. */
    g_pwg_prefix[0] = 0x52U;
    g_pwg_prefix[1] = 0x61U;
    g_pwg_prefix[2] = 0x53U;
    g_pwg_prefix[3] = 0x32U;

    h = g_pwg_prefix + 4UL;
    ap_copy_text(h + 0UL, 64UL, "PwgRaster");
    ap_copy_text(h + 128UL, 64UL, "stationery");

    ap_put_be32(h + 276UL, (ULONG)g_output_dpi); /* HWResolution X */
    ap_put_be32(h + 280UL, (ULONG)g_output_dpi); /* HWResolution Y */
    ap_put_be32(h + 272UL, g_duplex_requested ? 1UL : 0UL); /* Duplex */
    ap_put_be32(h + 340UL, 1UL);   /* NumCopies */

    /* Canonical selected-media raster. Landscape is source-rotated by printer.device. */
    ap_put_be32(h + 344UL, 0UL);   /* Orientation: bitmap already rasterized */
    ap_put_be32(h + 352UL, g_page_width_points);
    ap_put_be32(h + 356UL, g_page_height_points);
    ap_put_be32(h + 368UL, g_duplex_mode == AP_DUPLEX_SHORT_EDGE ? 1UL : 0UL); /* Tumble */
    ap_put_be32(h + 372UL, g_page_width);
    ap_put_be32(h + 376UL, g_page_height);
    ap_put_be32(h + 384UL, 8UL);                 /* BitsPerColor */
    ap_put_be32(h + 388UL, g_bytes_per_pixel == 1UL ? 8UL : 24UL); /* BitsPerPixel */
    ap_put_be32(h + 392UL, g_row_bytes);         /* BytesPerLine */
    ap_put_be32(h + 396UL, AP_PWG_COLOR_ORDER_CHUNKED);
    ap_put_be32(h + 400UL, g_color_space);
    ap_put_be32(h + 420UL, g_num_colors);        /* NumColors */
    ap_put_be32(h + 452UL, g_duplex_requested ? 0UL : 1UL); /* unknown for duplex */
    {
        ULONG cross_feed = 1UL;
        ULONG feed = 1UL;
        int back_page = g_duplex_requested && (g_pwg_page_number & 1UL) == 0UL;
        if (back_page) {
            if (g_duplex_mode == AP_DUPLEX_LONG_EDGE) {
                if (g_sheet_back == AP_SHEET_BACK_FLIPPED) feed = ~0UL;
                else if (g_sheet_back == AP_SHEET_BACK_ROTATED) {
                    cross_feed = ~0UL; feed = ~0UL;
                }
            } else if (g_duplex_mode == AP_DUPLEX_SHORT_EDGE) {
                if (g_sheet_back == AP_SHEET_BACK_FLIPPED) cross_feed = ~0UL;
                else if (g_sheet_back == AP_SHEET_BACK_MANUAL_TUMBLE) {
                    cross_feed = ~0UL; feed = ~0UL;
                }
                /* Normal and Rotated both use +1/+1 for short-edge. */
            }
        }
        ap_put_be32(h + 456UL, cross_feed); /* CrossFeedTransform */
        ap_put_be32(h + 460UL, feed);       /* FeedTransform */
    }
    ap_put_be32(h + 464UL, 0UL);                 /* ImageBoxLeft */
    ap_put_be32(h + 468UL, 0UL);                 /* ImageBoxTop */
    ap_put_be32(h + 472UL, g_page_width);   /* ImageBoxRight */
    ap_put_be32(h + 476UL, g_page_height);  /* ImageBoxBottom */
    ap_put_be32(h + 484UL, 0UL);                 /* use IPP/default quality */
    ap_copy_text(h + 1732UL, 64UL, g_media_keyword);
}

/*
 * Encode one PWG row (sRGB or sGray) using the CUPS/PWG v2 PackBits-like format.
 * repetition_minus_one is the number of identical raster lines minus one.
 */
static ULONG ap_encode_row(const UBYTE *row, UBYTE repetition_minus_one)
{
    ULONG pos = 0UL;
    ULONG out = 0UL;
    const UBYTE *pixel = row;

    g_encoded_row[out++] = repetition_minus_one;

    while (pos < g_page_width) {
        ULONG run = 1UL;
        const UBYTE *probe = ap_next_const_pixel(pixel);

        while (run < 128UL && pos + run < g_page_width &&
               ap_pixel_equal(pixel, probe)) {
            ++run;
            probe = ap_next_const_pixel(probe);
        }

        if (run >= 2UL) {
            g_encoded_row[out++] = (UBYTE)(run - 1UL);
            if (g_bytes_per_pixel == 1UL) {
                g_encoded_row[out++] = pixel[0];
            } else {
                g_encoded_row[out++] = pixel[0];
                g_encoded_row[out++] = pixel[1];
                g_encoded_row[out++] = pixel[2];
            }
            pos += run;
            pixel = probe;
        } else {
            ULONG count = 1UL;
            const UBYTE *scan = ap_next_const_pixel(pixel);
            const UBYTE *next = ap_next_const_pixel(scan);

            while (pos + count < g_page_width && count < 128UL) {
                if (pos + count + 1UL < g_page_width &&
                    ap_pixel_equal(scan, next)) {
                    break;
                }
                ++count;
                scan = next;
                next = ap_next_const_pixel(next);
            }

            g_encoded_row[out++] = count == 1UL ? 0U : (UBYTE)(257UL - count);
            while (count-- != 0UL) {
                if (g_bytes_per_pixel == 1UL) {
                    g_encoded_row[out++] = pixel[0];
                } else {
                    g_encoded_row[out++] = pixel[0];
                    g_encoded_row[out++] = pixel[1];
                    g_encoded_row[out++] = pixel[2];
                }
                pixel = ap_next_const_pixel(pixel);
                ++pos;
            }
        }
    }

    return out;
}

static UWORD ap_engine_device_format(void)
{
    if (g_engine == AP_ENGINE_PDF) return APDEV_FORMAT_PDF;
    if (g_engine == AP_ENGINE_POSTSCRIPT) return APDEV_FORMAT_POSTSCRIPT;
    return APDEV_FORMAT_PWG_RASTER;
}

static void ap_abort_active_job(void)
{
    if (g_apdev_open) (void)ap_control(APDEV_CTL_ABORT, ap_engine_device_format());
    g_duplex_document_open = 0;
    g_graphics_continuation_open = 0;
}

static LONG ap_write_row_repeat(const UBYTE *row, ULONG repeat)
{
    LONG err = PDERR_NOERR;

    if (row == NULL || repeat == 0UL) return PDERR_NOERR;
    if (g_engine == AP_ENGINE_PWG) {
        while (repeat != 0UL) {
            ULONG group = repeat > 256UL ? 256UL : repeat;
            ULONG encoded = ap_encode_row(row, (UBYTE)(group - 1UL));
            err = ap_pwrite(g_encoded_row, encoded);
            if (err != PDERR_NOERR) return err;
            g_rows_sent += group;
            repeat -= group;
        }
        return PDERR_NOERR;
    }

    while (repeat-- != 0UL) {
        err = ap_document_write_row(&g_document_writer, row,
                                    g_encoded_row, g_encoded_alloc_size);
        if (err != PDERR_NOERR) return err;
        ++g_rows_sent;
    }
    return PDERR_NOERR;
}

static LONG ap_finish_current_page(void)
{
    LONG err;

    if (g_engine == AP_ENGINE_PDF || g_engine == AP_ENGINE_POSTSCRIPT) {
        err = ap_document_end(&g_document_writer);
        if (err != PDERR_NOERR) {
            ap_abort_active_job();
            return err;
        }
    }

    if (g_engine == AP_ENGINE_PWG && g_duplex_requested)
        return PDERR_NOERR;

    err = ap_control(APDEV_CTL_END, ap_engine_device_format());
    if (err != PDERR_NOERR) ap_abort_active_job();
    return err;
}

static LONG ap_send_blank_rows(ULONG count)
{
    if (count == 0UL) return PDERR_NOERR;
    if (g_raw_row == NULL || g_encoded_row == NULL) return PDERR_BUFFERMEMORY;
    ap_memwhite(g_raw_row, g_row_bytes);
    return ap_write_row_repeat(g_raw_row, count);
}

static LONG ap_start_raster(void)
{
    LONG err;

    if (AP_PD == NULL) return PDERR_CANCEL;

    /*
     * SysBase is initialized by AP_Init() from the absolute ExecBase pointer
     * at address 4, matching the classic Commodore printer-driver ABI.
     * Treat a missing base as an internal initialization failure rather than
     * pretending that a print-buffer allocation failed.
     */
    if (SysBase == NULL) return PDERR_INTERNALMEMORY;

    if (g_page_width == 0UL || g_page_height == 0UL || g_row_bytes == 0UL)
        ap_apply_page_prefs();

    ap_release_buffer();

    AP_PD->pd_PrintBuf = (UBYTE *)AllocMem(g_row_bytes,
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (AP_PD->pd_PrintBuf == NULL) return PDERR_BUFFERMEMORY;
    g_raw_row = AP_PD->pd_PrintBuf;
    g_raw_alloc_size = g_row_bytes;

    g_encoded_row = (UBYTE *)AllocMem(g_encoded_max,
                                      MEMF_PUBLIC | MEMF_CLEAR);
    if (g_encoded_row == NULL) {
        ap_release_buffer();
        return PDERR_BUFFERMEMORY;
    }
    g_encoded_alloc_size = g_encoded_max;

    /* SPECIAL_NOFORMFEED keeps the physical page/document open between
     * PRD_DUMPRPORT calls (FinalWriter uses this for strip printing).  The
     * row buffers are per dump and were just reallocated, but the IPP/PWG or
     * PDF/PostScript stream and g_rows_sent must continue unchanged. */
    if (g_graphics_continuation_open) {
        g_graphics_continuation_open = 0;
        g_raster_started = 1;
        return PDERR_NOERR;
    }

    g_rows_sent = 0UL;
    g_raster_started = 0;

    if (g_engine == AP_ENGINE_PWG) {
        ++g_pwg_page_number;
        ap_build_pwg_prefix();
        if (!g_duplex_document_open) {
            err = ap_control(APDEV_CTL_BEGIN, APDEV_FORMAT_PWG_RASTER);
            if (err != PDERR_NOERR) {
                ap_release_buffer();
                return err;
            }
            if (g_duplex_requested) g_duplex_document_open = 1;
            err = ap_pwrite(g_pwg_prefix, AP_PWG_PREFIX_SIZE);
        } else {
            /* A multi-page PWG file has one RaS2 sync word, then one page
             * header per page. */
            err = ap_pwrite(g_pwg_prefix + 4UL, AP_PWG_HEADER_SIZE);
        }
    } else {
        UWORD device_format = ap_engine_device_format();
        UWORD doc_format = g_engine == AP_ENGINE_PDF
            ? AP_DOCUMENT_PDF : AP_DOCUMENT_POSTSCRIPT;
        err = ap_control(APDEV_CTL_BEGIN, device_format);
        if (err == PDERR_NOERR) {
            err = ap_document_begin(&g_document_writer, doc_format,
                                    (UWORD)g_num_colors,
                                    g_page_width, g_page_height,
                                    g_page_width_points, g_page_height_points,
                                    ap_pwrite);
        }
    }

    if (err != PDERR_NOERR) {
        ap_abort_active_job();
        ap_release_buffer();
        return err;
    }

    g_raster_started = 1;
    return PDERR_NOERR;
}


/* -------------------------------------------------------------------------
 * Native alphanumeric text path
 * -------------------------------------------------------------------------
 *
 * printer.device calls ped_ConvFunc for processed CMD_WRITE/PRT: characters.
 * Returning zero consumes the character instead of forwarding it to the
 * selected primitive port.  AmiAirPrint keeps classic text semantics locally
 * and converts each completed line to the same 600-dpi PWG Raster format used
 * by the graphics path.
 *
 * This is intentionally page oriented.  A form feed closes the current page;
 * ped_Close closes a final outstanding page, mirroring Commodore's LaserJet
 * driver model.  Multi-page text therefore remains correct even though each
 * physical PWG page is submitted as its own IPP print job.
 */

static void ap_text_clear_line(void)
{
    UWORD i;
    for (i = 0U; i < AP_TEXT_MAX_COLUMNS; ++i) {
        g_text_cells[i] = (UBYTE)' ';
        g_text_styles[i] = 0U;
    }
    g_text_line_has_data = 0;
    g_text_column = g_text_left_margin;
}

static void ap_text_update_geometry(void)
{
    UWORD cell_width;
    UWORD line_height;
    UWORD left_edge;
    UWORD top_edge;
    UWORD bottom_edge;
    ULONG usable_width;
    ULONG usable_height;
    ULONG physical_columns;
    ULONG physical_lines;
    ULONG requested_lines;
    UWORD left = 1U;
    UWORD right;

    cell_width = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 5UL, 10U);
    line_height = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 3UL, 6U);
    if (AP_PD != NULL) {
        if (AP_PD->pd_Preferences.PrintPitch == ELITE)
            cell_width = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 6UL, 12U);
        else if (AP_PD->pd_Preferences.PrintPitch == FINE)
            cell_width = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 8UL, 17U);
        if (AP_PD->pd_Preferences.PrintSpacing == EIGHT_LPI)
            line_height = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 4UL, 8U);
    }
    if (cell_width == 0U) cell_width = 1U;
    if (line_height == 0U) line_height = 1U;

    left_edge = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 5UL, 10U);
    top_edge = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 3UL, 6U);
    bottom_edge = top_edge;

    usable_width = g_page_width > (ULONG)left_edge
        ? g_page_width - (ULONG)left_edge : 1UL;
    physical_columns = ap_udiv_small(usable_width, cell_width);
    if (physical_columns == 0UL) physical_columns = 1UL;
    if (physical_columns > (ULONG)AP_TEXT_MAX_COLUMNS)
        physical_columns = (ULONG)AP_TEXT_MAX_COLUMNS;

    right = (UWORD)physical_columns;
    if (AP_PD != NULL) {
        ULONG pref_left = (ULONG)AP_PD->pd_Preferences.PrintLeftMargin;
        ULONG pref_right = (ULONG)AP_PD->pd_Preferences.PrintRightMargin;
        if (pref_left >= 1UL && pref_left <= physical_columns) left = (UWORD)pref_left;
        if (pref_right >= (ULONG)left && pref_right <= physical_columns) right = (UWORD)pref_right;
    }
    if (right < left) right = left;

    usable_height = g_page_height;
    if (usable_height > (ULONG)top_edge + (ULONG)bottom_edge)
        usable_height -= (ULONG)top_edge + (ULONG)bottom_edge;
    physical_lines = ap_udiv_small(usable_height, line_height);
    if (physical_lines == 0UL) physical_lines = 1UL;
    if (physical_lines > 999UL) physical_lines = 999UL;

    requested_lines = physical_lines;
    if (AP_PD != NULL && AP_PD->pd_Preferences.PaperLength != 0U &&
        (ULONG)AP_PD->pd_Preferences.PaperLength < requested_lines)
        requested_lines = (ULONG)AP_PD->pd_Preferences.PaperLength;
    if (requested_lines == 0UL) requested_lines = 1UL;

    g_text_max_columns = (UWORD)physical_columns;
    g_text_left_margin = left;
    g_text_right_margin = right;
    g_text_cell_width = cell_width;
    g_text_line_height = line_height;
    if (AP_PD != NULL && AP_PD->pd_Preferences.PrintPitch == ELITE)
        g_text_glyph_scale_x = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 60UL, 120U);
    else if (AP_PD != NULL && AP_PD->pd_Preferences.PrintPitch == FINE)
        g_text_glyph_scale_x = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 75UL, 150U);
    else
        g_text_glyph_scale_x = (UWORD)ap_udiv_small((ULONG)g_output_dpi + 50UL, 100U);
    if (g_text_glyph_scale_x == 0U) g_text_glyph_scale_x = 1U;
    g_text_glyph_scale_y = AP_PD != NULL && AP_PD->pd_Preferences.PrintSpacing == EIGHT_LPI
        ? (UWORD)ap_udiv_small((ULONG)g_output_dpi + 40UL, 80U)
        : (UWORD)ap_udiv_small((ULONG)g_output_dpi + 30UL, 60U);
    if (g_text_glyph_scale_y == 0U) g_text_glyph_scale_y = 1U;
    g_text_lines_per_page = (UWORD)requested_lines;
    AP_PEDData.ped_MaxColumns = (UBYTE)g_text_max_columns;
}

static void ap_text_reset_state(void)
{
    ap_text_update_geometry();
    g_text_line_on_page = 0U;
    g_text_style_flags = 0U;
    g_text_page_touched = 0;
    g_text_error = PDERR_NOERR;
    ap_text_clear_line();
}

static void ap_text_set_black(ULONG x, ULONG width)
{
    ULONG end;
    UBYTE *p;

    if (g_raw_row == NULL || x >= g_page_width || width == 0UL) return;
    end = x + width;
    if (end < x || end > g_page_width) end = g_page_width;

    if (g_bytes_per_pixel == 1UL) {
        p = g_raw_row + x;
        while (x++ < end) *p++ = 0U;
    } else {
        p = g_raw_row + (x << 1) + x;
        while (x++ < end) {
            p[0] = 0U;
            p[1] = 0U;
            p[2] = 0U;
            p += 3;
        }
    }
}

static LONG ap_text_send_row(ULONG repeat)
{
    if (repeat == 0UL) return PDERR_NOERR;
    return ap_write_row_repeat(g_raw_row, repeat);
}

static void ap_text_build_glyph_row(UWORD font_row)
{
    UWORD column;

    ap_memwhite(g_raw_row, g_row_bytes);

    for (column = g_text_left_margin;
         column <= g_text_right_margin && column <= g_text_max_columns;
         ++column) {
        UBYTE c = g_text_cells[column - 1U];
        UBYTE style = g_text_styles[column - 1U];
        const UBYTE *glyph;
        UBYTE bits;
        ULONG cell_x;
        ULONG glyph_width;
        ULONG glyph_x;
        ULONG italic_shift = 0UL;
        ULONG bold_extra = 0UL;
        UWORD bit;

        if (c == (UBYTE)' ' && (style & AP_TEXT_STYLE_UNDERLINE) == 0U)
            continue;

        glyph = ap_font8x8_glyph(c);
        bits = glyph[font_row & 7U];
        glyph_width = (ULONG)g_text_glyph_scale_x << 3;
        cell_x = ap_udiv_small((ULONG)g_output_dpi + 5UL, 10U) +
                 ap_mul_small((ULONG)(column - 1U), g_text_cell_width);
        glyph_x = cell_x;
        if ((ULONG)g_text_cell_width > glyph_width)
            glyph_x += ((ULONG)g_text_cell_width - glyph_width) >> 1;

        if ((style & AP_TEXT_STYLE_ITALIC) != 0U) {
            italic_shift = ap_mul_small((ULONG)(7U - (font_row & 7U)),
                                        g_text_glyph_scale_x) >> 3;
        }
        if ((style & AP_TEXT_STYLE_BOLD) != 0U)
            bold_extra = g_text_glyph_scale_x >= 4U ? 2UL : 1UL;

        for (bit = 0U; bit < 8U; ++bit) {
            if ((bits & (UBYTE)(1U << bit)) != 0U) {
                ULONG px = glyph_x + italic_shift +
                           ap_mul_small((ULONG)bit, g_text_glyph_scale_x);
                ap_text_set_black(px,
                                  (ULONG)g_text_glyph_scale_x + bold_extra);
            }
        }

        if ((style & AP_TEXT_STYLE_UNDERLINE) != 0U && font_row == 7U)
            ap_text_set_black(glyph_x, glyph_width);
    }
}

static LONG ap_text_ensure_page(void)
{
    LONG err;
    ULONG top;
    int continuing_graphics = g_graphics_continuation_open;

    if (g_raster_started) return PDERR_NOERR;

    err = ap_start_raster();
    if (err != PDERR_NOERR) {
        g_text_error = err;
        return err;
    }

    /* When text follows a SPECIAL_NOFORMFEED graphics strip, continue at the
     * current raster position rather than inserting a second top margin. */
    top = continuing_graphics ? 0UL :
          ap_udiv_small((ULONG)g_output_dpi + 3UL, 6U);
    if (top > g_page_height) top = g_page_height;
    err = ap_send_blank_rows(top);
    if (err != PDERR_NOERR) {
        ap_abort_active_job();
        ap_release_buffer();
        g_text_error = err;
        return err;
    }

    g_text_page_touched = 1;
    return PDERR_NOERR;
}

static LONG ap_text_send_line(void)
{
    LONG err;
    ULONG glyph_height;
    ULONG top_pad;
    ULONG bottom_pad;
    UWORD row;

    err = ap_text_ensure_page();
    if (err != PDERR_NOERR) return err;

    glyph_height = ap_mul_small(8UL, g_text_glyph_scale_y);
    top_pad = g_text_line_height > glyph_height
        ? ((ULONG)g_text_line_height - glyph_height) >> 1 : 0UL;
    bottom_pad = (ULONG)g_text_line_height - glyph_height - top_pad;

    err = ap_send_blank_rows(top_pad);
    if (err != PDERR_NOERR) goto fail;

    for (row = 0U; row < 8U; ++row) {
        ap_text_build_glyph_row(row);
        err = ap_text_send_row((ULONG)g_text_glyph_scale_y);
        if (err != PDERR_NOERR) goto fail;
    }

    err = ap_send_blank_rows(bottom_pad);
    if (err != PDERR_NOERR) goto fail;

    ++g_text_line_on_page;
    AP_PEDData.ped_PrintMode = 1L;
    ap_text_clear_line();
    return PDERR_NOERR;

fail:
    ap_abort_active_job();
    ap_release_buffer();
    g_text_error = err;
    return err;
}

static LONG ap_text_finish_page(int force_blank)
{
    LONG err = PDERR_NOERR;

    if (g_text_error != PDERR_NOERR) {
        if (g_raster_started) {
            ap_abort_active_job();
            ap_release_buffer();
        }
        return g_text_error;
    }

    if (g_text_line_has_data) {
        err = ap_text_send_line();
        if (err != PDERR_NOERR) return err;
    }

    if (!g_raster_started) {
        /* SPECIAL_NOFORMFEED leaves a graphics page deliberately open after
         * PRS_CLOSE.  A later form feed/reset is the application's explicit
         * request to eject that physical page even when no alphanumeric text
         * was added in between.  Do not mistake the lack of text for an empty
         * page: ap_text_ensure_page() will resume the existing graphics stream
         * at g_rows_sent, not start a new page. */
        if (!force_blank && !g_text_page_touched &&
            !g_graphics_continuation_open) return PDERR_NOERR;
        err = ap_text_ensure_page();
        if (err != PDERR_NOERR) return err;
    }

    if (g_rows_sent < g_page_height) {
        err = ap_send_blank_rows(g_page_height - g_rows_sent);
        if (err != PDERR_NOERR) {
            ap_abort_active_job();
            ap_release_buffer();
            g_text_error = err;
            return err;
        }
    }

    err = ap_finish_current_page();
    if (err != PDERR_NOERR) g_text_error = err;
    ap_release_buffer();

    g_text_line_on_page = 0U;
    g_text_page_touched = 0;
    AP_PEDData.ped_PrintMode = 0L;
    ap_text_clear_line();
    return err;
}

static LONG ap_text_advance_line(int carriage_return)
{
    UWORD old_column = g_text_column;
    LONG err;

    if (g_text_error != PDERR_NOERR) return g_text_error;

    /* A line feed is visible paper movement even when the line is blank. */
    g_text_page_touched = 1;
    err = ap_text_send_line();
    if (err != PDERR_NOERR) return err;

    if (g_text_line_on_page >= g_text_lines_per_page) {
        err = ap_text_finish_page(0);
        if (err != PDERR_NOERR) return err;
    }

    if (!carriage_return) {
        if (old_column < g_text_left_margin) old_column = g_text_left_margin;
        if (old_column > g_text_right_margin + 1U)
            old_column = g_text_right_margin + 1U;
        g_text_column = old_column;
    }
    return PDERR_NOERR;
}

static void ap_text_put_printable(UBYTE c)
{
    if (g_text_error != PDERR_NOERR) return;

    if (g_text_column < g_text_left_margin)
        g_text_column = g_text_left_margin;

    if (g_text_column > g_text_right_margin) {
        if (ap_text_advance_line(1) != PDERR_NOERR) return;
    }

    if (g_text_column >= 1U && g_text_column <= g_text_max_columns) {
        g_text_cells[g_text_column - 1U] = c;
        g_text_styles[g_text_column - 1U] = g_text_style_flags;
        g_text_line_has_data = 1;
        g_text_page_touched = 1;
        AP_PEDData.ped_PrintMode = 1L;
    }
    ++g_text_column;
}

LONG AP_ConvFuncC(APTR output_buffer, LONG c, LONG crlf_flag)
{
    UBYTE ch = (UBYTE)c;
    (void)output_buffer;

    /* Consume the processed stream so no alphanumeric bytes reach the dummy
     * Parallel compatibility port. */
    switch (ch) {
        case 0U:
            break;
        case 8U: /* BS */
            if (g_text_column > g_text_left_margin) --g_text_column;
            break;
        case 9U: /* HT */
        {
            UWORD rel = g_text_column > 0U ? (UWORD)(g_text_column - 1U) : 0U;
            UWORD next = (UWORD)(((rel >> 3) + 1U) << 3);
            g_text_column = (UWORD)(next + 1U);
            if (g_text_column > g_text_right_margin)
                (void)ap_text_advance_line(1);
            break;
        }
        case 10U: /* LF */
            (void)ap_text_advance_line(crlf_flag == 0L ? 1 : 0);
            break;
        case 11U: /* VT: one logical line */
            (void)ap_text_advance_line(0);
            break;
        case 12U: /* FF */
            /* A page-oriented driver only has something to eject when data is
             * pending.  Do not synthesize blank IPP jobs for redundant form
             * feeds; some word processors emit more than one during teardown. */
            if (g_raster_started || g_graphics_continuation_open ||
                g_text_page_touched || g_text_line_has_data)
                (void)ap_text_finish_page(0);
            else
                AP_PEDData.ped_PrintMode = 0L;
            break;
        case 13U: /* CR */
            g_text_column = g_text_left_margin;
            break;
        case 27U:   /* ESC */
        case 0x9BU: /* CSI */
            /* Do NOT consume escape introducers here.  Commodore's printer
             * driver contract explicitly recommends leaving ESC/CSI to
             * printer.device.  Returning -1 lets its processed-text parser
             * consume the complete ANSI/Amiga sequence and then call our
             * CommandTable/DoSpecial hook.  Consuming ESC here would leave
             * the tail (for example "[1m") to be printed as plain text. */
            return -1L;
        default:
            /* Printable ASCII plus the Amiga/Latin-1 graphic range.
             * 0x80..0x9f are C1 controls, not printable glyphs. */
            if ((ch >= 0x20U && ch < 0x7FU) || ch >= 0xA0U)
                ap_text_put_printable(ch);
            break;
    }

    return 0L;
}

static int ap_text_geometry_change_allowed(void)
{
    return !g_raster_started && !g_text_page_touched &&
           !g_text_line_has_data && g_text_line_on_page == 0U;
}

LONG AP_DoSpecialC(UWORD *command, UBYTE *output_buffer, BYTE *vline,
                   BYTE *current_vmi, BYTE *crlf_flag, UBYTE *parms)
{
    UWORD cmd;
    (void)output_buffer;
    (void)vline;

    if (command == NULL) return 0L;
    cmd = *command;

    switch (cmd) {
        case aRIS:
            (void)ap_text_finish_page(0);
            g_text_style_flags = 0U;
            if (ap_text_geometry_change_allowed()) {
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            break;

        case aRIN:
            g_text_style_flags = 0U;
            if (ap_text_geometry_change_allowed()) {
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            if (current_vmi != NULL)
                *current_vmi = (BYTE)(g_text_line_height == 75U ? 27 : 36);
            if (crlf_flag != NULL) *crlf_flag = 0;
            break;

        case aIND:
            (void)ap_text_advance_line(0);
            break;
        case aNEL:
            (void)ap_text_advance_line(1);
            break;

        case aSGR0:
            g_text_style_flags = 0U;
            break;
        case aSGR3:
            g_text_style_flags |= AP_TEXT_STYLE_ITALIC;
            break;
        case aSGR23:
            g_text_style_flags &= (UBYTE)~AP_TEXT_STYLE_ITALIC;
            break;
        case aSGR4:
            g_text_style_flags |= AP_TEXT_STYLE_UNDERLINE;
            break;
        case aSGR24:
            g_text_style_flags &= (UBYTE)~AP_TEXT_STYLE_UNDERLINE;
            break;
        case aSGR1:
            g_text_style_flags |= AP_TEXT_STYLE_BOLD;
            break;
        case aSGR22:
            g_text_style_flags &= (UBYTE)~AP_TEXT_STYLE_BOLD;
            break;

        case aSHORP0:
        case aSHORP1:
        case aSHORP2:
        case aSHORP3:
        case aSHORP4:
            if (AP_PD != NULL && ap_text_geometry_change_allowed()) {
                if (cmd == aSHORP2)
                    AP_PD->pd_Preferences.PrintPitch = ELITE;
                else if (cmd == aSHORP4)
                    AP_PD->pd_Preferences.PrintPitch = FINE;
                else
                    AP_PD->pd_Preferences.PrintPitch = PICA;
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            break;

        case aVERP0:
        case aVERP1:
            if (AP_PD != NULL && ap_text_geometry_change_allowed()) {
                AP_PD->pd_Preferences.PrintSpacing =
                    cmd == aVERP0 ? EIGHT_LPI : SIX_LPI;
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            break;

        case aSLPP:
            if (AP_PD != NULL && parms != NULL && parms[0] != 0U &&
                ap_text_geometry_change_allowed()) {
                AP_PD->pd_Preferences.PaperLength = parms[0];
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            break;

        case aSLRM:
            if (AP_PD != NULL && parms != NULL && parms[0] != 0U &&
                parms[1] >= parms[0] && ap_text_geometry_change_allowed()) {
                AP_PD->pd_Preferences.PrintLeftMargin = parms[0];
                AP_PD->pd_Preferences.PrintRightMargin = parms[1];
                ap_text_update_geometry();
                ap_text_clear_line();
            }
            break;

        default:
            break;
    }

    /* CommandTable entries use 0xFF to delegate here.  Returning zero means
     * the command has no primitive-printer byte sequence to emit. */
    return 0L;
}

static void ap_build_output_row(struct PrtInfo *pinfo)
{
    union colorEntry *colors;
    UWORD *scale;
    ULONG source;
    ULONG dest;
    ULONG row_width;
    ULONG scaled_row_width;
    ULONG dpi_accum;
    UWORD horizontal_accum = 99U;
    UBYTE *pixel;

    ap_memwhite(g_raw_row, g_row_bytes);
    if (pinfo == NULL || pinfo->pi_ColorInt == NULL || pinfo->pi_ScaleX == NULL) return;

    row_width = 0UL;
    scale = pinfo->pi_ScaleX;
    for (source = 0UL; source < (ULONG)pinfo->pi_width; ++source)
        row_width += (ULONG)*scale++;
    scaled_row_width = ap_scale_dimension(ap_frontend_to_pwg(row_width));
    if (scaled_row_width > g_page_width) scaled_row_width = g_page_width;

    colors = pinfo->pi_ColorInt;
    scale = pinfo->pi_ScaleX;
    if (g_center_on_paper && scaled_row_width < g_page_width)
        dest = (g_page_width - scaled_row_width) >> 1;
    else
        dest = ap_frontend_to_pwg((ULONG)pinfo->pi_xpos);
    if (dest >= g_page_width) return;

    pixel = g_raw_row;
    if (g_bytes_per_pixel == 1UL) pixel += dest;
    else {
        ULONG skip = dest;
        while (skip-- != 0UL) pixel += 3;
    }

    dpi_accum = g_frontend_dpi > 0U ? (ULONG)g_frontend_dpi - 1UL : 0UL;
    for (source = 0UL; source < (ULONG)pinfo->pi_width; ++source) {
        ULONG repeats = (ULONG)*scale++;
        UBYTE blue = colors[source].colorByte[PCMBLUE];
        UBYTE green = colors[source].colorByte[PCMGREEN];
        UBYTE red = colors[source].colorByte[PCMRED];
        UBYTE white = colors[source].colorByte[PCMWHITE];

        red = red >= 15U ? 255U : (UBYTE)(((UWORD)red << 4) | red);
        green = green >= 15U ? 255U : (UBYTE)(((UWORD)green << 4) | green);
        blue = blue >= 15U ? 255U : (UBYTE)(((UWORD)blue << 4) | blue);
        white = white >= 15U ? 255U : (UBYTE)(((UWORD)white << 4) | white);

        while (repeats-- != 0UL && dest < g_page_width) {
            ULONG copies;
            dpi_accum += (ULONG)g_output_dpi;
            copies = ap_udiv_small(dpi_accum, g_frontend_dpi);
            dpi_accum -= ap_mul_small(copies, g_frontend_dpi);
            while (copies-- != 0UL && dest < g_page_width) {
                horizontal_accum = (UWORD)(horizontal_accum + g_job_scale_percent);
                if (horizontal_accum >= 100U) {
                    horizontal_accum = (UWORD)(horizontal_accum - 100U);
                    if (g_bytes_per_pixel == 1UL) pixel[0] = white;
                    else { pixel[0] = red; pixel[1] = green; pixel[2] = blue; }
                    pixel = ap_next_pixel(pixel);
                    ++dest;
                }
            }
        }
    }
}

LONG AP_Render(LONG ct, LONG x, LONG y, LONG status)
{
    LONG err = PDERR_NOERR;

    switch (status) {
        case 5: /* PRS_PREINIT */
            /* Text and graphics cannot share one raster page in this driver.
             * Close a pending page-oriented text job before a graphic dump. */
            if (g_text_page_touched || g_text_line_has_data) {
                err = ap_text_finish_page(0);
                if (err != PDERR_NOERR) return err;
            }

            /* Apply selected media only when starting a new physical page.
             * A SPECIAL_NOFORMFEED continuation must retain the exact engine,
             * media and page geometry of the preceding strip. */
            if (!g_graphics_continuation_open) ap_apply_page_prefs();
            ap_set_graphics_density((UWORD)x);
            (void)ct;
            break;

        case 0: /* PRS_INIT */
        {
            int continuing = g_graphics_continuation_open;
            int no_formfeed = 0;
            struct IODRPReq *request = (struct IODRPReq *)ct;

            if (request != NULL &&
                (request->io_Special & SPECIAL_NOFORMFEED) != 0U)
                no_formfeed = 1;

            if (g_page_width == 0UL) ap_apply_page_prefs();
            g_picture_width = x > 0 ? (ULONG)x : 0UL;
            g_picture_height = y > 0 ? (ULONG)y : 0UL;
            if (!continuing) g_job_scale_percent = g_scale_percent;

            /* Keep the logical-density geometry visible for the duration of
             * this dump; PWG output itself remains fixed at the selected dpi. */
            AP_PEDData.ped_MaxXDots = g_frontend_page_width;
            AP_PEDData.ped_MaxYDots = g_frontend_page_height;

            if (!continuing &&
                (ap_frontend_to_pwg(g_picture_width) > g_page_width ||
                 ap_frontend_to_pwg(g_picture_height) > g_page_height)) {
                UWORD fit = ap_job_fit_percent(ap_frontend_to_pwg(g_picture_width),
                                               ap_frontend_to_pwg(g_picture_height));
                ULONG combined = ap_udiv_small(ap_mul_small((ULONG)g_scale_percent, fit), 100U);
                if (combined == 0UL) combined = 1UL;
                if (combined > 100UL) combined = 100UL;
                g_job_scale_percent = (UWORD)combined;
            }

            g_scaled_picture_width = ap_scale_dimension(ap_frontend_to_pwg(g_picture_width));
            g_scaled_picture_height = ap_scale_dimension(ap_frontend_to_pwg(g_picture_height));
            if (g_scaled_picture_width > g_page_width) g_scaled_picture_width = g_page_width;
            if (g_scaled_picture_height > g_page_height) g_scaled_picture_height = g_page_height;

            /* Vertical centering applies to a complete dump, never to one
             * strip of a striped page.  The first strip advertises
             * SPECIAL_NOFORMFEED; later strips are recognized by continuing. */
            g_top_padding = (!continuing && !no_formfeed && g_center_on_paper &&
                             g_scaled_picture_height < g_page_height)
                ? ((g_page_height - g_scaled_picture_height) >> 1) : 0UL;

            if (!continuing) {
                g_vertical_dpi_accum = g_frontend_dpi > 0U
                    ? (ULONG)g_frontend_dpi - 1UL : 0UL;
                g_vertical_scale_accum = 99U;
            }
            g_emit_current_rows = 0U;
            err = ap_start_raster();
            if (err == PDERR_NOERR && g_top_padding != 0UL)
                err = ap_send_blank_rows(g_top_padding);
            break;
        }

        case 1: /* PRS_TRANSFER */
            if (g_raw_row == NULL) return PDERR_BUFFERMEMORY;
            (void)y;
            {
                ULONG base_emit;
                ULONG emit = 0UL;
                g_vertical_dpi_accum += (ULONG)g_output_dpi;
                base_emit = ap_udiv_small(g_vertical_dpi_accum, g_frontend_dpi);
                g_vertical_dpi_accum -= ap_mul_small(base_emit, g_frontend_dpi);
                while (base_emit-- != 0UL) {
                    ULONG scale_accum = (ULONG)g_vertical_scale_accum +
                                        (ULONG)g_job_scale_percent;
                    if (scale_accum >= 100UL) {
                        ++emit;
                        scale_accum -= 100UL;
                    }
                    g_vertical_scale_accum = (UWORD)scale_accum;
                }
                g_emit_current_rows = emit > 65535UL ? 65535U : (UWORD)emit;
            }
            if (g_emit_current_rows != 0U)
                ap_build_output_row((struct PrtInfo *)ct);
            break;

        case 2: /* PRS_FLUSH */
            if (g_raw_row == NULL || g_encoded_row == NULL) return PDERR_BUFFERMEMORY;
            (void)x;
            (void)y;
            if (g_emit_current_rows != 0U && g_rows_sent < g_page_height) {
                ULONG copies = (ULONG)g_emit_current_rows;
                if (copies > g_page_height - g_rows_sent)
                    copies = g_page_height - g_rows_sent;
                err = ap_write_row_repeat(g_raw_row, copies);
            }
            break;

        case 3: /* PRS_CLEAR */
            /* The row is reset to white immediately before every transfer. */
            break;

        case 4: /* PRS_CLOSE */
            if (!g_raster_started) {
                ap_release_buffer();
                break;
            }

            if (ct == PDERR_CANCEL) {
                ap_abort_active_job();
                ap_release_buffer();
                break;
            }

            /* SPECIAL_NOFORMFEED explicitly means that this graphics dump is
             * not the end of the physical page.  FinalWriter Graphics (Final)
             * uses this for horizontal strip printing.  Keep the document
             * stream and current raster row position open, but release the
             * per-dump row buffers before printer.device starts the next strip. */
            if ((((UWORD)x) & SPECIAL_NOFORMFEED) != 0U) {
                g_graphics_continuation_open = 1;
                ap_release_buffer();
                break;
            }

            if (g_rows_sent < g_page_height)
                err = ap_send_blank_rows(g_page_height - g_rows_sent);
            if (err == PDERR_NOERR) err = ap_finish_current_page();
            g_graphics_continuation_open = 0;
            ap_release_buffer();
            break;

        case 6: /* PRS_NEXTCOLOR */
            /* Not used: PCC_BGR is a single-pass additive color class. */
            break;

        default:
            break;
    }

    return err;
}
