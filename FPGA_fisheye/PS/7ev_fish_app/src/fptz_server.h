#ifndef FPTZ_SERVER_H
#define FPTZ_SERVER_H

#include <stdint.h>

#include "fptz_protocol.h"
#include "fptz_lut_generate.h"
#include "fptz_lut.h"

#define FPTZ_ERR_HARDWARE_FAULT 0x0101u
#define FPTZ_FRAME_SLOTS 4u
#define FPTZ_INPUT_BITMAP_BYTES 8192u
#define FPTZ_OUTPUT_BITMAP_BYTES 4096u
/* Bounded egress: yield to RX and hardware pumps between small batches. */
#define FPTZ_VIDEO_TX_BURST 16u
#define FPTZ_VIDEO_TX_GAP_US 250u
#define FPTZ_VIDEO_TX_BUDGET_US 250u
#define FPTZ_VIDEO_TX_RETRY_US 100000u
#define FPTZ_SEND_AGAIN (-2)
#define FPTZ_FLOW_BYTES 280u
#define FPTZ_LATENCY_BYTES 160u
#define FPTZ_CORRECTOR_CLOCK_HZ 166664993u /* platform XSA corrector/aclk */

enum FptzSlotState {
    FPTZ_SLOT_FREE, FPTZ_SLOT_RECEIVING, FPTZ_SLOT_READY,
    FPTZ_SLOT_RUNNING, FPTZ_SLOT_OUTPUT, FPTZ_SLOT_WAIT_ACK
};

enum FptzReturnTestState {
    FPTZ_RETURN_STARTED = 1, FPTZ_RETURN_STOPPED = 2,
    FPTZ_RETURN_NO_SAMPLE = 3, FPTZ_RETURN_INVALID_STATE = 4
};

enum FptzCapacityKind {
    FPTZ_CAPACITY_KIND_INPUT = 1,
    FPTZ_CAPACITY_KIND_PIPELINE = 2
};

enum FptzCapacityState {
    FPTZ_CAPACITY_STARTED = 1, FPTZ_CAPACITY_PROGRESS = 2,
    FPTZ_CAPACITY_STOPPED = 3, FPTZ_CAPACITY_NO_SAMPLE = 4,
    FPTZ_CAPACITY_INVALID_STATE = 5
};

typedef struct FptzFrameSlot {
    uint8_t *input;
    uint8_t *rgb;
    uint8_t *rgb_rear;
    uint8_t *corrected;
    uint8_t *output;
    uint8_t *input_bitmap;
    uint8_t *output_bitmap;
    uint32_t input_len;
    FptzJpegPair jpeg_pair;
    uint8_t pair_valid, sources_decoded, corrected_valid;
    uint32_t output_len;
    uint32_t frame_id;
    uint32_t ptz_epoch;
    uint32_t capture_ts_ms;
    uint32_t output_crc32;
    uint32_t decode_us; /* Wall time from first START until both decoders finish. */
    uint32_t decode_front_us, decode_rear_us;
    uint32_t correct_us;
    uint32_t encode_us;
    uint32_t frame_cycles;
    uint32_t decode_wait_us, correct_wait_us, encode_wait_us;
    uint16_t tx_count;
    uint16_t tx_next;
    uint16_t tx_base;
    uint8_t state;
    uint8_t tx_credit_taken;
    uint8_t rx_ack_counter;
    uint64_t last_tx_us;
    uint64_t tx_retry_started_us;
    uint64_t first_rx_us;
    /* Board-local monotonic timestamps; never subtract Host capture_ts_ms. */
    uint64_t input_ready_us, admitted_us, output_ready_us, first_tx_us;
    uint64_t crc_done_us, output_prepared_us;
    uint32_t flow_generation;
    uint8_t latency_valid;
    uint64_t stage_times_us[8]; /* front/rear/correct/encode start,end; PS local */
    FptzRx rx;
} FptzFrameSlot;

enum FptzLatencyPart {
    FPTZ_LAT_RX, FPTZ_LAT_READY, FPTZ_LAT_PIPE,
    FPTZ_LAT_OUTQ, FPTZ_LAT_TX, FPTZ_LAT_TOTAL, FPTZ_LAT_PARTS
};
typedef struct {
    uint32_t completed, gaps, intervals, max_gap_us;
    uint64_t busy_us, idle_us, interval_us;
} FptzStageFlow;

typedef struct {
    uint64_t sum_us[FPTZ_LAT_PARTS];
    uint32_t max_us[FPTZ_LAT_PARTS];
    uint32_t last_us[FPTZ_LAT_PARTS];
    uint32_t samples, last_frame;
} FptzLatencyStats;

typedef struct {
    uint32_t decode_started;
    uint32_t decode_completed;
    uint32_t decode_failed;
    uint32_t decode_timeout;
    uint32_t correct_started;
    uint32_t correct_completed;
    uint32_t correct_failed;
    uint32_t correct_timeout;
    uint32_t encode_started;
    uint32_t encode_completed;
    uint32_t encode_failed;
    uint32_t encode_timeout;
    uint32_t decode_lens_started[2], decode_lens_completed[2];
    uint32_t decode_lens_failed[2], decode_lens_timeout[2];
    uint32_t decode_lens_last_us[2];
    FptzStageFlow flow[3]; /* Normal video only; observed at software polls. */
} FptzPipelineStats;

typedef struct {
    /* send: nonnegative accepted; FPTZ_SEND_AGAIN defers; other negatives fail. */
    int (*send)(void *ctx, uint16_t local_port, uint16_t remote_port,
                const uint8_t *packet, uint16_t length);
    /* start_frame: 0 admitted, positive defer, negative hardware/argument fault. */
    int (*start_frame)(void *ctx, FptzFrameSlot *slot,
                       const uint8_t *lut);
    int (*poll_frame)(void *ctx, FptzFrameSlot *slot);
    int (*lut_published)(void *ctx, const uint8_t *lut, uint32_t bytes);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
    void (*get_pipeline_stats)(void *ctx, FptzPipelineStats *stats);
    int (*start_capacity_stage)(void *ctx, uint8_t stage,
                                FptzFrameSlot *slot, const uint8_t *lut);
    int (*poll_capacity_stage)(void *ctx, uint8_t stage,
                               FptzFrameSlot *slot);
    void (*set_flow_active)(void *ctx, int active);
} FptzServerOps;

typedef struct {
    FptzFrameSlot *slots;
    uint8_t slot_count;
    uint8_t *lut_banks[2];
    const uint8_t *scene_bits;
    const uint8_t *rear_scene_bits;
    const uint8_t *scene_pair_bits;
    const uint8_t *rear_scene_pair_bits;
    FptzLutGen lut_gen;
    FptzLutCheck lut_check;
    FptzServerOps ops;
    uint32_t session_id;
    uint32_t last_hello_txn;
    uint8_t hello_request[8];
    uint32_t active_epoch;
    uint32_t last_ptz_txn;
    uint32_t last_stream_txn;
    uint32_t last_stream_epoch;
    uint8_t last_stream_request[4];
    uint8_t last_stream_response[4];
    uint32_t credit_seq;
    uint32_t host_credit_seq;
    uint64_t last_heartbeat_us;
    uint64_t lut_started_us;
    uint64_t lut_work_us;
    uint32_t lut_last_wall_us;
    uint32_t lut_last_work_us;
    uint32_t lut_last_anchor_us;
    uint64_t lut_generated_us, lut_validated_us;
    uint32_t lut_candidate_generate_us, lut_candidate_validate_us;
    /* Last committed pose metrics; a rejected candidate never changes these. */
    uint32_t lut_last_generate_us, lut_last_validate_us;
    uint32_t lut_last_wait_us, lut_last_switch_us;
    uint32_t active_lens_span[FPTZ_LENS_COUNT];
    int32_t active_pan_mdeg, active_tilt_mdeg;
    uint64_t next_video_tx_us;
    uint32_t pending_ptz_txn;
    int32_t pending_pan_mdeg;
    int32_t pending_tilt_mdeg;
    uint32_t pending_hfov_mdeg;
    uint8_t last_ptz_state;
    uint8_t last_ptz_reason;
    uint32_t last_frame_id;
    uint32_t last_input_complete;
    uint32_t input_complete_count;
    uint32_t input_complete_bytes;
    uint32_t last_output_sent;
    uint32_t output_complete_count;
    uint32_t output_complete_bytes;
    uint32_t input_incomplete_drop;
    uint32_t input_crc_drop;
    uint32_t stale_epoch_drop;
    uint32_t no_slot_drop;
    uint32_t ready_superseded_drop;
    uint32_t output_send_drop;
    uint32_t output_superseded_drop;
    FptzLatencyStats latency;
    uint32_t telemetry_attempts, telemetry_failures;
    uint8_t flow_active;
    uint32_t flow_generation, flow_crc_samples, flow_sent_samples;
    uint32_t flow_credit_events, flow_intervals;
    uint64_t flow_crc_us, flow_prepare_us, flow_first_wait_us, flow_tx_us;
    uint64_t flow_interval_us, flow_last_output_us;
    uint32_t flow_input_crc_samples;
    uint64_t flow_input_crc_us, flow_input_parse_us;
    uint16_t max_payload;
    uint16_t send_window;
    uint16_t host_free_slots;
    uint8_t active_bank;
    uint8_t active_lut_valid;
    uint8_t candidate_bank;
    uint8_t generating;
    uint8_t validating;
    uint8_t switch_pending;
    uint8_t paused;
    uint8_t source_mode;
    uint8_t streaming;
    uint8_t stop_diagnostics_pending; /* Accepted STOP/PAUSE: print outside steady media. */
    uint8_t fault;
    uint8_t *return_jpeg;
    uint32_t return_capacity;
    uint32_t return_len;
    uint32_t return_crc32;
    uint32_t return_test_id;
    uint32_t return_pending_txn;
    uint32_t return_last_txn;
    uint32_t return_stop_txn;
    uint32_t return_frames_started;
    uint32_t return_frames_completed;
    uint32_t return_bytes_completed;
    uint32_t return_send_fail;
    uint64_t return_started_us;
    uint16_t return_tx_count;
    uint16_t return_tx_next;
    uint64_t return_retry_started_us;
    uint8_t return_active;
    FptzFrameSlot *capacity_sample;
    uint32_t capacity_test_id;
    uint32_t capacity_pending_txn;
    uint32_t capacity_last_start_txn;
    uint32_t capacity_last_stop_txn;
    uint64_t capacity_started_us;
    uint64_t capacity_stage_started_us;
    uint32_t capacity_elapsed_ms[3];
    uint32_t capacity_attempts[3];
    uint32_t capacity_completed[3];
    uint32_t capacity_bytes[3];
    uint32_t capacity_failures[3];
    uint32_t capacity_timeouts[3];
    uint8_t capacity_kind;
    uint8_t capacity_stage;
    uint8_t capacity_active;
    uint8_t capacity_operation_active;
    uint8_t capacity_stopping;
    uint32_t capacity_stop_txn;
} FptzServer;

int fptz_server_init(FptzServer *server, FptzFrameSlot *slots,
                     uint8_t slot_count, uint8_t *lut_a, uint8_t *lut_b,
                     const uint8_t *scene_bits,
                     const FptzServerOps *ops);
int fptz_server_on_packet(FptzServer *server, uint16_t local_port,
                          const uint8_t *packet, uint16_t length);
void fptz_server_pump(FptzServer *server);
/* Unsolicited terminal error: reason bit0=timeout, bit1=rear decoder. */
int fptz_server_report_hardware_fault(FptzServer *server, uint8_t stage,
                                      uint8_t reason, uint32_t frame);
/* Yield networking before a later input can supersede a completed READY frame. */
int fptz_server_rx_needs_pump(const FptzServer *server);
int fptz_server_configure_return_test(FptzServer *server, uint8_t *storage,
                                      uint32_t capacity);
int fptz_server_cache_return_jpeg(FptzServer *server, const uint8_t *jpeg,
                                  uint32_t length);

#endif
