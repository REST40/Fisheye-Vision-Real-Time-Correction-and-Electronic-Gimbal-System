#include "fptz_protocol.h"

#include <string.h>

uint16_t fptz_get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint32_t fptz_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void fptz_put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

void fptz_put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* Same reflected IEEE CRC-32 as v3, processed eight bytes at a time.
 * Immutable tables avoid initialization/races; byte loads support every alignment
 * and host endianness, and the tail never reads beyond the supplied object. */
#include "fptz_crc32_tables.h"

uint32_t fptz_crc32(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    while (length >= 8u) {
        uint32_t low = crc ^ ((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
        crc = fptz_crc32_tables[7][low & 255u] ^
              fptz_crc32_tables[6][(low >> 8) & 255u] ^
              fptz_crc32_tables[5][(low >> 16) & 255u] ^
              fptz_crc32_tables[4][low >> 24] ^
              fptz_crc32_tables[3][bytes[4]] ^ fptz_crc32_tables[2][bytes[5]] ^
              fptz_crc32_tables[1][bytes[6]] ^ fptz_crc32_tables[0][bytes[7]];
        bytes += 8u;
        length -= 8u;
    }
    while (length--) crc = (crc >> 8) ^ fptz_crc32_tables[0][(crc ^ *bytes++) & 255u];
    return crc ^ 0xffffffffu;
}

static int known_type(uint8_t type) {
    return (type >= FPTZ_HELLO && type <= FPTZ_CAPACITY_TEST_STATUS) ||
           (type >= FPTZ_INPUT_JPEG && type <= FPTZ_INPUT_JPEG_PAIR);
}

int fptz_parse_packet(const uint8_t *packet, size_t length, FptzHeader *header) {
    if (!packet || !header || length < FPTZ_HEADER_SIZE ||
        memcmp(packet, "FPTZ", 4) || packet[4] != 3 ||
        !known_type(packet[5]) || fptz_get16(packet + 6) != FPTZ_HEADER_SIZE ||
        fptz_get16(packet + 8) != 0 || fptz_get32(packet + 44) != 0)
        return -1;
    header->msg_type = packet[5];
    header->payload_len = fptz_get16(packet + 10);
    header->session_id = fptz_get32(packet + 12);
    header->txn_id = fptz_get32(packet + 16);
    header->frame_id = fptz_get32(packet + 20);
    header->ptz_epoch = fptz_get32(packet + 24);
    header->chunk_index = fptz_get16(packet + 28);
    header->chunk_count = fptz_get16(packet + 30);
    header->object_len = fptz_get32(packet + 32);
    header->object_crc32 = fptz_get32(packet + 36);
    header->capture_ts_ms = fptz_get32(packet + 40);
    if (header->payload_len > FPTZ_MAX_PAYLOAD ||
        length != FPTZ_HEADER_SIZE + header->payload_len ||
        header->chunk_count == 0 || header->chunk_index >= header->chunk_count)
        return -1;
    if (header->msg_type < FPTZ_INPUT_JPEG &&
        (header->chunk_index != 0 || header->chunk_count != 1 ||
         header->object_len != header->payload_len ||
         fptz_crc32(packet + FPTZ_HEADER_SIZE, header->payload_len) !=
             header->object_crc32))
        return -1;
    return 0;
}

void fptz_write_header(uint8_t *packet, const FptzHeader *header) {
    memcpy(packet, "FPTZ", 4);
    packet[4] = 3;
    packet[5] = header->msg_type;
    fptz_put16(packet + 6, FPTZ_HEADER_SIZE);
    fptz_put16(packet + 8, 0);
    fptz_put16(packet + 10, header->payload_len);
    fptz_put32(packet + 12, header->session_id);
    fptz_put32(packet + 16, header->txn_id);
    fptz_put32(packet + 20, header->frame_id);
    fptz_put32(packet + 24, header->ptz_epoch);
    fptz_put16(packet + 28, header->chunk_index);
    fptz_put16(packet + 30, header->chunk_count);
    fptz_put32(packet + 32, header->object_len);
    fptz_put32(packet + 36, header->object_crc32);
    fptz_put32(packet + 40, header->capture_ts_ms);
    fptz_put32(packet + 44, 0);
}

int fptz_rx_begin(FptzRx *rx, uint8_t *storage, uint32_t capacity,
                  uint8_t *bitmap, uint32_t bitmap_bytes, uint16_t max_payload,
                  const FptzHeader *first) {
    uint32_t expected;
    if (!rx || !storage || !bitmap || !first || !max_payload ||
        max_payload > FPTZ_MAX_PAYLOAD || !first->object_len ||
        first->object_len > capacity || !first->chunk_count)
        return -1;
    expected = (first->object_len - 1u) / max_payload + 1u;
    if (expected > 65535u || expected != first->chunk_count ||
        bitmap_bytes < (expected + 7u) / 8u) return -1;
    memset(rx, 0, sizeof(*rx));
    rx->object = *first;
    rx->storage = storage;
    rx->bitmap = bitmap;
    rx->capacity = capacity;
    rx->bitmap_bytes = bitmap_bytes;
    rx->max_payload = max_payload;
    memset(bitmap, 0, bitmap_bytes);
    return 0;
}

int fptz_rx_push(FptzRx *rx, const FptzHeader *header,
                 const uint8_t *payload) {
    uint32_t offset, expected_length;
    uint8_t mask;
    if (!rx || !header || !payload ||
        header->msg_type != rx->object.msg_type ||
        header->session_id != rx->object.session_id ||
        header->frame_id != rx->object.frame_id ||
        header->txn_id != rx->object.txn_id ||
        header->capture_ts_ms != rx->object.capture_ts_ms ||
        header->ptz_epoch != rx->object.ptz_epoch ||
        header->chunk_count != rx->object.chunk_count ||
        header->object_len != rx->object.object_len ||
        header->object_crc32 != rx->object.object_crc32 ||
        header->chunk_index >= rx->object.chunk_count)
        return -1;
    offset = (uint32_t)header->chunk_index * rx->max_payload;
    expected_length = rx->object.object_len - offset;
    if (expected_length > rx->max_payload) expected_length = rx->max_payload;
    if (header->payload_len != expected_length) return -1;
    mask = (uint8_t)(1u << (header->chunk_index & 7u));
    if (rx->bitmap[header->chunk_index >> 3] & mask) {
        return memcmp(rx->storage + offset, payload, expected_length) ? -1 : 0;
    }
    memcpy(rx->storage + offset, payload, expected_length);
    rx->bitmap[header->chunk_index >> 3] |= mask;
    ++rx->received_count;
    return rx->received_count == rx->object.chunk_count ? 1 : 0;
}

int fptz_rx_verify(const FptzRx *rx) {
    if (!rx || rx->received_count != rx->object.chunk_count) return -1;
    return fptz_crc32(rx->storage, rx->object.object_len) ==
           rx->object.object_crc32 ? 0 : -1;
}

void fptz_rx_sack(const FptzRx *rx, uint16_t *base, uint64_t *mask) {
    uint32_t first = 0;
    uint64_t bits = 0;
    while (first < rx->object.chunk_count &&
           (rx->bitmap[first >> 3] & (1u << (first & 7u)))) ++first;
    for (uint32_t i = 0; i < 64 && first + i < rx->object.chunk_count; ++i)
        if (rx->bitmap[(first + i) >> 3] & (1u << ((first + i) & 7u)))
            bits |= (uint64_t)1 << i;
    *base = (uint16_t)first;
    *mask = bits;
}

/* Find the next FF without reading outside the supplied entropy interval.
 * Prefix/tail use byte reads; aligned words skip eight ordinary bytes at once.
 * The zero-byte test on ~word is endian-independent. memcpy avoids aliasing. */
static const uint8_t *jpeg_find_ff(const uint8_t *data, size_t length) {
    while (length && ((uintptr_t)data & 7u)) {
        if (*data == 0xff) return data;
        ++data; --length;
    }
    while (length >= 8u) {
        uint64_t word;
        memcpy(&word, data, sizeof(word));
        uint64_t inverse = ~word;
        if ((inverse - UINT64_C(0x0101010101010101)) &
            ~inverse & UINT64_C(0x8080808080808080)) break;
        data += 8u; length -= 8u;
    }
    while (length--) {
        if (*data == 0xff) return data;
        ++data;
    }
    return NULL;
}

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
                const uint8_t *marker = jpeg_find_ff(data + scan, length - scan - 1u);
                if (!marker) return 0;
                scan = (size_t)(marker - data);
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

int fptz_jpeg_is_source_baseline(const uint8_t *data, size_t length) {
    return jpeg_is_baseline_geometry(data, length, FPTZ_SOURCE_WIDTH,
                                     FPTZ_SOURCE_HEIGHT, 1);
}
int fptz_jpeg_is_1080p_baseline(const uint8_t *data, size_t length) {
    return jpeg_is_baseline_geometry(data, length, FPTZ_OUTPUT_WIDTH,
                                     FPTZ_OUTPUT_HEIGHT, 0);
}

uint32_t fptz_input_object_limit(uint16_t payload) {
    uint32_t wire_limit;
    if (payload < 256u || payload > FPTZ_MAX_PAYLOAD) return 0;
    wire_limit = (uint32_t)payload * 65535u;
    return wire_limit < FPTZ_MAX_INPUT_BYTES ? wire_limit : FPTZ_MAX_INPUT_BYTES;
}
int fptz_jpeg_pair_parse(const uint8_t *storage, size_t bytes, FptzJpegPair *pair) {
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
        if (!fptz_jpeg_is_source_baseline(jpeg, result.length[j])) return -1;
        for (uint32_t i = result.length[j]; i < blocks[j]; ++i)
            if (jpeg[i]) return -1;
    }
    *pair = result;
    return 0;
}
