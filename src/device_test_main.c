#include "airprint_device.h"
#include "testpage_jpeg.h"

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <stdio.h>

#define TEST_CHUNK 4096U

static const char *stage_name(UWORD stage)
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

static void show_device_status(struct IOStdReq *io)
{
    struct APDeviceStatus status;
    LONG error;

    io->io_Command = APDEV_CMD_GET_STATUS;
    io->io_Data = &status;
    io->io_Length = sizeof(status);
    io->io_Actual = 0;
    error = DoIO((struct IORequest *)io);
    if (error != 0) return;

    printf("Device status: stage=%s bytes=%lu socket_errno=%ld HTTP=%ld IPP=0x%04x\n",
           stage_name(status.stage),
           (unsigned long)status.bytes_spooled,
           (long)status.socket_errno,
           (long)status.http_status,
           (unsigned int)status.ipp_status);
}

static int send_write(struct IOStdReq *io, const void *data, ULONG length)
{
    LONG error;

    io->io_Command = CMD_WRITE;
    io->io_Data = (APTR)data;
    io->io_Length = length;
    io->io_Actual = 0;
    error = DoIO((struct IORequest *)io);
    if (error != 0) {
        printf("CMD_WRITE failed: io_Error=%ld io_Actual=%lu\n",
               (long)io->io_Error, (unsigned long)io->io_Actual);
        return 0;
    }
    return 1;
}

int main(void)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    struct APDeviceControl control;
    ULONG offset;
    int opened;

    puts("AirPrintDeviceTest 0.8.3");
    puts("Exec device -> spool -> IPP Print-Job test");
    puts("");

    port = CreateMsgPort();
    if (port == NULL) {
        puts("Could not create message port.");
        return 20;
    }

    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (io == NULL) {
        DeleteMsgPort(port);
        puts("Could not create I/O request.");
        return 20;
    }

    opened = OpenDevice((CONST_STRPTR)APDEV_NAME, 0, (struct IORequest *)io, 0) == 0;
    if (!opened) {
        printf("Could not open %s (error %ld). Copy it to DEVS: first.\n",
               APDEV_NAME, (long)io->io_Error);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    printf("Opened %s unit 0.\n", APDEV_NAME);

    control.magic = APDEV_CONTROL_MAGIC;
    control.version = APDEV_CONTROL_VERSION;
    control.command = APDEV_CTL_BEGIN;
    control.format = APDEV_FORMAT_JPEG;
    control.reserved = 0U;

    if (!send_write(io, &control, (ULONG)sizeof(control))) {
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    printf("Spooling %lu byte JPEG in %u byte chunks...\n",
           (unsigned long)g_airprint_testpage_jpeg_len, (unsigned int)TEST_CHUNK);

    offset = 0U;
    while (offset < (ULONG)g_airprint_testpage_jpeg_len) {
        ULONG length;
        length = (ULONG)g_airprint_testpage_jpeg_len - offset;
        if (length > TEST_CHUNK) length = TEST_CHUNK;
        if (!send_write(io, g_airprint_testpage_jpeg + offset, length)) {
            control.command = APDEV_CTL_ABORT;
            send_write(io, &control, (ULONG)sizeof(control));
            CloseDevice((struct IORequest *)io);
            DeleteIORequest((struct IORequest *)io);
            DeleteMsgPort(port);
            return 20;
        }
        offset += length;
    }

    control.command = APDEV_CTL_END;
    puts("Submitting spool through airprint.device...");
    if (!send_write(io, &control, (ULONG)sizeof(control))) {
        show_device_status(io);
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return 20;
    }

    show_device_status(io);
    puts("Print job accepted by the printer.");

    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return 0;
}
