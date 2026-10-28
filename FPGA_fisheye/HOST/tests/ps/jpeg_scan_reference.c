/* Preserved bytewise validator before block scanning; differential oracle. */
#include "fptz_protocol.h"

static int jpeg_is_baseline_geometry(const uint8_t *data, size_t length,
                                     uint16_t width, uint16_t height,
                                     int require_420) {
    size_t pos = 2;
    int sof = 0, dqt = 0, dht = 0;
    if (!data || length < 4 || data[0] != 0xff || data[1] != 0xd8 ||
        data[length - 2] != 0xff || data[length - 1] != 0xd9) return 0;
    while (pos + 4 <= length) {
        uint8_t marker;
        uint16_t segment;
        if (data[pos++] != 0xff) return 0;
        while (pos < length && data[pos] == 0xff) ++pos;
        if (pos + 3 > length) return 0;
        marker = data[pos++];
        segment = fptz_get16(data + pos);
        if (segment < 2 || segment > length - pos) return 0;
        if (marker == 0xc0) {
            if (sof || segment != 17 || data[pos + 2] != 8 ||
                fptz_get16(data + pos + 3) != height ||
                fptz_get16(data + pos + 5) != width || data[pos + 7] != 3 ||
                data[pos + 8] != 1 || data[pos + 11] != 2 ||
                data[pos + 14] != 3 ||
                (data[pos + 9] != 0x22 && (require_420 || data[pos + 9] != 0x11)) ||
                data[pos + 12] != 0x11 || data[pos + 15] != 0x11)
                return 0;
            sof = 1;
        } else if (marker == 0xdb) dqt = 1;
        else if (marker == 0xc4) dht = 1;
        else if (marker == 0xda) {
            if (segment != 12 || data[pos + 2] != 3 ||
                data[pos + 3] != 1 || data[pos + 5] != 2 ||
                data[pos + 7] != 3 || data[pos + 9] != 0 ||
                data[pos + 10] != 63 || data[pos + 11] != 0) return 0;
            if (!(sof && dqt && dht)) return 0;
            if (!require_420) return 1;
            /* Source decoder supports one interleaved scan, no restarts.
             * Reject RST even when the sender omitted the DRI segment. */
            for (size_t scan = pos + segment; scan + 1u < length; ) {
                if (data[scan] != 0xff) { ++scan; continue; }
                if (data[scan + 1u] == 0) { scan += 2u; continue; }
                if (data[scan + 1u] == 0xff) { ++scan; continue; }
                return data[scan + 1u] == 0xd9 && scan == length - 2u;
            }
            return 0;
        } else if (marker != 0xfe && (marker < 0xe0 || marker > 0xef))
            return 0;
        pos += segment;
    }
    return 0;
}

int fptz_jpeg_is_source_baseline_reference(const uint8_t *data, size_t length) {
    return jpeg_is_baseline_geometry(data, length, FPTZ_SOURCE_WIDTH,
                                     FPTZ_SOURCE_HEIGHT, 1);
}
int fptz_jpeg_is_1080p_baseline_reference(const uint8_t *data, size_t length) {
    return jpeg_is_baseline_geometry(data, length, FPTZ_OUTPUT_WIDTH,
                                     FPTZ_OUTPUT_HEIGHT, 0);
}

uint32_t fptz_input_object_limit_reference(uint16_t payload) {
    uint32_t wire_limit;
    if (payload < 256u || payload > FPTZ_MAX_PAYLOAD) return 0;
    wire_limit = (uint32_t)payload * 65535u;
    return wire_limit < FPTZ_MAX_INPUT_BYTES ? wire_limit : FPTZ_MAX_INPUT_BYTES;
}
int fptz_jpeg_pair_parse_reference(const uint8_t *storage, size_t bytes, FptzJpegPair *pair) {
    FptzJpegPair result;
    uint32_t blocks[2], total;
    if (!storage || !pair || bytes < FPTZ_PAIR_HEADER_BYTES || bytes > FPTZ_MAX_INPUT_BYTES)
        return -1;
    result.length[0] = fptz_get32(storage); result.length[1] = fptz_get32(storage + 4u);
    for (unsigned j = 0; j < 2; ++j) {
        if (result.length[j] < 4u || result.length[j] > FPTZ_MAX_JPEG_BYTES) return -1;
        blocks[j] = FPTZ_PAIR_JPEG_BLOCK(result.length[j]);
    }
    result.offset[0] = FPTZ_PAIR_HEADER_BYTES;
    result.offset[1] = result.offset[0] + blocks[0];
    total = result.offset[1] + blocks[1];
    if (bytes != total) return -1;
    for (unsigned i = 8; i < FPTZ_PAIR_HEADER_BYTES; ++i)
        if (storage[i]) return -1;
    for (unsigned j = 0; j < 2; ++j) {
        const uint8_t *jpeg = storage + result.offset[j];
        if (!fptz_jpeg_is_source_baseline_reference(jpeg, result.length[j])) return -1;
        for (uint32_t i = result.length[j]; i < blocks[j]; ++i)
            if (jpeg[i]) return -1;
    }
    *pair = result;
    return 0;
}
