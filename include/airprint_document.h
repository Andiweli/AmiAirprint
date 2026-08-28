#ifndef AIRPRINT_DOCUMENT_H
#define AIRPRINT_DOCUMENT_H

#include <exec/types.h>

#define AP_DOCUMENT_PDF        1U
#define AP_DOCUMENT_POSTSCRIPT 2U

#define AP_DOCUMENT_MAX_OBJECTS 7U

typedef LONG (*APDocumentWriteFunc)(const void *data, ULONG length);

struct APDocumentWriter {
    UWORD format;
    UWORD components;
    ULONG width;
    ULONG height;
    ULONG width_points;
    ULONG height_points;
    ULONG row_bytes;
    ULONG bytes_written;
    ULONG stream_bytes;
    ULONG object_offsets[AP_DOCUMENT_MAX_OBJECTS];
    APDocumentWriteFunc write_func;
};

ULONG ap_document_scratch_size(ULONG row_bytes);
LONG ap_document_begin(struct APDocumentWriter *writer,
                       UWORD format,
                       UWORD components,
                       ULONG width,
                       ULONG height,
                       ULONG width_points,
                       ULONG height_points,
                       APDocumentWriteFunc write_func);
LONG ap_document_write_row(struct APDocumentWriter *writer,
                           const UBYTE *row,
                           UBYTE *scratch,
                           ULONG scratch_size);
LONG ap_document_end(struct APDocumentWriter *writer);

#endif
