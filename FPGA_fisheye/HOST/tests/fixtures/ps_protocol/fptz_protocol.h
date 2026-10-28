#ifndef FPTZ_PROTOCOL_H
#define FPTZ_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FPTZ_HEADER_SIZE 48u
#define FPTZ_MAX_PAYLOAD 1400u
#define FPTZ_CONTROL_PORT 5500u
#define FPTZ_INPUT_PORT 5501u
#define FPTZ_OUTPUT_PORT 5502u
#define FPTZ_MAX_JPEG_BYTES (8u * 1024u * 1024u)
#define FPTZ_PAIR_HEADER_BYTES 64u
#define FPTZ_ALIGN64(n) (((uint32_t)(n) + 63u) & ~63u)
#define FPTZ_PAIR_JPEG_BLOCK(n) FPTZ_ALIGN64((uint32_t)(n) + 64u)
#define FPTZ_MAX_INPUT_BYTES (FPTZ_PAIR_HEADER_BYTES + 2u * FPTZ_PAIR_JPEG_BLOCK(FPTZ_MAX_JPEG_BYTES))
#define FPTZ_PAIR_CAPABILITY 1u
#include "fptz_geometry.h"

enum {
    FPTZ_HELLO = 0x01, FPTZ_HELLO_ACK = 0x02,
    FPTZ_SET_PTZ = 0x03, FPTZ_PTZ_STATUS = 0x04,
    FPTZ_STREAM_CTRL = 0x05, FPTZ_STREAM_STATUS = 0x06,
    FPTZ_MEDIA_ACK = 0x07, FPTZ_FRAME_RESULT = 0x08,
    FPTZ_CREDIT = 0x09, FPTZ_HEARTBEAT = 0x0a,
    FPTZ_ERROR = 0x0b, FPTZ_LUT_PREPARE = 0x0c,
    FPTZ_LUT_STATUS = 0x0d, FPTZ_RETURN_TEST_CTRL = 0x0e,
    FPTZ_RETURN_TEST_STATUS = 0x0f,
    FPTZ_CAPACITY_TEST_CTRL = 0x10, FPTZ_CAPACITY_TEST_STATUS = 0x11,
    FPTZ_INPUT_JPEG = 0x20,
    FPTZ_OUTPUT_JPEG = 0x21, FPTZ_LUT_DATA = 0x22, FPTZ_INPUT_JPEG_PAIR = 0x23
};

typedef struct {
    uint8_t msg_type;
    uint16_t payload_len;
    uint32_t session_id;
    uint32_t txn_id;
    uint32_t frame_id;
    uint32_t ptz_epoch;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint32_t object_len;
    uint32_t object_crc32;
    uint32_t capture_ts_ms;
} FptzHeader;

typedef struct {
    uint32_t offset[2], length[2];
} FptzJpegPair;
/* Exact zero-padded container and two complete Baseline 4:2:0 images.
 * Offsets are relative to storage; no image copy is performed. */
int fptz_jpeg_pair_parse(const uint8_t *storage, size_t bytes, FptzJpegPair *pair);
uint32_t fptz_input_object_limit(uint16_t payload);
typedef struct {
    FptzHeader object;
    uint8_t *storage;
    uint8_t *bitmap;
    uint32_t capacity;
    uint32_t bitmap_bytes;
    uint16_t max_payload;
    uint16_t received_count;
} FptzRx;

uint16_t fptz_get16(const uint8_t *data);
uint32_t fptz_get32(const uint8_t *data);
void fptz_put16(uint8_t *data, uint16_t value);
void fptz_put32(uint8_t *data, uint32_t value);
uint32_t fptz_crc32(const void *data, size_t length);
int fptz_parse_packet(const uint8_t *packet, size_t length, FptzHeader *header);
void fptz_write_header(uint8_t *packet, const FptzHeader *header);
int fptz_rx_begin(FptzRx *receiver, uint8_t *storage, uint32_t capacity,
                  uint8_t *bitmap, uint32_t bitmap_bytes, uint16_t max_payload,
                  const FptzHeader *first);
int fptz_rx_push(FptzRx *receiver, const FptzHeader *header,
                 const uint8_t *payload);
int fptz_rx_verify(const FptzRx *receiver);
void fptz_rx_sack(const FptzRx *receiver, uint16_t *base, uint64_t *mask);
/* Input is strictly 1328x1328 Baseline JPEG 4:2:0. */
int fptz_jpeg_is_source_baseline(const uint8_t *data, size_t length);
/* Retained for output JPEG validation, not input admission. */
int fptz_jpeg_is_1080p_baseline(const uint8_t *data, size_t length);

#endif
