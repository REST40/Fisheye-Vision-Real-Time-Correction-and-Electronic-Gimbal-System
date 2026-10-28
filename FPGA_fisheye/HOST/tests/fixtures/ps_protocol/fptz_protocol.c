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

/* Reflected IEEE CRC-32: identical v3 wire polynomial and initial/final XOR. */
static const uint32_t crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

uint32_t fptz_crc32(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ bytes[i]) & 255u];
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
