#ifndef AIRPRINT_DEVICE_H
#define AIRPRINT_DEVICE_H

#include <exec/types.h>
#include <exec/io.h>

#define APDEV_NAME "airprint.device"

#define APDEV_CONTROL_MAGIC   0x41504456UL /* 'APDV' */
#define APDEV_CONTROL_VERSION 1U

#define APDEV_CTL_BEGIN 1U
#define APDEV_CTL_END   2U
#define APDEV_CTL_ABORT 3U

#define APDEV_FORMAT_JPEG       1U
#define APDEV_FORMAT_URF        2U
#define APDEV_FORMAT_PWG_RASTER 3U

#define APDEV_CMD_GET_STATUS       (CMD_NONSTD + 1U)
#define APDEV_CMD_SET_DRIVER_DIAG  (CMD_NONSTD + 2U)

#define APDEV_DRIVER_DIAG_VERSION 4U

#define APDEV_STAGE_IDLE       0U
#define APDEV_STAGE_SPOOL      1U
#define APDEV_STAGE_PREFS      2U
#define APDEV_STAGE_CONNECT    3U
#define APDEV_STAGE_CONTINUE   4U
#define APDEV_STAGE_SEND       5U
#define APDEV_STAGE_RESPONSE   6U
#define APDEV_STAGE_DONE       7U

struct APDeviceControl {
    ULONG magic;
    UWORD version;
    UWORD command;
    UWORD format;
    UWORD reserved;
};

struct APDriverDiag {
    UWORD version;
    UWORD session;
    ULONG events_mask;
    ULONG render_calls[7];
    LONG last_ct;
    LONG last_x;
    LONG last_y;
    LONG last_status;
    LONG last_result;
    LONG open_result;
    LONG preinit_ct;
    LONG preinit_special;
    LONG preinit1_dest_cols;
    LONG preinit1_dest_rows;
    ULONG preinit1_modes;
    UWORD preinit1_src_x;
    UWORD preinit1_src_y;
    UWORD preinit1_src_width;
    UWORD preinit1_src_height;
    UWORD preinit1_special;
    UWORD preinit1_reserved;
    LONG preinit2_dest_cols;
    LONG preinit2_dest_rows;
    ULONG preinit2_modes;
    UWORD preinit2_src_x;
    UWORD preinit2_src_y;
    UWORD preinit2_src_width;
    UWORD preinit2_src_height;
    UWORD preinit2_special;
    UWORD preinit2_reserved;
    ULONG preinit_ped_max_x;
    ULONG preinit_ped_max_y;
    UWORD preinit_ped_x_dpi;
    UWORD preinit_ped_y_dpi;
    UWORD preinit_print_density;
    UWORD preinit_paper_size;
    UWORD preinit_print_aspect;
    UWORD preinit_reserved2;
    LONG init_x;
    LONG init_y;
    LONG first_transfer_y;
    UWORD first_transfer_width;
    UWORD first_transfer_xpos;
    ULONG flush_rows_total;
    LONG close_error;
    LONG close_special;
    ULONG rows_sent;
    UWORD raster_started;
    UWORD transport_open;
    UWORD close_seen;
    UWORD compat_mil_active;
    ULONG compat_max_x;
    ULONG compat_max_y;
    UWORD job_scale_percent;
    UWORD primitive_io_hooked;
    ULONG primitive_write_calls;
    ULONG primitive_write_bytes;
    ULONG primitive_bothready_calls;
};

struct APDeviceStatus {
    ULONG bytes_spooled;
    LONG socket_errno;
    LONG http_status;
    UWORD ipp_status;
    UWORD format;
    UWORD stage;
    UWORD job_active;
    struct APDriverDiag driver_diag;
};

#endif
