#include "airprint_document.h"

#include <devices/printer.h>
#include <stddef.h>

#define AP_PDF_OBJECT_COUNT 6U

static ULONG ap_doc_strlen(const char *text)
{
    ULONG length = 0UL;
    if (text == NULL) return 0UL;
    while (text[length] != '\0') ++length;
    return length;
}

static LONG ap_doc_write(struct APDocumentWriter *writer,
                         const void *data,
                         ULONG length)
{
    LONG result;

    if (writer == NULL || writer->write_func == NULL ||
        data == NULL || length == 0UL) return PDERR_CANCEL;

    result = writer->write_func(data, length);
    if (result == PDERR_NOERR) writer->bytes_written += length;
    return result;
}

static LONG ap_doc_write_text(struct APDocumentWriter *writer, const char *text)
{
    ULONG length = ap_doc_strlen(text);
    if (length == 0UL) return PDERR_NOERR;
    return ap_doc_write(writer, text, length);
}

static ULONG ap_doc_ulong_text(ULONG value, char *buffer)
{
    static const ULONG places[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    ULONG i;
    ULONG out = 0UL;
    int started = 0;

    for (i = 0UL; i < 10UL; ++i) {
        UBYTE digit = 0U;
        while (value >= places[i]) {
            value -= places[i];
            ++digit;
        }
        if (digit != 0U || started || places[i] == 1UL) {
            buffer[out++] = (char)('0' + digit);
            started = 1;
        }
    }
    buffer[out] = '\0';
    return out;
}

static LONG ap_doc_write_ulong(struct APDocumentWriter *writer, ULONG value)
{
    char buffer[16];
    ULONG length = ap_doc_ulong_text(value, buffer);
    return ap_doc_write(writer, buffer, length);
}

static LONG ap_doc_write_pdf_xref_ulong(struct APDocumentWriter *writer, ULONG value)
{
    static const ULONG places[10] = {
        1000000000UL, 100000000UL, 10000000UL, 1000000UL, 100000UL,
        10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    char buffer[12];
    ULONG i;

    for (i = 0UL; i < 10UL; ++i) {
        UBYTE digit = 0U;
        while (value >= places[i]) {
            value -= places[i];
            ++digit;
        }
        buffer[i] = (char)('0' + digit);
    }
    buffer[10] = ' ';
    buffer[11] = '\0';
    return ap_doc_write(writer, buffer, 11UL);
}

static ULONG ap_doc_rle_row(const UBYTE *input,
                            ULONG input_length,
                            UBYTE *output,
                            ULONG output_size)
{
    ULONG in_pos = 0UL;
    ULONG out_pos = 0UL;

    if (input == NULL || output == NULL) return 0UL;

    while (in_pos < input_length) {
        ULONG repeat = 1UL;

        while (in_pos + repeat < input_length && repeat < 128UL &&
               input[in_pos + repeat] == input[in_pos]) {
            ++repeat;
        }

        if (repeat >= 3UL) {
            if (out_pos + 2UL > output_size) return 0UL;
            output[out_pos++] = (UBYTE)(257UL - repeat);
            output[out_pos++] = input[in_pos];
            in_pos += repeat;
        } else {
            ULONG literal_start = in_pos;
            ULONG literal_count = 0UL;

            while (in_pos < input_length && literal_count < 128UL) {
                repeat = 1UL;
                while (in_pos + repeat < input_length && repeat < 128UL &&
                       input[in_pos + repeat] == input[in_pos]) {
                    ++repeat;
                }
                if (repeat >= 3UL && literal_count != 0UL) break;
                if (repeat >= 3UL) break;

                ++in_pos;
                ++literal_count;
                if (repeat == 2UL && in_pos < input_length &&
                    literal_count < 128UL) {
                    ++in_pos;
                    ++literal_count;
                }
            }

            if (literal_count == 0UL) continue;
            if (out_pos + 1UL + literal_count > output_size) return 0UL;
            output[out_pos++] = (UBYTE)(literal_count - 1UL);
            while (literal_count-- != 0UL)
                output[out_pos++] = input[literal_start++];
        }
    }

    return out_pos;
}

ULONG ap_document_scratch_size(ULONG row_bytes)
{
    if (row_bytes == 0UL) return 0UL;
    return row_bytes + ((row_bytes + 127UL) >> 7) + 2UL;
}

static LONG ap_pdf_begin(struct APDocumentWriter *writer)
{
    LONG err;

    err = ap_doc_write_text(writer, "%PDF-1.3\n%\342\343\317\323\n");
    if (err != PDERR_NOERR) return err;

    writer->object_offsets[1] = writer->bytes_written;
    err = ap_doc_write_text(writer,
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    if (err != PDERR_NOERR) return err;

    writer->object_offsets[2] = writer->bytes_written;
    err = ap_doc_write_text(writer,
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    if (err != PDERR_NOERR) return err;

    writer->object_offsets[3] = writer->bytes_written;
    err = ap_doc_write_text(writer,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer,
        "] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 6 0 R >>\nendobj\n");
    if (err != PDERR_NOERR) return err;

    writer->object_offsets[4] = writer->bytes_written;
    err = ap_doc_write_text(writer,
        "4 0 obj\n<< /Type /XObject /Subtype /Image /Width ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " /Height ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer,
        writer->components == 1U
            ? " /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /RunLengthDecode /Length 5 0 R >>\nstream\n"
            : " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /RunLengthDecode /Length 5 0 R >>\nstream\n");
    return err;
}

static LONG ap_ps_begin(struct APDocumentWriter *writer)
{
    LONG err;

    err = ap_doc_write_text(writer,
        "%!PS-Adobe-3.0\n%%Creator: AmiAirPrint\n%%Pages: 1\n%%BoundingBox: 0 0 ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer,
        "\n%%LanguageLevel: 2\n%%EndComments\n<< /PageSize [");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, "] >> setpagedevice\ngsave\n");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height_points);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer,
        " scale\n/Data currentfile /RunLengthDecode filter def\n/picstr ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->row_bytes);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " string def\n");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " 8\n[");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->width);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " 0 0 -");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " 0 ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->height);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer,
        writer->components == 1U
            ? "]\n{ Data picstr readstring pop } image\n"
            : "]\n{ Data picstr readstring pop } false 3 colorimage\n");
    return err;
}

LONG ap_document_begin(struct APDocumentWriter *writer,
                       UWORD format,
                       UWORD components,
                       ULONG width,
                       ULONG height,
                       ULONG width_points,
                       ULONG height_points,
                       APDocumentWriteFunc write_func)
{
    ULONG i;

    if (writer == NULL || write_func == NULL ||
        (format != AP_DOCUMENT_PDF && format != AP_DOCUMENT_POSTSCRIPT) ||
        (components != 1U && components != 3U) ||
        width == 0UL || height == 0UL ||
        width_points == 0UL || height_points == 0UL) return PDERR_CANCEL;

    writer->format = format;
    writer->components = components;
    writer->width = width;
    writer->height = height;
    writer->width_points = width_points;
    writer->height_points = height_points;
    writer->row_bytes = components == 1U ? width : (width << 1) + width;
    writer->bytes_written = 0UL;
    writer->stream_bytes = 0UL;
    writer->write_func = write_func;
    for (i = 0UL; i < AP_DOCUMENT_MAX_OBJECTS; ++i)
        writer->object_offsets[i] = 0UL;

    return format == AP_DOCUMENT_PDF
        ? ap_pdf_begin(writer) : ap_ps_begin(writer);
}

LONG ap_document_write_row(struct APDocumentWriter *writer,
                           const UBYTE *row,
                           UBYTE *scratch,
                           ULONG scratch_size)
{
    ULONG encoded;
    LONG err;

    if (writer == NULL || row == NULL || scratch == NULL ||
        scratch_size < ap_document_scratch_size(writer->row_bytes))
        return PDERR_BUFFERMEMORY;

    encoded = ap_doc_rle_row(row, writer->row_bytes, scratch, scratch_size);
    if (encoded == 0UL) return PDERR_BUFFERMEMORY;

    err = ap_doc_write(writer, scratch, encoded);
    if (err == PDERR_NOERR) writer->stream_bytes += encoded;
    return err;
}

static LONG ap_pdf_end(struct APDocumentWriter *writer)
{
    static const UBYTE eod = 128U;
    LONG err;
    ULONG xref_offset;
    ULONG content_length;
    char content[96];
    ULONG pos = 0UL;
    char number[16];
    ULONG number_len;
    ULONG object;

    err = ap_doc_write(writer, &eod, 1UL);
    if (err != PDERR_NOERR) return err;
    ++writer->stream_bytes;

    err = ap_doc_write_text(writer, "\nendstream\nendobj\n");
    if (err != PDERR_NOERR) return err;

    writer->object_offsets[5] = writer->bytes_written;
    err = ap_doc_write_text(writer, "5 0 obj\n");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, writer->stream_bytes);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, "\nendobj\n");
    if (err != PDERR_NOERR) return err;

    content[pos++] = 'q'; content[pos++] = '\n';
    number_len = ap_doc_ulong_text(writer->width_points, number);
    {
        ULONG j;
        for (j = 0UL; j < number_len; ++j) content[pos++] = number[j];
    }
    content[pos++] = ' '; content[pos++] = '0'; content[pos++] = ' '; content[pos++] = '0'; content[pos++] = ' ';
    number_len = ap_doc_ulong_text(writer->height_points, number);
    {
        ULONG j;
        for (j = 0UL; j < number_len; ++j) content[pos++] = number[j];
    }
    content[pos++] = ' '; content[pos++] = '0'; content[pos++] = ' '; content[pos++] = '0'; content[pos++] = ' ';
    content[pos++] = 'c'; content[pos++] = 'm'; content[pos++] = '\n';
    content[pos++] = '/'; content[pos++] = 'I'; content[pos++] = 'm'; content[pos++] = '0'; content[pos++] = ' ';
    content[pos++] = 'D'; content[pos++] = 'o'; content[pos++] = '\n';
    content[pos++] = 'Q'; content[pos++] = '\n';
    content_length = pos;

    writer->object_offsets[6] = writer->bytes_written;
    err = ap_doc_write_text(writer, "6 0 obj\n<< /Length ");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, content_length);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, " >>\nstream\n");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write(writer, content, content_length);
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_text(writer, "endstream\nendobj\n");
    if (err != PDERR_NOERR) return err;

    xref_offset = writer->bytes_written;
    err = ap_doc_write_text(writer, "xref\n0 7\n0000000000 65535 f \n");
    if (err != PDERR_NOERR) return err;
    for (object = 1UL; object <= AP_PDF_OBJECT_COUNT; ++object) {
        err = ap_doc_write_pdf_xref_ulong(writer, writer->object_offsets[object]);
        if (err != PDERR_NOERR) return err;
        err = ap_doc_write_text(writer, "00000 n \n");
        if (err != PDERR_NOERR) return err;
    }
    err = ap_doc_write_text(writer,
        "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n");
    if (err != PDERR_NOERR) return err;
    err = ap_doc_write_ulong(writer, xref_offset);
    if (err != PDERR_NOERR) return err;
    return ap_doc_write_text(writer, "\n%%EOF\n");
}

static LONG ap_ps_end(struct APDocumentWriter *writer)
{
    static const UBYTE eod = 128U;
    LONG err;

    err = ap_doc_write(writer, &eod, 1UL);
    if (err != PDERR_NOERR) return err;
    return ap_doc_write_text(writer,
        "\nData closefile\ngrestore\nshowpage\n%%EOF\n");
}

LONG ap_document_end(struct APDocumentWriter *writer)
{
    if (writer == NULL || writer->write_func == NULL) return PDERR_CANCEL;
    return writer->format == AP_DOCUMENT_PDF
        ? ap_pdf_end(writer) : ap_ps_end(writer);
}
