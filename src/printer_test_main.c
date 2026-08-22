#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/preferences.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include "airprint_device.h"
#include "ami_airprint_version.h"

#include <stdio.h>
#include <string.h>

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

union APPrinterIO {
    struct IOStdReq ios;
    struct IODRPReq iodrp;
    struct IOPrtCmdReq iopc;
};

static void ap_message(const char *text)
{
    puts(text);
    fflush(stdout);
}



static int ap_arg_equals(const char *arg, const char *word)
{
    unsigned char a;
    unsigned char b;

    if (arg == NULL || word == NULL) return 0;
    while (*arg != '\0' && *word != '\0') {
        a = (unsigned char)*arg++;
        b = (unsigned char)*word++;
        if (a >= (unsigned char)'a' && a <= (unsigned char)'z')
            a = (unsigned char)(a - (unsigned char)'a' + (unsigned char)'A');
        if (b >= (unsigned char)'a' && b <= (unsigned char)'z')
            b = (unsigned char)(b - (unsigned char)'a' + (unsigned char)'A');
        if (a != b) return 0;
    }
    return *arg == '\0' && *word == '\0';
}

static const char *ap_stage_name(UWORD stage)
{
    switch (stage) {
        case APDEV_STAGE_IDLE: return "idle";
        case APDEV_STAGE_SPOOL: return "spool";
        case APDEV_STAGE_PREFS: return "prefs";
        case APDEV_STAGE_CONNECT: return "connect";
        case APDEV_STAGE_CONTINUE: return "100-continue";
        case APDEV_STAGE_SEND: return "send";
        case APDEV_STAGE_RESPONSE: return "response";
        case APDEV_STAGE_DONE: return "done";
        default: return "unknown";
    }
}

static void ap_print_device_status(void)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    struct APDeviceStatus status;

    port = CreateMsgPort();
    if (port == NULL) return;
    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io == NULL) {
        DeleteMsgPort(port);
        return;
    }

    if (OpenDevice((CONST_STRPTR)APDEV_NAME, 0, (struct IORequest *)io, 0) == 0) {
        memset(&status, 0, sizeof(status));
        io->io_Command = APDEV_CMD_GET_STATUS;
        io->io_Data = &status;
        io->io_Length = sizeof(status);
        if (DoIO((struct IORequest *)io) == 0 && io->io_Error == 0) {
            printf("airprint.device: stage=%s(%u) bytes=%lu format=%u HTTP=%ld IPP=0x%04x socket=%ld active=%u\n",
                   ap_stage_name(status.stage), (unsigned int)status.stage,
                   (unsigned long)status.bytes_spooled, (unsigned int)status.format,
                   (long)status.http_status, (unsigned int)status.ipp_status,
                   (long)status.socket_errno, (unsigned int)status.job_active);
            fflush(stdout);
        }
        CloseDevice((struct IORequest *)io);
    }

    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
}

static const char *ap_driver_name(struct PrinterData *pd)
{
    if (pd == NULL) return NULL;
    if (pd->pd_DriverName != NULL) return (const char *)pd->pd_DriverName;
    if (pd->pd_SegmentData != NULL)
        return (const char *)pd->pd_SegmentData->ps_PED.ped_PrinterName;
    return NULL;
}

static int is_airprint_driver(struct PrinterData *pd)
{
    const char *name = ap_driver_name(pd);
    return name != NULL && strcmp(name, "AirPrint") == 0;
}

static void ap_print_graphics_prefs(const struct PrinterData *pd)
{
    if (pd == NULL) return;
    printf("PrinterGfx copy: flags=0x%04x max=%ux%u margins=%u..%u aspect=%u shade=%u density=%u\n",
           (unsigned int)pd->pd_Preferences.PrintFlags,
           (unsigned int)pd->pd_Preferences.PrintMaxWidth,
           (unsigned int)pd->pd_Preferences.PrintMaxHeight,
           (unsigned int)pd->pd_Preferences.PrintLeftMargin,
           (unsigned int)pd->pd_Preferences.PrintRightMargin,
           (unsigned int)pd->pd_Preferences.PrintAspect,
           (unsigned int)pd->pd_Preferences.PrintShade,
           (unsigned int)pd->pd_Preferences.PrintDensity);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct MsgPort *port = NULL;
    union APPrinterIO *pio = NULL;
    struct Screen *screen = NULL;
    struct PrinterData *pd = NULL;
    struct PrinterSegment *segment = NULL;
    ULONG mode_id;
    LONG error = 20;
    int device_open = 0;
    int open_only = 0;
    int stretch_mode = 0;
    int preflight_only = 0;
    int text_mode = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (ap_arg_equals(argv[argi], "OPENONLY")) open_only = 1;
        else if (ap_arg_equals(argv[argi], "STRETCH")) stretch_mode = 1;
        else if (ap_arg_equals(argv[argi], "ASPECT")) stretch_mode = 0; /* compatibility alias */
        else if (ap_arg_equals(argv[argi], "PREFLIGHT")) preflight_only = 1;
        else if (ap_arg_equals(argv[argi], "TEXT") ||
                 ap_arg_equals(argv[argi], "TEXTONLY")) text_mode = 1;
    }

    ap_message("AirPrintPrinterTest " AMIAIRPRINT_CORE_VERSION_TEXT);
    ap_message("printer.device -> DEVS:Printers/AirPrint -> airprint.device test");
    if (text_mode)
        ap_message("Mode: TEXT/CMD_WRITE (no Workbench RastPort dump will be opened)");
    else if (preflight_only)
        ap_message("Mode: graphics PREFLIGHT");
    else
        ap_message("Mode: graphics Workbench RastPort dump");
    ap_message("");

    port = CreateMsgPort();
    if (port == NULL) {
        ap_message("Could not create message port.");
        goto cleanup;
    }

    pio = (union APPrinterIO *)CreateIORequest(port, sizeof(union APPrinterIO));
    if (pio == NULL) {
        ap_message("Could not create printer I/O request.");
        goto cleanup;
    }

    ap_message("[1/4] Opening printer.device...");
    if (OpenDevice((CONST_STRPTR)"printer.device", 0,
                   (struct IORequest *)pio, 0) != 0) {
        printf("Could not open printer.device (error %ld).\n",
               (long)pio->ios.io_Error);
        fflush(stdout);
        if (pio->ios.io_Error == IOERR_UNITBUSY) {
            ap_message("IOERR_UNITBUSY (-6): printer.device is already owned by another task.");
            ap_message("This is a local exclusive-device condition, not an IPP/network error.");
            ap_message("Close any program/PRT: user that is printing; if none is visible, reboot once.");
        } else {
            ap_message("Check SYS:Prefs/Printer and the selected output device.");
        }
        goto cleanup;
    }
    device_open = 1;
    ap_message("[2/4] printer.device opened successfully.");

    pd = (struct PrinterData *)pio->iodrp.io_Device;
    if (pd != NULL) segment = pd->pd_SegmentData;

    if (segment != NULL) {
        printf("Driver: %s  segment version %u.%u\n",
               ap_driver_name(pd) != NULL ? ap_driver_name(pd) : "unknown",
               (unsigned int)segment->ps_Version,
               (unsigned int)segment->ps_Revision);
        fflush(stdout);
    }

    if (pd != NULL && segment != NULL) {
        struct PrinterExtendedData *ped = &segment->ps_PED;
        APTR abs_exec = *(APTR *)4;
        printf("ExecBase: absolute=%p PrinterData=%p\n",
               abs_exec, pd->pd_Device.dd_ExecBase);
        printf("PED: class=%u color=0x%02x columns=%u charsets=%u rows=%u max=%lux%lu dpi=%ux%u\n",
               (unsigned int)ped->ped_PrinterClass,
               (unsigned int)ped->ped_ColorClass,
               (unsigned int)ped->ped_MaxColumns,
               (unsigned int)ped->ped_NumCharSets,
               (unsigned int)ped->ped_NumRows,
               (unsigned long)ped->ped_MaxXDots,
               (unsigned long)ped->ped_MaxYDots,
               (unsigned int)ped->ped_XDotsInch,
               (unsigned int)ped->ped_YDotsInch);
        ap_print_graphics_prefs(pd);
        fflush(stdout);
    }

    if (!is_airprint_driver(pd)) {
        printf("Current printer driver is '%s', not 'AirPrint'.\n",
               ap_driver_name(pd) != NULL ? ap_driver_name(pd) : "unknown");
        fflush(stdout);
        ap_message("Select DEVS:Printers/AirPrint in SYS:Prefs/Printer first.");
        goto cleanup;
    }

    ap_message("[3/4] AirPrint driver header validated.");

    if (open_only) {
        ap_message("OPENONLY requested: closing without submitting a print job.");
        error = 0;
        goto cleanup;
    }

    if (text_mode) {
        static const char native_text_test[] =
            "\033#1"
            "AmiAirPrint native PRT:/CMD_WRITE text test\n"
            "===========================================\n\n"
            "ASCII: The quick brown fox jumps over the lazy dog. 0123456789\n"
            "Tabs:\t1\t2\t3\n"
            "Latin-1: AOU aou ss = "
            "\304\326\334 \344\366\374 \337\n"
            "\033[1mBold\033[22m  "
            "\033[3mItalic\033[23m  "
            "\033[4mUnderline\033[24m\n"
            "\nIf this page is black text on white paper, native text works.\n"
            "\014";
        LONG text_error;

        ap_message("[4/4] Submitting processed native text with CMD_WRITE...");
        pio->ios.io_Command = CMD_WRITE;
        pio->ios.io_Data = (APTR)native_text_test;
        pio->ios.io_Length = (ULONG)-1L;
        text_error = DoIO((struct IORequest *)pio);
        if (text_error == 0 && pio->ios.io_Error == 0) {
            ap_message("CMD_WRITE completed. Form feed submitted the PWG text page.");
            ap_print_device_status();
            error = 0;
        } else {
            printf("CMD_WRITE failed: DoIO=%ld io_Error=%ld\n",
                   (long)text_error, (long)pio->ios.io_Error);
            fflush(stdout);
            ap_print_device_status();
            error = 20;
        }
        goto cleanup;
    }

    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39L);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39L);
    if (GfxBase == NULL || IntuitionBase == NULL) {
        ap_message("Could not open AmigaOS 3.0+ graphics/intuition libraries.");
        goto cleanup;
    }

    screen = LockPubScreen(NULL);
    if (screen == NULL) {
        ap_message("Could not lock the default public screen.");
        goto cleanup;
    }

    mode_id = GetVPModeID(&screen->ViewPort);

    pio->iodrp.io_Command = PRD_DUMPRPORT;
    pio->iodrp.io_RastPort = &screen->RastPort;
    pio->iodrp.io_ColorMap = screen->ViewPort.ColorMap;
    pio->iodrp.io_Modes = mode_id;
    pio->iodrp.io_SrcX = 0;
    pio->iodrp.io_SrcY = 0;
    pio->iodrp.io_SrcWidth = screen->Width;
    pio->iodrp.io_SrcHeight = screen->Height;
    if (stretch_mode) {
        /*
         * Diagnostic only: request the driver's exact physical page raster.
         * This deliberately allows distortion and is useful to prove that the
         * complete selected-media PWG surface can be addressed.
         */
        if (segment != NULL) {
            pio->iodrp.io_DestCols = (LONG)segment->ps_PED.ped_MaxXDots;
            pio->iodrp.io_DestRows = (LONG)segment->ps_PED.ped_MaxYDots;
        } else {
            pio->iodrp.io_DestCols = 0;
            pio->iodrp.io_DestRows = 0;
        }
        pio->iodrp.io_Special = SPECIAL_DENSITY6;
    } else {
        /*
         * Classic Amiga full-screen default: maximum width, preserve the
         * source display aspect ratio.  Commodore's screendump() helper uses
         * exactly FULLCOLS + ASPECT for this case.
         */
        pio->iodrp.io_DestCols = 0;
        pio->iodrp.io_DestRows = 0;
        pio->iodrp.io_Special = SPECIAL_DENSITY6 | SPECIAL_FULLCOLS |
                                SPECIAL_ASPECT;
    }

    ap_message("[4/4] Submitting the complete Workbench RastPort through printer.device...");
    if (stretch_mode) {
        ap_message("v" AMIAIRPRINT_CORE_VERSION_TEXT " STRETCH mode: use the open-time selected-media PED raster.");
    } else {
        ap_message("v" AMIAIRPRINT_CORE_VERSION_TEXT " FIT mode: maximum selected-media width while preserving Workbench proportions.");
    }
    printf("Source: %ux%u  target request: %s\n",
           (unsigned int)pio->iodrp.io_SrcWidth,
           (unsigned int)pio->iodrp.io_SrcHeight,
           stretch_mode ? "EXACT PAGE PIXELS (distorted)" : "FULLCOLS+ASPECT");
    printf("Public memory before dump: free=%lu largest=%lu bytes\n",
           (unsigned long)AvailMem(MEMF_PUBLIC),
           (unsigned long)AvailMem(MEMF_PUBLIC | MEMF_LARGEST));
    fflush(stdout);

    if (preflight_only) {
        LONG preflight_error;
        UWORD requested_special = pio->iodrp.io_Special;

        ap_message("PREFLIGHT requested: computing geometry without printing.");
        pio->iodrp.io_Special = requested_special | SPECIAL_NOPRINT;
        preflight_error = DoIO((struct IORequest *)pio);
        if (preflight_error != 0 || pio->iodrp.io_Error != 0) {
            printf("Preflight failed: DoIO=%ld io_Error=%ld\n",
                   (long)preflight_error, (long)pio->iodrp.io_Error);
            fflush(stdout);
            error = 20;
            goto cleanup;
        }
        printf("Preflight printer.device size: %ld x %ld printer pixels\n",
               (long)pio->iodrp.io_DestCols, (long)pio->iodrp.io_DestRows);
        ap_print_graphics_prefs(pd);
        fflush(stdout);
        error = 0;
        goto cleanup;
    }

    error = DoIO((struct IORequest *)pio);
    if (error == 0 && pio->iodrp.io_Error == 0) {
        printf("Actual printer.device dump size: %ld x %ld printer pixels\n",
               (long)pio->iodrp.io_DestCols, (long)pio->iodrp.io_DestRows);
        fflush(stdout);
        ap_message("printer.device completed the AirPrint driver request successfully.");
        ap_print_device_status();
        error = 0;
    } else {
        printf("printer.device failed: DoIO=%ld io_Error=%ld\n",
               (long)error, (long)pio->iodrp.io_Error);
        if (pio->iodrp.io_Error == PDERR_INTERNALMEMORY) {
            ap_message("PDERR_INTERNALMEMORY: printer-driver initialization/internal allocation failed.");
        } else if (pio->iodrp.io_Error == PDERR_BUFFERMEMORY) {
            ap_message("PDERR_BUFFERMEMORY: printer/driver print-buffer allocation failed.");
        }
        fflush(stdout);
        ap_print_device_status();
        error = 20;
    }

cleanup:
    if (device_open && pio != NULL) {
        ap_message("[close] Closing printer.device...");
        CloseDevice((struct IORequest *)pio);
        device_open = 0;
        ap_message("[close] printer.device closed.");
    }
    if (pio != NULL) DeleteIORequest((struct IORequest *)pio);
    if (port != NULL) DeleteMsgPort(port);
    if (screen != NULL) UnlockPubScreen(NULL, screen);
    if (IntuitionBase != NULL) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase != NULL) CloseLibrary((struct Library *)GfxBase);
    return (int)error;
}
