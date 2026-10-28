#include "fptz_server.h"
#include "fptz_scheduler.h"

#include <string.h>

enum {
    PTZ_ACCEPTED = 1, PTZ_READY = 2, PTZ_ACTIVE = 3, PTZ_REJECTED = 4
};
enum {
    PTZ_REASON_NONE = 0, PTZ_REASON_INVALID = 1,
    PTZ_REASON_OLD_TXN = 2, PTZ_REASON_SUPERSEDED = 3,
    PTZ_REASON_UNSAFE = 4, PTZ_REASON_COMPUTE = 5
};

static void send_input_ack(FptzServer *s,const FptzFrameSlot *slot,uint8_t state);

static int send_control(FptzServer *s, uint8_t type, uint32_t txn,
                        uint32_t frame, uint32_t epoch,
                        const uint8_t *body, uint16_t length) {
    uint8_t packet[FPTZ_HEADER_SIZE + FPTZ_FLOW_BYTES] = {0};
    FptzHeader h = {0};
    if (length > FPTZ_FLOW_BYTES) return -1;
    h.msg_type = type;
    h.payload_len = length;
    h.session_id = s->session_id;
    h.txn_id = txn;
    h.frame_id = frame;
    h.ptz_epoch = epoch;
    h.chunk_count = 1;
    h.object_len = length;
    h.object_crc32 = fptz_crc32(body, length);
    if (length) memcpy(packet + FPTZ_HEADER_SIZE, body, length);
    fptz_write_header(packet, &h);
    return s->ops.send(s->ops.ctx, FPTZ_CONTROL_PORT,
                       FPTZ_CONTROL_PORT, packet,
                       (uint16_t)(FPTZ_HEADER_SIZE + length));
}

static int send_ptz_status(FptzServer *s, uint32_t txn,
                           uint8_t state, uint8_t reason) {
    uint8_t body[20] = {0};
    body[0] = state;
    body[1] = reason;
    fptz_put32(body + 4, s->active_epoch);
    if (state == PTZ_READY) {
        fptz_put32(body + 8, s->lut_last_wall_us);
        fptz_put32(body + 12, s->lut_last_work_us);
        fptz_put32(body + 16, s->lut_last_anchor_us);
    }
    return send_control(s, FPTZ_PTZ_STATUS, txn, 0,
                        s->active_epoch, body, state == PTZ_READY ? 20u : 8u);
}

static uint32_t saturate_us(uint64_t us) {
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint16_t free_slots(const FptzServer *s) {
    uint16_t free_count = 0;
    for (uint8_t i = 0; i < s->slot_count; ++i)
        if (s->slots[i].state == FPTZ_SLOT_FREE) ++free_count;
    return free_count;
}

static void send_credit(FptzServer *s) {
    uint8_t body[12] = {0};
    fptz_put16(body, free_slots(s));
    fptz_put16(body + 2, s->slot_count);
    fptz_put16(body + 4, (uint16_t)(s->slot_count - free_slots(s)));
    fptz_put32(body + 8, ++s->credit_seq);
    int result=send_control(s, FPTZ_CREDIT, 0, 0, s->active_epoch, body, sizeof(body));
    if (result>=0 && s->flow_active && free_slots(s)) ++s->flow_credit_events;
}

static void put64(uint8_t *p,uint64_t value) {
    fptz_put32(p,(uint32_t)(value>>32)); fptz_put32(p+4,(uint32_t)value);
}
static void update_flow_mode(FptzServer *s) {
    int active=s->session_id && s->streaming && !s->paused && !s->fault &&
        !s->capacity_active && !s->capacity_pending_txn && !s->return_active;
    if (active==s->flow_active) return;
    s->flow_active=(uint8_t)active; ++s->flow_generation;
    s->flow_last_output_us=0;
    if (s->ops.set_flow_active) s->ops.set_flow_active(s->ops.ctx,active);
}
static int send_flow_stats(FptzServer *s,uint32_t txn,const FptzPipelineStats *stats) {
    uint8_t b[FPTZ_FLOW_BYTES]={0};
    fptz_put32(b,0x464c4f57u); fptz_put32(b+4,2);
    fptz_put32(b+8,s->flow_active); fptz_put32(b+12,free_slots(s));
    put64(b+16,s->ops.now_us(s->ops.ctx));
    fptz_put32(b+24,s->flow_crc_samples); fptz_put32(b+28,s->flow_credit_events);
    put64(b+32,s->flow_crc_us); put64(b+40,s->flow_prepare_us);
    fptz_put32(b+48,s->flow_sent_samples); fptz_put32(b+52,s->flow_intervals);
    put64(b+56,s->flow_first_wait_us); put64(b+64,s->flow_tx_us);
    put64(b+72,s->flow_interval_us);
    const uint32_t drops[6]={s->ready_superseded_drop,s->output_superseded_drop,
        s->output_send_drop,s->input_incomplete_drop,s->input_crc_drop,s->no_slot_drop};
    for (unsigned i=0;i<6;++i) fptz_put32(b+80+4*i,drops[i]);
    for (uint8_t i=0;i<s->slot_count;++i) {
        unsigned state=s->slots[i].state;
        if (state>=FPTZ_SLOT_RECEIVING && state<=FPTZ_SLOT_OUTPUT) {
            unsigned offset=104+4*(state-FPTZ_SLOT_RECEIVING);
            fptz_put32(b+offset,fptz_get32(b+offset)+1);
        }
    }
    fptz_put32(b+120,s->flow_generation);
    for (unsigned i=0;i<3;++i) {
        unsigned o=128+40*i; const FptzStageFlow *f=&stats->flow[i];
        fptz_put32(b+o,f->completed); fptz_put32(b+o+4,f->gaps);
        put64(b+o+8,f->busy_us); put64(b+o+16,f->idle_us);
        put64(b+o+24,f->interval_us); fptz_put32(b+o+32,f->intervals);
        fptz_put32(b+o+36,f->max_gap_us);
    }
    fptz_put32(b+256,s->flow_input_crc_samples);
    put64(b+264,s->flow_input_crc_us); put64(b+272,s->flow_input_parse_us);
    return send_control(s,FPTZ_HEARTBEAT,txn,0,s->active_epoch,b,sizeof(b));
}
static int send_stats(FptzServer *s, uint32_t txn) {
    update_flow_mode(s);
    uint8_t body[152] = {0};
    FptzPipelineStats stats = {0};
    fptz_put32(body, s->last_input_complete);
    fptz_put32(body + 4, s->last_output_sent);
    fptz_put32(body + 8, s->input_incomplete_drop);
    fptz_put32(body + 12, s->input_crc_drop);
    fptz_put32(body + 16, s->stale_epoch_drop);
    fptz_put32(body + 20, s->no_slot_drop);
    fptz_put32(body + 24, s->input_complete_count);
    fptz_put32(body + 28, s->input_complete_bytes);
    if (s->ops.get_pipeline_stats)
        s->ops.get_pipeline_stats(s->ops.ctx, &stats);
    fptz_put32(body + 32, stats.decode_started);
    fptz_put32(body + 36, stats.decode_completed);
    fptz_put32(body + 40, stats.decode_failed);
    fptz_put32(body + 44, stats.decode_timeout);
    fptz_put32(body + 48, stats.correct_started);
    fptz_put32(body + 52, stats.correct_completed);
    fptz_put32(body + 56, stats.correct_failed);
    fptz_put32(body + 60, stats.correct_timeout);
    fptz_put32(body + 64, stats.encode_started);
    fptz_put32(body + 68, stats.encode_completed);
    fptz_put32(body + 72, stats.encode_failed);
    fptz_put32(body + 76, stats.encode_timeout);
    fptz_put32(body + 80, s->output_complete_count);
    fptz_put32(body + 84, s->output_complete_bytes);
    fptz_put32(body + 88, s->output_superseded_drop);
    fptz_put32(body + 92, s->output_send_drop);
    /* Optional WAIT extension; the original 96-byte heartbeat prefix is unchanged.
     * All intervals use the PS monotonic clock and completed normal-video frames. */
    fptz_put32(body + 96, 0x57414954u);
    fptz_put32(body + 100, s->latency.samples);
    for (unsigned i = 0; i < FPTZ_LAT_PARTS; ++i) {
        fptz_put32(body + 104 + i * 8, (uint32_t)(s->latency.sum_us[i] >> 32));
        fptz_put32(body + 108 + i * 8, (uint32_t)s->latency.sum_us[i]);
    }
    /* Keep the original heartbeat for hosts that accept only the legacy sizes.
     * Updated hosts also consume the following extended heartbeat. */
    int sent = send_control(s, FPTZ_HEARTBEAT, txn, 0, s->active_epoch, body, 96u);
    if (sent < 0) return sent;
    sent=send_control(s,FPTZ_HEARTBEAT,txn,0,s->active_epoch,body,sizeof(body));
    if (sent<0) return sent;
    return send_flow_stats(s,txn,&stats);
}

int fptz_server_report_hardware_fault(FptzServer *s, uint8_t stage,
                                      uint8_t reason, uint32_t frame) {
    uint8_t body[8] = {0};
    if (!s || !s->session_id || !stage || stage > 3u) return -1;
    s->fault = 1;
    fptz_put16(body, FPTZ_ERR_HARDWARE_FAULT);
    body[2] = stage; body[3] = reason;
    fptz_put32(body + 4, frame);
    return send_control(s, FPTZ_ERROR, 0, frame, s->active_epoch, body, sizeof(body));
}

static int send_error(FptzServer *s, const FptzHeader *request,
                      uint16_t code, uint8_t stage) {
    uint8_t body[8] = {0};
    fptz_put16(body, code);
    body[2] = stage;
    (void)send_control(s, FPTZ_ERROR, request->txn_id, request->frame_id,
                       request->ptz_epoch, body, sizeof(body));
    return -1;
}

static int any_active_frame(const FptzServer *s) {
    for (uint8_t i = 0; i < s->slot_count; ++i)
        if (s->slots[i].state != FPTZ_SLOT_FREE) return 1;
    return 0;
}

int fptz_server_configure_return_test(FptzServer *s, uint8_t *storage,
                                      uint32_t capacity) {
    if (!s || !storage || capacity < 4u || capacity > FPTZ_MAX_JPEG_BYTES)
        return -1;
    s->return_jpeg = storage;
    s->return_capacity = capacity;
    return 0;
}

int fptz_server_cache_return_jpeg(FptzServer *s, const uint8_t *jpeg,
                                  uint32_t length) {
    if (!s || !s->return_jpeg || !jpeg || length < 4u ||
        length > s->return_capacity || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[length - 2u] != 0xff || jpeg[length - 1u] != 0xd9) return -1;
    memcpy(s->return_jpeg, jpeg, length);
    s->return_len = length;
    s->return_crc32 = fptz_crc32(jpeg, length);
    return 0;
}

static int send_return_status(FptzServer *s, uint32_t txn, uint8_t action,
                              uint8_t state) {
    uint8_t body[32] = {0};
    uint64_t elapsed = s->return_started_us
        ? s->ops.now_us(s->ops.ctx) - s->return_started_us : 0;
    body[0] = action; body[1] = state;
    fptz_put32(body + 4, s->return_test_id);
    fptz_put32(body + 8, (uint32_t)(elapsed / 1000u));
    fptz_put32(body + 12, s->return_frames_started);
    fptz_put32(body + 16, s->return_frames_completed);
    fptz_put32(body + 20, s->return_bytes_completed);
    fptz_put32(body + 24, s->return_send_fail);
    fptz_put32(body + 28, s->return_len);
    return send_control(s, FPTZ_RETURN_TEST_STATUS, txn, 0,
                        s->active_epoch, body, sizeof(body));
}

static void start_return_test(FptzServer *s, uint32_t txn) {
    ++s->return_test_id; if (!s->return_test_id) ++s->return_test_id;
    s->return_frames_started = s->return_frames_completed = 0;
    s->return_bytes_completed = s->return_send_fail = 0;
    s->return_tx_count = (uint16_t)((s->return_len + s->max_payload - 1u) /
                                    s->max_payload);
    s->return_tx_next = 0;
    s->return_started_us = s->ops.now_us(s->ops.ctx);
    s->return_active = 1;
    s->return_last_txn = txn;
    s->return_pending_txn = 0;
    (void)send_return_status(s, txn, 1, FPTZ_RETURN_STARTED);
}

static int handle_return_test(FptzServer *s, const FptzHeader *h,
                              const uint8_t *body) {
    if (h->payload_len != 4 || !h->txn_id || body[1] || body[2] || body[3] ||
        (body[0] != 1 && body[0] != 2)) return send_error(s, h, 2, 1);
    if (body[0] == 1) {
        if (s->return_active && h->txn_id == s->return_last_txn)
            return send_return_status(s, h->txn_id, 1, FPTZ_RETURN_STARTED);
        if (!s->return_len)
            return send_return_status(s, h->txn_id, 1, FPTZ_RETURN_NO_SAMPLE);
        if (!s->streaming || !s->paused || s->return_active || s->capacity_active ||
            s->generating || s->validating || s->switch_pending)
            return send_return_status(s, h->txn_id, 1,
                                      FPTZ_RETURN_INVALID_STATE);
        if (any_active_frame(s)) { s->return_pending_txn = h->txn_id; return 0; }
        start_return_test(s, h->txn_id);
        return 0;
    }
    /* STOP also cancels a pending start and is idempotent after resume
     * implicitly stops the cached return stream. Preserve its final counters. */
    s->return_active = 0;
    s->return_pending_txn = 0;
    s->return_stop_txn = h->txn_id;
    return send_return_status(s, h->txn_id, 2, FPTZ_RETURN_STOPPED);
}

static int send_capacity_status(FptzServer *s, uint32_t txn, uint8_t action,
                                uint8_t state) {
    uint8_t body[84] = {0};
    uint64_t now = s->ops.now_us(s->ops.ctx);
    body[0] = action; body[1] = state; body[2] = s->capacity_kind;
    body[3] = s->capacity_stage;
    fptz_put32(body + 4, s->capacity_test_id);
    fptz_put32(body + 8, s->capacity_started_us
        ? (uint32_t)((now - s->capacity_started_us) / 1000u) : 0u);
    for (uint8_t i = 0; i < 3; ++i) {
        uint32_t elapsed = s->capacity_elapsed_ms[i];
        if (s->capacity_active && s->capacity_kind == FPTZ_CAPACITY_KIND_PIPELINE &&
            s->capacity_stage == i + 1u)
            elapsed = (uint32_t)((now - s->capacity_stage_started_us) / 1000u);
        if (s->capacity_active && s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT && !i)
            elapsed = (uint32_t)((now - s->capacity_stage_started_us) / 1000u);
        fptz_put32(body + 12u + i * 24u, elapsed);
        fptz_put32(body + 16u + i * 24u, s->capacity_attempts[i]);
        fptz_put32(body + 20u + i * 24u, s->capacity_completed[i]);
        fptz_put32(body + 24u + i * 24u, s->capacity_bytes[i]);
        fptz_put32(body + 28u + i * 24u, s->capacity_failures[i]);
        fptz_put32(body + 32u + i * 24u, s->capacity_timeouts[i]);
    }
    return send_control(s, FPTZ_CAPACITY_TEST_STATUS, txn, 0,
                        s->active_epoch, body, sizeof(body));
}

static void finish_capacity_test(FptzServer *s, uint32_t txn) {
    uint64_t now = s->ops.now_us(s->ops.ctx);
    uint8_t index = s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT ? 0u :
                    (uint8_t)(s->capacity_stage - 1u);
    if (!s->capacity_elapsed_ms[index])
        s->capacity_elapsed_ms[index] =
            (uint32_t)((now - s->capacity_stage_started_us) / 1000u);
    if (s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT) {
        for (uint8_t i = 0; i < s->slot_count; ++i) {
            FptzFrameSlot *slot = &s->slots[i];
            if (slot->state == FPTZ_SLOT_RECEIVING) {
                ++s->input_incomplete_drop;
                ++s->capacity_timeouts[0];
                send_input_ack(s,slot,2u);
                slot->state = FPTZ_SLOT_FREE;
            }
        }
    }
    s->capacity_active = 0;
    s->capacity_operation_active = 0;
    s->capacity_stopping=0;s->capacity_stop_txn=0;
    s->capacity_last_stop_txn = txn;
    (void)send_capacity_status(s, txn, 2, FPTZ_CAPACITY_STOPPED);
    if (s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT) send_credit(s);
}

static void request_capacity_stop(FptzServer *s,uint32_t txn) {
    if (!s->capacity_active) return;
    if (!txn) txn=++s->capacity_last_stop_txn;
    if (s->capacity_operation_active) {
        s->capacity_stopping=1;s->capacity_stop_txn=txn;
    } else finish_capacity_test(s,txn);
}

static int start_capacity_test(FptzServer *s, uint32_t txn) {
    memset(s->capacity_elapsed_ms, 0, sizeof(s->capacity_elapsed_ms));
    memset(s->capacity_attempts, 0, sizeof(s->capacity_attempts));
    memset(s->capacity_completed, 0, sizeof(s->capacity_completed));
    memset(s->capacity_bytes, 0, sizeof(s->capacity_bytes));
    memset(s->capacity_failures, 0, sizeof(s->capacity_failures));
    memset(s->capacity_timeouts, 0, sizeof(s->capacity_timeouts));
    s->capacity_stage = s->capacity_kind == FPTZ_CAPACITY_KIND_PIPELINE ? 1u : 0u;
    s->capacity_last_start_txn = txn;
    s->capacity_pending_txn = 0;
    s->capacity_started_us = s->capacity_stage_started_us =
        s->ops.now_us(s->ops.ctx);
    s->capacity_operation_active = 0;
    s->capacity_active = 1;
    s->capacity_stopping=0;s->capacity_stop_txn=0;
    return send_capacity_status(s, txn, 1, FPTZ_CAPACITY_STARTED);
}

static int handle_capacity_test(FptzServer *s, const FptzHeader *h,
                                const uint8_t *body) {
    uint8_t action, kind;
    uint32_t test_id;
    if (h->payload_len != 8 || !h->txn_id || body[2] || body[3])
        return send_error(s, h, 2, 1);
    action = body[0]; kind = body[1]; test_id = fptz_get32(body + 4);
    if ((action != 1 && action != 2) ||
        (kind != FPTZ_CAPACITY_KIND_INPUT &&
         kind != FPTZ_CAPACITY_KIND_PIPELINE) || !test_id)
        return send_error(s, h, 2, 1);
    if (action == 1) {
        if (s->capacity_active || s->capacity_pending_txn) {
            if (test_id!=s->capacity_test_id || kind!=s->capacity_kind)
                return send_capacity_status(s,h->txn_id,1,FPTZ_CAPACITY_INVALID_STATE);
            if (s->capacity_active)
                return send_capacity_status(s,h->txn_id,1,s->capacity_stopping ?
                                            FPTZ_CAPACITY_PROGRESS:FPTZ_CAPACITY_STARTED);
        }
        if (s->capacity_pending_txn && test_id == s->capacity_test_id &&
            h->txn_id == s->capacity_pending_txn) return 0;
        if (!s->streaming || !s->paused || s->return_active ||
            s->generating || s->validating || s->switch_pending || s->capacity_active || s->capacity_pending_txn) {
            s->capacity_kind = kind; s->capacity_test_id = test_id;
            return send_capacity_status(s, h->txn_id, 1,
                                        FPTZ_CAPACITY_INVALID_STATE);
        }
        if (kind == FPTZ_CAPACITY_KIND_PIPELINE &&
            (!s->capacity_sample || !s->active_lut_valid ||
             !s->ops.start_capacity_stage || !s->ops.poll_capacity_stage)) {
            s->capacity_kind = kind; s->capacity_test_id = test_id;
            return send_capacity_status(s, h->txn_id, 1,
                                        FPTZ_CAPACITY_NO_SAMPLE);
        }
        s->capacity_kind = kind; s->capacity_test_id = test_id;
        s->capacity_last_start_txn = h->txn_id;
        if (any_active_frame(s)) {s->capacity_pending_txn = h->txn_id;return 0;}
        return start_capacity_test(s, h->txn_id);
    }
    if ((s->capacity_active || s->capacity_pending_txn) && kind!=s->capacity_kind)
        return send_capacity_status(s,h->txn_id,2,FPTZ_CAPACITY_INVALID_STATE);
    if (s->capacity_pending_txn && test_id == s->capacity_test_id) {
        s->capacity_pending_txn = 0;
        s->capacity_last_stop_txn = h->txn_id;
        return send_capacity_status(s, h->txn_id, 2, FPTZ_CAPACITY_STOPPED);
    }
    if (!s->capacity_active && test_id == s->capacity_test_id)
        return send_capacity_status(s, h->txn_id, 2, FPTZ_CAPACITY_STOPPED);
    if (!s->capacity_active || test_id != s->capacity_test_id)
        return send_capacity_status(s, h->txn_id, 2,
                                    FPTZ_CAPACITY_INVALID_STATE);
    request_capacity_stop(s, h->txn_id);
    return 0;
}

int fptz_server_init(FptzServer *s, FptzFrameSlot *slots,
                     uint8_t slot_count, uint8_t *lut_a, uint8_t *lut_b,
                     const uint8_t *scene_bits,
                     const FptzServerOps *ops) {
    if (!s || !slots || !slot_count || slot_count > FPTZ_FRAME_SLOTS ||
        !lut_a || !lut_b || lut_a == lut_b || !scene_bits ||
        ((uintptr_t)lut_a & 63u) || ((uintptr_t)lut_b & 63u) ||
        !ops || !ops->send ||
        !ops->start_frame || !ops->poll_frame || !ops->lut_published ||
        !ops->now_us)
        return -1;
    for (uint8_t i = 0; i < slot_count; ++i)
        if (!slots[i].input || !slots[i].output ||
            !slots[i].input_bitmap || !slots[i].output_bitmap) return -1;
    /* Keep calibration-mask identity without a front-only pose table. */
    if (memcmp(scene_bits,fptz_scene_mask_front_bits,FPTZ_SCENE_MASK_BYTES)) return -1;
    memset(s, 0, sizeof(*s));
    s->slots = slots;
    s->slot_count = slot_count;
    s->lut_banks[0] = lut_a;
    s->lut_banks[1] = lut_b;
    s->scene_bits = scene_bits;
    s->rear_scene_bits = fptz_scene_mask_rear_bits;
    s->max_payload = FPTZ_MAX_PAYLOAD;
    s->send_window = 64;
    s->ops = *ops;
    for (uint8_t i = 0; i < slot_count; ++i) slots[i].state = FPTZ_SLOT_FREE;
    return 0;
}

static int send_hello_ack(FptzServer *s, uint32_t txn) {
    uint8_t reply[20] = {0};
    fptz_put16(reply, s->max_payload);
    fptz_put16(reply + 2, s->send_window);
    fptz_put16(reply + 4, s->slot_count);
    reply[6] = 1; /* The PS generates the LUT from SET_PTZ. */
    fptz_put32(reply + 8, fptz_input_object_limit(s->max_payload));
    fptz_put16(reply + 12, FPTZ_PAIR_CAPABILITY);
    fptz_put16(reply + 14, FPTZ_LENS_COUNT);
    fptz_put16(reply + 16, FPTZ_SOURCE_WIDTH);
    fptz_put16(reply + 18, FPTZ_PANORAMA_CAPABILITY);
    if (send_control(s, FPTZ_HELLO_ACK, txn, 0, 0,
                     reply, sizeof(reply)) < 0) return -1;
    send_credit(s);
    return 0;
}

static int handle_hello(FptzServer *s, const FptzHeader *h,
                        const uint8_t *body) {
    uint16_t requested_payload, requested_window;
    if (h->session_id || h->payload_len != 8)
        return send_error(s, h, 2, 1);
    if (s->session_id && h->txn_id && h->txn_id == s->last_hello_txn) {
        if (memcmp(body, s->hello_request, sizeof(s->hello_request)))
            return send_error(s, h, 2, 1);
        return send_hello_ack(s, h->txn_id);
    }
    if (any_active_frame(s) || s->streaming || s->capacity_active || s->capacity_pending_txn)
        return send_error(s, h, 2, 1);
    requested_payload = fptz_get16(body);
    requested_window = fptz_get16(body + 2);
    if (requested_payload < 256 || requested_payload > FPTZ_MAX_PAYLOAD ||
        !requested_window || requested_window > 64)
        return send_error(s, h, 2, 1);
    for (uint8_t i=0;i<s->slot_count;++i) {
        s->slots[i].pair_valid=0;
        s->slots[i].sources_decoded=s->slots[i].corrected_valid=0;
    }
    s->max_payload = requested_payload;
    s->send_window = requested_window;
    s->host_free_slots = fptz_get16(body + 4);
    s->host_credit_seq = 0;
    s->last_hello_txn = h->txn_id;
    memcpy(s->hello_request, body, sizeof(s->hello_request));
    s->session_id = (uint32_t)s->ops.now_us(s->ops.ctx) ^ 0x4650545au;
    if (!s->session_id) s->session_id = 1;
    s->active_epoch = 0;
    s->last_ptz_txn = 0;
    s->last_stream_txn = 0;
    s->last_stream_epoch = 0;
    s->last_frame_id = 0;
    s->last_heartbeat_us = s->ops.now_us(s->ops.ctx);
    s->next_video_tx_us = 0;
    s->last_input_complete = 0;
    s->input_complete_count = 0;
    s->input_complete_bytes = 0;
    s->last_output_sent = 0;
    s->output_complete_count = 0;
    s->output_complete_bytes = 0;
    s->input_incomplete_drop = 0;
    s->input_crc_drop = 0;
    s->stale_epoch_drop = 0;
    s->no_slot_drop = 0;
    s->ready_superseded_drop = 0;
    s->output_send_drop = 0;
    s->output_superseded_drop = 0;
    memset(&s->latency, 0, sizeof(s->latency));
    s->telemetry_attempts = s->telemetry_failures = 0;
    s->flow_active=0; s->flow_generation=0;
    s->flow_crc_samples=s->flow_sent_samples=s->flow_credit_events=s->flow_intervals=0;
    s->flow_crc_us=s->flow_prepare_us=s->flow_first_wait_us=s->flow_tx_us=0;
    s->flow_interval_us=s->flow_last_output_us=0;
    s->flow_input_crc_samples=0; s->flow_input_crc_us=s->flow_input_parse_us=0;
    if (s->ops.set_flow_active) s->ops.set_flow_active(s->ops.ctx,0);
    s->active_lut_valid = 0;
    s->active_bank = 0;
    s->generating = s->validating = s->switch_pending = 0;
    s->pending_ptz_txn = 0;
    s->active_pan_mdeg=s->active_tilt_mdeg=0;
    s->lut_last_generate_us=s->lut_last_validate_us=0;
    s->lut_last_wait_us=s->lut_last_switch_us=0;
    s->active_lens_span[0]=s->active_lens_span[1]=0;
    s->last_ptz_state = s->last_ptz_reason = 0;
    s->paused = 0;
    s->source_mode = 0;
    s->return_active = 0;
    s->return_pending_txn = 0;
    s->return_last_txn = 0;
    s->return_stop_txn = 0;
    s->capacity_active = 0;
    s->capacity_pending_txn = 0;
    s->capacity_operation_active = 0;
    s->capacity_stopping=0;s->capacity_stop_txn=0;
    return send_hello_ack(s, h->txn_id);
}

static int handle_ptz(FptzServer *s, const FptzHeader *h,
                      const uint8_t *body) {
    int32_t pan, tilt;
    uint32_t hfov;
    if (h->payload_len != 12 || !h->txn_id) {
        (void)send_ptz_status(s, h->txn_id, PTZ_REJECTED, PTZ_REASON_INVALID);
        return -1;
    }
    if (s->capacity_active || s->capacity_pending_txn ||
        s->return_active || s->return_pending_txn) {
        (void)send_ptz_status(s,h->txn_id,PTZ_REJECTED,PTZ_REASON_COMPUTE);
        return -1;
    }
    pan = (int32_t)fptz_get32(body);
    tilt = (int32_t)fptz_get32(body + 4);
    hfov = fptz_get32(body + 8);
    if ((hfov!=FPTZ_PTZ_HFOV_MDEG && hfov!=FPTZ_PANORAMA_HFOV_MDEG) ||
        (hfov==FPTZ_PANORAMA_HFOV_MDEG && (pan!=0 || tilt!=0)) ||
        tilt < -FPTZ_PTZ_TILT_LIMIT_MDEG || tilt > FPTZ_PTZ_TILT_LIMIT_MDEG ||
        pan % FPTZ_PTZ_GRID_MDEG || tilt % FPTZ_PTZ_GRID_MDEG) {
        (void)send_ptz_status(s,h->txn_id,PTZ_REJECTED,PTZ_REASON_INVALID);
        return -1;
    }
    pan %= FPTZ_PTZ_PAN_PERIOD_MDEG;
    if (pan >= FPTZ_PTZ_PAN_PERIOD_MDEG/2) pan -= FPTZ_PTZ_PAN_PERIOD_MDEG;
    if (pan < -FPTZ_PTZ_PAN_PERIOD_MDEG/2) pan += FPTZ_PTZ_PAN_PERIOD_MDEG;
    if (h->txn_id == s->last_ptz_txn) {
        if (pan != s->pending_pan_mdeg ||
            tilt != s->pending_tilt_mdeg || hfov != s->pending_hfov_mdeg) {
            (void)send_ptz_status(s, h->txn_id, PTZ_REJECTED,
                                  PTZ_REASON_INVALID);
            return -1;
        }
        return send_ptz_status(s, h->txn_id,
                               s->last_ptz_state, s->last_ptz_reason);
    }
    if (s->last_ptz_txn && h->txn_id < s->last_ptz_txn) {
        (void)send_ptz_status(s, h->txn_id, PTZ_REJECTED, PTZ_REASON_OLD_TXN);
        return -1;
    }
    /* Full dual-lens generation and validation decide coverage at every
     * output pixel. No front-camera region limits 360-degree admission. */
    if (s->generating || s->validating || s->switch_pending)
        (void)send_ptz_status(s, s->pending_ptz_txn, PTZ_REJECTED,
                              PTZ_REASON_SUPERSEDED);
    s->generating = s->validating = s->switch_pending = 0;
    s->candidate_bank = s->active_lut_valid ? (uint8_t)(1u - s->active_bank) : 0;
    int generation_result=hfov==FPTZ_PANORAMA_HFOV_MDEG
        ? fptz_lut_gen_begin_panorama(&s->lut_gen,s->lut_banks[s->candidate_bank],s->scene_bits,s->rear_scene_bits)
        : fptz_lut_gen_begin_dual(&s->lut_gen,s->lut_banks[s->candidate_bank],s->scene_bits,s->rear_scene_bits,pan,tilt,hfov);
    if (generation_result) {
        (void)send_ptz_status(s, h->txn_id, PTZ_REJECTED,
                              PTZ_REASON_COMPUTE);
        return -1;
    }
    s->pending_ptz_txn = h->txn_id;
    s->pending_pan_mdeg = pan;
    s->pending_tilt_mdeg = tilt;
    s->pending_hfov_mdeg = hfov;
    s->last_ptz_txn = h->txn_id;
    s->last_ptz_state = PTZ_ACCEPTED;
    s->last_ptz_reason = PTZ_REASON_NONE;
    s->generating = 1;
    s->lut_started_us = s->ops.now_us(s->ops.ctx);
    s->lut_gen.now_us = s->ops.now_us;
    s->lut_gen.clock_ctx = s->ops.ctx;
    s->lut_gen.scene_pair_bits = s->scene_pair_bits;
    s->lut_gen.rear_scene_pair_bits = s->rear_scene_pair_bits;
    s->lut_generated_us=s->lut_validated_us=0;
    s->lut_candidate_generate_us=s->lut_candidate_validate_us=0;
    s->lut_work_us = 0;
    s->lut_last_wall_us = s->lut_last_work_us = s->lut_last_anchor_us = 0;
    return send_ptz_status(s, h->txn_id, PTZ_ACCEPTED, PTZ_REASON_NONE);
}

static void stop_return_for_stream(FptzServer *s) {
    /* Cached return capacity traffic is valid only while normal video is
     * paused. No hardware descriptor belongs to this software-only sender. */
    s->return_active = 0;
    s->return_pending_txn = 0;
    s->return_tx_next = 0;
}

static int handle_stream(FptzServer *s, const FptzHeader *h,
                         const uint8_t *body) {
    uint8_t response[4] = {0};
    if (h->payload_len != 4 || !h->txn_id || body[1] > 2 ||
        body[2] || body[3])
        return send_error(s, h, 2, 1);
    if (h->txn_id == s->last_stream_txn) {
        if (memcmp(body, s->last_stream_request, 4))
            return send_error(s, h, 2, 1);
        return send_control(s, FPTZ_STREAM_STATUS, h->txn_id, 0,
                            s->last_stream_epoch,
                            s->last_stream_response, 4);
    }
    if (s->last_stream_txn && h->txn_id < s->last_stream_txn)
        return send_error(s, h, 8, 1);
    response[0] = body[0];
    if (body[0] == 1 && FPTZ_DUAL_MEDIA_READY &&
        s->active_epoch && s->active_lut_valid && !s->fault &&
        !s->capacity_active && !s->capacity_pending_txn) {
        stop_return_for_stream(s);
        s->streaming = 1;
        s->paused = 0;
    } else if (body[0] == 2 || body[0] == 3) {
        s->streaming = 0;
        s->paused = 0;
        s->return_active = 0;
        s->return_pending_txn = 0;
        s->capacity_pending_txn = 0;
        request_capacity_stop(s,0); /* Keep polling any hardware operation to drain. */
        for (uint8_t i = 0; i < s->slot_count; ++i) {
            FptzFrameSlot *slot = &s->slots[i];
            if (slot->state == FPTZ_SLOT_RECEIVING ||
                slot->state == FPTZ_SLOT_READY) slot->state = FPTZ_SLOT_FREE;
        }
        send_credit(s);
    } else if (body[0] == 4 && s->streaming) {
        s->paused = 1;
    } else if (body[0] == 5 && s->streaming) {
        stop_return_for_stream(s);
        s->paused = 0;
    } else return send_error(s, h, 7, 1);
    if (body[1]) s->source_mode = body[1];
    response[1] = (uint8_t)(s->streaming && !s->paused);
    response[2] = s->source_mode;
    response[3] = s->paused;
    s->last_stream_txn = h->txn_id;
    s->last_stream_epoch = s->active_epoch;
    memcpy(s->last_stream_request, body, 4);
    memcpy(s->last_stream_response, response, 4);
    if (body[0] == 2 || body[0] == 4) s->stop_diagnostics_pending = 1;
    return send_control(s, FPTZ_STREAM_STATUS, h->txn_id, 0,
                        s->active_epoch, response, sizeof(response));
}

static FptzFrameSlot *find_frame(FptzServer *s, uint32_t id) {
    for (uint8_t i = 0; i < s->slot_count; ++i)
        if ((s->slots[i].state != FPTZ_SLOT_FREE ||
             (s->slots[i].pair_valid && s->slots[i].rx.received_count == s->slots[i].rx.object.chunk_count)) &&
            s->slots[i].frame_id == id) return &s->slots[i];
    return 0;
}

/* Shared ACK for the complete paired object: type, state, first missing
 * chunk, 64-bit SACK and received count; all multibyte fields big endian. */
static void send_input_ack(FptzServer *s,const FptzFrameSlot *slot,uint8_t state) {
    uint8_t body[16]={FPTZ_INPUT_JPEG_PAIR,0};
    uint16_t base; uint64_t mask;
    fptz_rx_sack(&slot->rx,&base,&mask);
    body[1]=state;fptz_put16(body+2,base);
    fptz_put32(body+4,(uint32_t)(mask>>32));fptz_put32(body+8,(uint32_t)mask);
    fptz_put32(body+12,slot->rx.received_count);
    (void)send_control(s,FPTZ_MEDIA_ACK,slot->rx.object.txn_id,slot->frame_id,
                       slot->ptz_epoch,body,sizeof(body));
}
static int handle_input(FptzServer *s, const FptzHeader *h,
                        const uint8_t *body) {
    FptzFrameSlot *slot = find_frame(s, h->frame_id);
    int added,duplicate;
    if (!s->streaming ||
        (s->paused && !(s->capacity_active &&
                        s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT)) ||
        s->switch_pending ||
        !h->frame_id || h->ptz_epoch != s->active_epoch ||
        !h->object_len || h->object_len > fptz_input_object_limit(s->max_payload)) {
        ++s->stale_epoch_drop;
        return 0;
    }
    if (slot && slot->state != FPTZ_SLOT_RECEIVING) {
        /* A duplicate may acknowledge reception; it must never change a
         * running frame or its immutable input storage. */
        if (slot->pair_valid)
            send_input_ack(s,slot,fptz_rx_push(&slot->rx,h,body)<0 ? 2u:1u);
        return 0;
    }
    if (!slot) {
        if (h->frame_id <= s->last_frame_id) return 0;
        for (uint8_t i = 0; i < s->slot_count; ++i) {
            FptzFrameSlot *old = &s->slots[i];
            if (old->frame_id >= h->frame_id) continue;
            if (old->state == FPTZ_SLOT_RECEIVING) {
                ++s->input_incomplete_drop;
                send_input_ack(s,old,2u);
                old->state = FPTZ_SLOT_FREE;
            } else if (old->state == FPTZ_SLOT_READY) {
                ++s->ready_superseded_drop;
                old->state = FPTZ_SLOT_FREE;
            }
        }
        for (uint8_t i = 0; i < s->slot_count; ++i)
            if (s->slots[i].state == FPTZ_SLOT_FREE) {
                slot = &s->slots[i];
                break;
            }
        s->last_frame_id = h->frame_id;
        if (!slot) {
            ++s->no_slot_drop;
            return 0;
        }
        if (fptz_rx_begin(&slot->rx, slot->input, fptz_input_object_limit(s->max_payload),
                          slot->input_bitmap, FPTZ_INPUT_BITMAP_BYTES,
                          s->max_payload, h)) {
            ++s->input_crc_drop;
            return 0;
        }
        slot->frame_id = h->frame_id;
        slot->ptz_epoch = h->ptz_epoch;
        slot->capture_ts_ms = h->capture_ts_ms;
        slot->input_len = h->object_len;
        slot->pair_valid=slot->sources_decoded=slot->corrected_valid=0;
        slot->rx_ack_counter=0;
        memset(&slot->jpeg_pair,0,sizeof(slot->jpeg_pair));
        slot->state = FPTZ_SLOT_RECEIVING;
        slot->first_rx_us = s->ops.now_us(s->ops.ctx);
        slot->input_ready_us = slot->admitted_us = 0;
        slot->output_ready_us = slot->first_tx_us = 0;
        slot->latency_valid = 1;
        memset(slot->stage_times_us, 0, sizeof(slot->stage_times_us));
        if (s->capacity_active &&
            s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT)
            ++s->capacity_attempts[0];
        send_credit(s);
    }
    duplicate=h->chunk_index<slot->rx.object.chunk_count &&
              (slot->rx.bitmap[h->chunk_index>>3] & (1u<<(h->chunk_index&7u)));
    added = fptz_rx_push(&slot->rx, h, body);
    if (added < 0) {
        send_input_ack(s,slot,2u);
        ++s->input_crc_drop;
        if (s->capacity_active &&
            s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT)
            ++s->capacity_failures[0];
        slot->state = FPTZ_SLOT_FREE;
        send_credit(s);
        return 0;
    }
    if (added == 1) {
        uint64_t verify_started=s->ops.now_us(s->ops.ctx);
        int verified=fptz_rx_verify(&slot->rx);
        uint64_t verify_ended=s->ops.now_us(s->ops.ctx);
        int parsed=verified ? -1 :
            fptz_jpeg_pair_parse(slot->input,slot->input_len,&slot->jpeg_pair);
        uint64_t parse_ended=s->ops.now_us(s->ops.ctx);
        if (s->flow_active && !s->capacity_active) {
            ++s->flow_input_crc_samples;
            s->flow_input_crc_us+=verify_ended-verify_started;
            if (!verified) s->flow_input_parse_us+=parse_ended-verify_ended;
        }
        if (verified || parsed) {
            send_input_ack(s,slot,2u);
            ++s->input_crc_drop;
            if (s->capacity_active &&
                s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT)
                ++s->capacity_failures[0];
            slot->state = FPTZ_SLOT_FREE;
            send_credit(s);
            return 0;
        }
        slot->pair_valid=1;
        send_input_ack(s,slot,1u);
        s->last_input_complete = h->frame_id;
        ++s->input_complete_count;
        s->input_complete_bytes += slot->input_len;
        if (s->capacity_active &&
            s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT) {
            ++s->capacity_completed[0];
            s->capacity_bytes[0] += slot->input_len;
            slot->state = FPTZ_SLOT_FREE;
            send_credit(s);
        } else {
            slot->input_ready_us = s->ops.now_us(s->ops.ctx);
            slot->state = FPTZ_SLOT_READY;
        }
    } else {
        uint16_t cadence=s->send_window<32u ? s->send_window:32u;
        if (++slot->rx_ack_counter>=cadence || h->chunk_index+1u==h->chunk_count ||
            duplicate) {
            send_input_ack(s,slot,0u);slot->rx_ack_counter=0;
        }
    }
    return 0;
}

int fptz_server_on_packet(FptzServer *s, uint16_t local_port,
                          const uint8_t *packet, uint16_t length) {
    FptzHeader h;
    const uint8_t *body;
    if (!s || s->fault || fptz_parse_packet(packet, length, &h)) return -1;
    if ((local_port == FPTZ_CONTROL_PORT &&
         h.msg_type >= FPTZ_INPUT_JPEG) ||
        (local_port == FPTZ_INPUT_PORT &&
         h.msg_type != FPTZ_INPUT_JPEG_PAIR &&
         h.msg_type != FPTZ_LUT_DATA) ||
        (local_port != FPTZ_CONTROL_PORT &&
         local_port != FPTZ_INPUT_PORT)) return -1;
    body = packet + FPTZ_HEADER_SIZE;
    if (h.msg_type == FPTZ_HELLO) return handle_hello(s, &h, body);
    if (!s->session_id || h.session_id != s->session_id) return -1;
    switch (h.msg_type) {
    case FPTZ_LUT_PREPARE:
    case FPTZ_LUT_DATA: return send_error(s, &h, 7, 2);
    case FPTZ_SET_PTZ: return handle_ptz(s, &h, body);
    case FPTZ_STREAM_CTRL: return handle_stream(s, &h, body);
    case FPTZ_RETURN_TEST_CTRL: return handle_return_test(s, &h, body);
    case FPTZ_CAPACITY_TEST_CTRL: return handle_capacity_test(s, &h, body);
    case FPTZ_INPUT_JPEG_PAIR: return handle_input(s, &h, body);
    case FPTZ_INPUT_JPEG: return send_error(s,&h,7,2);
    case FPTZ_MEDIA_ACK: return -1;
    case FPTZ_CREDIT: return -1;
    case FPTZ_HEARTBEAT:
        if (h.payload_len) return -1;
        return send_stats(s, h.txn_id);
    default: return -1;
    }
}

/* LAT1 v1: big-endian u32 words. Events: 1 processed, 2 sent,
 * 3 superseded, 4 send failure. Relative times use first_rx_us, not Host time.
 * Only normal video reaches this path. Best-effort diagnostics never fail video. */
static void send_latency_result(FptzServer *s, const FptzFrameSlot *f, uint32_t event) {
    uint8_t b[FPTZ_LATENCY_BYTES] = {0};
    const uint32_t stages[8] = {f->decode_us, f->correct_us, f->encode_us,
        f->decode_front_us, f->decode_rear_us, f->decode_wait_us,
        f->correct_wait_us, f->encode_wait_us};
    const uint64_t t[6] = {f->first_rx_us, f->input_ready_us, f->admitted_us,
        f->output_ready_us, f->first_tx_us, f->last_tx_us};
    fptz_put32(b, 0x4c415431u); fptz_put32(b+4, 1); fptz_put32(b+8, event);
    fptz_put32(b+12, FPTZ_CORRECTOR_CLOCK_HZ);
    fptz_put32(b+16, ++s->telemetry_attempts);
    fptz_put32(b+20, s->telemetry_failures);
    for (unsigned i=0; i<8; ++i) fptz_put32(b+24+i*4, stages[i]);
    fptz_put32(b+56, f->frame_cycles);
    for (unsigned i=0; i<3; ++i)
        fptz_put32(b+64+i*4, saturate_us(t[i+1]>=t[i] ? t[i+1]-t[i]:0));
    if (event==2) {
        for (unsigned i=3; i<5; ++i)
            fptz_put32(b+64+i*4, saturate_us(t[i+1]>=t[i] ? t[i+1]-t[i]:0));
        fptz_put32(b+84, saturate_us(t[5]-t[0]));
    }
    for (unsigned i=0; i<8; ++i)
        fptz_put32(b+88+i*4, saturate_us(f->stage_times_us[i]>=t[0] ?
                    f->stage_times_us[i]-t[0]:0));
    for (unsigned i=0; i<6; ++i)
        fptz_put32(b+120+i*4, saturate_us(t[i]>=t[0] ? t[i]-t[0]:0));
    fptz_put32(b+144, f->input_len); fptz_put32(b+148, f->output_len);
    if (send_control(s,FPTZ_FRAME_RESULT,0,f->frame_id,f->ptz_epoch,b,sizeof(b))<0)
        ++s->telemetry_failures;
}

static void send_frame_result(FptzServer *s, const FptzFrameSlot *slot) {
    uint8_t body[48] = {0};
    fptz_put32(body + 4, slot->decode_us);
    fptz_put32(body + 8, slot->correct_us);
    fptz_put32(body + 12, slot->encode_us);
    fptz_put32(body + 16, slot->input_len);
    fptz_put32(body + 20, slot->output_len);
    fptz_put32(body + 24, slot->frame_cycles);
    fptz_put32(body + 28,slot->decode_front_us);
    fptz_put32(body + 32,slot->decode_rear_us);
    fptz_put32(body + 36,slot->decode_wait_us);
    fptz_put32(body + 40,slot->correct_wait_us);
    fptz_put32(body + 44,slot->encode_wait_us);
    (void)send_control(s, FPTZ_FRAME_RESULT, 0, slot->frame_id,
                       slot->ptz_epoch, body, 36u);
    (void)send_control(s, FPTZ_FRAME_RESULT, 0, slot->frame_id,
                       slot->ptz_epoch, body, sizeof(body));
}

static void record_sent_latency(FptzServer *s, const FptzFrameSlot *slot) {
    const uint64_t t[6] = {slot->first_rx_us, slot->input_ready_us,
        slot->admitted_us, slot->output_ready_us,
        slot->first_tx_us, slot->last_tx_us};
    FptzLatencyStats *stats = &s->latency;
    if (!slot->latency_valid || stats->samples == UINT32_MAX) return;
    for (unsigned i = 1; i < 6; ++i)
        if (t[i] < t[i-1]) return; /* Reject incomplete or discontinuous clocks. */
    for (unsigned i = 0; i < FPTZ_LAT_PARTS; ++i) {
        uint32_t us = saturate_us(i == FPTZ_LAT_TOTAL ? t[5] - t[0] : t[i+1] - t[i]);
        stats->last_us[i] = us;
        stats->sum_us[i] += us;
        if (us > stats->max_us[i]) stats->max_us[i] = us;
    }
    ++stats->samples;
    stats->last_frame = slot->frame_id;
    if (s->flow_active && slot->flow_generation==s->flow_generation &&
        slot->output_prepared_us>=slot->crc_done_us &&
        slot->first_tx_us>=slot->output_prepared_us) {
        ++s->flow_sent_samples;
        s->flow_first_wait_us+=slot->first_tx_us-slot->output_prepared_us;
        s->flow_tx_us+=slot->last_tx_us-slot->first_tx_us;
    }
}

static void pump_output(FptzServer *s, FptzFrameSlot *slot, uint8_t *batch_used) {
    uint8_t packet[FPTZ_HEADER_SIZE + FPTZ_MAX_PAYLOAD];
    FptzHeader h = {0};
    uint64_t now = s->ops.now_us(s->ops.ctx);
    uint32_t offset, remaining;
    uint16_t bytes;
    if (!slot->tx_next) {
        /* Replace only unsent JPEGs. Once the first packet is emitted, finish
         * that object so Host can reassemble it instead of discarding bytes. */
        for (uint8_t i = 0; i < s->slot_count; ++i) {
            const FptzFrameSlot *newer = &s->slots[i];
            if (newer->state == FPTZ_SLOT_OUTPUT &&
                newer->frame_id > slot->frame_id) {
                ++s->output_superseded_drop;
                send_latency_result(s,slot,3);
                slot->state = FPTZ_SLOT_FREE;
                send_credit(s);
                return;
            }
        }
        /* Keep exactly one JPEG in flight; pending output remains replaceable
         * by the newest frame, with no queue of stale video to send later. */
        for (uint8_t i = 0; i < s->slot_count; ++i) {
            const FptzFrameSlot *sending = &s->slots[i];
            if (sending != slot && sending->state == FPTZ_SLOT_OUTPUT &&
                sending->tx_next) return;
        }
    }
    if (*batch_used || now < s->next_video_tx_us) return;
    *batch_used = 1; /* One batch per pump, even if later hardware polling is slow. */
    h.msg_type = FPTZ_OUTPUT_JPEG;
    h.session_id = s->session_id;
    h.frame_id = slot->frame_id;
    h.ptz_epoch = slot->ptz_epoch;
    h.chunk_count = slot->tx_count;
    h.object_len = slot->output_len;
    h.object_crc32 = slot->output_crc32;
    h.capture_ts_ms = slot->capture_ts_ms;
    /* One global batch reservation, even on an error or a short final batch.
       Do not accumulate catch-up tokens while a hardware stage is busy. */
    s->next_video_tx_us = now + FPTZ_VIDEO_TX_GAP_US;
    for (uint16_t batch = 0; batch < FPTZ_VIDEO_TX_BURST; ++batch) {
        offset = (uint32_t)slot->tx_next * s->max_payload;
        remaining = slot->output_len - offset;
        bytes = (uint16_t)(remaining > s->max_payload
                           ? s->max_payload : remaining);
        h.chunk_index = slot->tx_next;
        h.payload_len = bytes;
        memcpy(packet + FPTZ_HEADER_SIZE, slot->output + offset, bytes);
        fptz_write_header(packet, &h);
        if (!slot->tx_next) slot->first_tx_us = s->ops.now_us(s->ops.ctx);
        if (s->ops.send(s->ops.ctx, FPTZ_OUTPUT_PORT, FPTZ_OUTPUT_PORT,
                        packet, (uint16_t)(FPTZ_HEADER_SIZE + bytes)) < 0) {
            ++s->output_send_drop;
            send_latency_result(s,slot,4);
            slot->state = FPTZ_SLOT_FREE;
            s->next_video_tx_us = s->ops.now_us(s->ops.ctx) +
                                 FPTZ_VIDEO_TX_GAP_US;
            send_credit(s);
            return;
        }
        ++slot->tx_next;
        slot->last_tx_us = s->ops.now_us(s->ops.ctx);
        s->next_video_tx_us = slot->last_tx_us + FPTZ_VIDEO_TX_GAP_US;
        if (slot->tx_next == slot->tx_count) {
            record_sent_latency(s, slot);
            send_latency_result(s,slot,2);
            s->last_output_sent = slot->frame_id;
            ++s->output_complete_count;
            s->output_complete_bytes += slot->output_len;
            slot->state = FPTZ_SLOT_FREE;
            send_credit(s);
            return;
        }
    }
}

static void pump_return_output(FptzServer *s, uint8_t *batch_used) {
    uint8_t packet[FPTZ_HEADER_SIZE + FPTZ_MAX_PAYLOAD];
    FptzHeader h = {0};
    uint64_t now = s->ops.now_us(s->ops.ctx);
    if (!s->return_active || !s->streaming || !s->paused ||
        *batch_used || now < s->next_video_tx_us) return;
    *batch_used = 1;
    if (!s->return_tx_next) ++s->return_frames_started;
    h.msg_type=FPTZ_OUTPUT_JPEG; h.session_id=s->session_id;
    h.frame_id=0x80000000u | (s->return_frames_started & 0x7fffffffu);
    h.ptz_epoch=s->active_epoch; h.chunk_count=s->return_tx_count;
    h.object_len=s->return_len; h.object_crc32=s->return_crc32;
    s->next_video_tx_us = now + FPTZ_VIDEO_TX_GAP_US;
    for (uint16_t batch=0; batch<FPTZ_VIDEO_TX_BURST; ++batch) {
        uint32_t offset=(uint32_t)s->return_tx_next*s->max_payload;
        uint32_t remaining=s->return_len-offset;
        uint16_t bytes=(uint16_t)(remaining>s->max_payload?s->max_payload:remaining);
        h.chunk_index=s->return_tx_next; h.payload_len=bytes;
        memcpy(packet+FPTZ_HEADER_SIZE,s->return_jpeg+offset,bytes);
        fptz_write_header(packet,&h);
        if (s->ops.send(s->ops.ctx,FPTZ_OUTPUT_PORT,FPTZ_OUTPUT_PORT,packet,
                        (uint16_t)(FPTZ_HEADER_SIZE+bytes))<0) {
            ++s->return_send_fail; s->return_tx_next=0;
            s->next_video_tx_us=s->ops.now_us(s->ops.ctx)+FPTZ_VIDEO_TX_GAP_US;
            return;
        }
        ++s->return_tx_next;
        s->next_video_tx_us=s->ops.now_us(s->ops.ctx)+FPTZ_VIDEO_TX_GAP_US;
        if (s->return_tx_next==s->return_tx_count) {
            ++s->return_frames_completed;
            s->return_bytes_completed += s->return_len;
            s->return_tx_next=0;
            return;
        }
    }
}

int fptz_server_rx_needs_pump(const FptzServer *s) {
    if (!s) return 0;
    if (s->stop_diagnostics_pending) return 1;
    for (uint8_t i = 0; i < s->slot_count; ++i)
        if (s->slots[i].state == FPTZ_SLOT_READY) return 1;
    return 0;
}

static void reject_candidate(FptzServer *s,uint8_t reason) {
    s->generating=s->validating=s->switch_pending=0;
    s->last_ptz_state=PTZ_REJECTED;s->last_ptz_reason=reason;
    (void)send_ptz_status(s,s->pending_ptz_txn,PTZ_REJECTED,reason);
}
static void prepare_lut_slice(FptzServer *s) {
    uint64_t started,ended;
    int result;
    if (!s->generating && !s->validating) return;
    started=s->ops.now_us(s->ops.ctx);
    if (s->generating) {
        result=fptz_lut_gen_pixels(&s->lut_gen,FPTZ_LUT_SLICE_PIXELS);
        ended=s->ops.now_us(s->ops.ctx);
        if (ended>=started) s->lut_work_us+=ended-started;
        if (result<0) {
            reject_candidate(s,result==-2 ? PTZ_REASON_UNSAFE:PTZ_REASON_COMPUTE);
        } else if (result==1) {
            s->generating=0;s->lut_generated_us=ended;
            s->lut_candidate_generate_us=saturate_us(ended-s->lut_started_us);
            if (fptz_lut_check_begin(&s->lut_check,s->lut_banks[s->candidate_bank],
                                     FPTZ_LUT_BYTES,s->scene_bits,s->rear_scene_bits))
                reject_candidate(s,PTZ_REASON_COMPUTE);
            else s->validating=1;
        }
        return; /* One bounded slice per pump, never both phases together. */
    }
    result=fptz_lut_check_pixels(&s->lut_check,FPTZ_LUT_SLICE_PIXELS);
    ended=s->ops.now_us(s->ops.ctx);
    if (ended>=started) s->lut_work_us+=ended-started;
    if (result<0) {reject_candidate(s,PTZ_REASON_UNSAFE);return;}
    if (result!=1) return;
    s->validating=0;s->switch_pending=1;s->lut_validated_us=ended;
    s->lut_candidate_validate_us=saturate_us(ended-s->lut_generated_us);
    s->lut_last_wall_us=saturate_us(ended-s->lut_started_us);
    s->lut_last_work_us=saturate_us(s->lut_work_us);
    s->lut_last_anchor_us=saturate_us(s->lut_gen.anchor_work_us);
    if (s->lut_last_anchor_us>s->lut_last_work_us) s->lut_last_anchor_us=s->lut_last_work_us;
    s->last_ptz_state=PTZ_READY;
    (void)send_ptz_status(s,s->pending_ptz_txn,PTZ_READY,PTZ_REASON_NONE);
    for (uint8_t i=0;i<s->slot_count;++i) {
        FptzFrameSlot *slot=&s->slots[i];
        if (slot->state==FPTZ_SLOT_RECEIVING) send_input_ack(s,slot,2u);
        if (slot->state==FPTZ_SLOT_RECEIVING || slot->state==FPTZ_SLOT_READY)
            slot->state=FPTZ_SLOT_FREE;
    }
    send_credit(s);
}

void fptz_server_pump(FptzServer *s) {
    uint8_t output_batch_used = 0;
    if (!s || s->fault || !s->session_id) return;
    update_flow_mode(s);
    if (s->return_pending_txn && s->streaming && s->paused && !any_active_frame(s))
        start_return_test(s, s->return_pending_txn);
    if (s->capacity_pending_txn && !any_active_frame(s))
        (void)start_capacity_test(s, s->capacity_pending_txn);
    prepare_lut_slice(s);
    if (s->ops.now_us(s->ops.ctx) - s->last_heartbeat_us >= 1000000u) {
        s->last_heartbeat_us = s->ops.now_us(s->ops.ctx);
        (void)send_stats(s, 0);
        send_credit(s);
    }
    if (s->capacity_active) {
        uint64_t now = s->ops.now_us(s->ops.ctx);
        if (s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT) {
            for (uint8_t i = 0; i < s->slot_count; ++i) {
                FptzFrameSlot *slot = &s->slots[i];
                if (slot->state == FPTZ_SLOT_RECEIVING &&
                    now - slot->first_rx_us > 200000u) {
                    ++s->input_incomplete_drop;
                    ++s->capacity_timeouts[0];
                    slot->state = FPTZ_SLOT_FREE;
                    send_credit(s);
                }
            }
            if (now - s->capacity_stage_started_us >= 5000000u)
                finish_capacity_test(s, ++s->capacity_last_stop_txn);
            return;
        }
        if (s->capacity_stopping && !s->capacity_operation_active) {
            finish_capacity_test(s,s->capacity_stop_txn);return;
        }
        if (!s->capacity_operation_active) {
            uint8_t stage = s->capacity_stage;
            uint8_t index = (uint8_t)(stage - 1u);
            if (now - s->capacity_stage_started_us >= 5000000u) {
                s->capacity_elapsed_ms[index] =
                    (uint32_t)((now - s->capacity_stage_started_us) / 1000u);
                if (stage == 3u) {
                    finish_capacity_test(s, ++s->capacity_last_stop_txn);
                    return;
                }
                ++s->capacity_stage;
                s->capacity_stage_started_us = now;
                (void)send_capacity_status(s, s->capacity_last_start_txn, 1,
                                           FPTZ_CAPACITY_PROGRESS);
                stage = s->capacity_stage; index = (uint8_t)(stage - 1u);
            }
            ++s->capacity_attempts[index];
            if (s->ops.start_capacity_stage(s->ops.ctx, stage,
                    s->capacity_sample, s->lut_banks[s->active_bank]) < 0) {
                ++s->capacity_failures[index];
                finish_capacity_test(s, ++s->capacity_last_stop_txn);
                s->fault=1; /* A failed hardware admission must retain ownership. */
                return;
            }
            s->capacity_operation_active = 1;
        }
        {
            uint8_t index = (uint8_t)(s->capacity_stage - 1u);
            int result = s->ops.poll_capacity_stage(s->ops.ctx,
                s->capacity_stage, s->capacity_sample);
            if (result < 0) {
                ++s->capacity_failures[index];
                finish_capacity_test(s, ++s->capacity_last_stop_txn);
                s->fault=1; /* Never admit bytes into a sample still owned by PL. */
            } else if (result == 1) {
                ++s->capacity_completed[index];
                s->capacity_operation_active = 0;
                if (s->capacity_stopping) finish_capacity_test(s,s->capacity_stop_txn);
            }
        }
        return;
    }
    for (uint8_t i = 0; i < s->slot_count; ++i) {
        FptzFrameSlot *slot = &s->slots[i];
        if (slot->state == FPTZ_SLOT_RECEIVING &&
            s->ops.now_us(s->ops.ctx) - slot->first_rx_us > 200000u) {
            ++s->input_incomplete_drop;
            if (s->capacity_active &&
                s->capacity_kind == FPTZ_CAPACITY_KIND_INPUT)
                ++s->capacity_timeouts[0];
            send_input_ack(s,slot,2u);
            slot->state = FPTZ_SLOT_FREE;
            send_credit(s);
        }
        if (slot->state == FPTZ_SLOT_READY && !s->switch_pending) {
            int admitted = s->ops.start_frame(s->ops.ctx, slot,
                                              s->lut_banks[s->active_bank]);
            if (admitted < 0) {
                s->fault = 1;
                return;
            }
            /* A deferred frame stays READY and may be superseded by newer input.
             * Only admitted frames own PL buffers and become RUNNING. */
            if (admitted > 0) continue;
            slot->admitted_us = s->ops.now_us(s->ops.ctx);
            slot->flow_generation=s->flow_generation;
            slot->state = FPTZ_SLOT_RUNNING;
        }
        if (slot->state == FPTZ_SLOT_RUNNING) {
            int status = s->ops.poll_frame(s->ops.ctx, slot);
            if (status < 0) {
                s->fault = 1;
                return;
            }
            if (status == 1) {
                if (slot->output_len < 4 ||
                    slot->output_len > FPTZ_MAX_JPEG_BYTES ||
                    slot->output[0] != 0xff || slot->output[1] != 0xd8 ||
                    slot->output[slot->output_len - 2] != 0xff ||
                    slot->output[slot->output_len - 1] != 0xd9) {
                    s->fault = 1;
                    return;
                }
                slot->output_ready_us = s->ops.now_us(s->ops.ctx);
                send_latency_result(s,slot,1);
                if (s->flow_active && slot->flow_generation==s->flow_generation) {
                    if (s->flow_last_output_us) {
                        ++s->flow_intervals;
                        s->flow_interval_us+=slot->output_ready_us-s->flow_last_output_us;
                    }
                    s->flow_last_output_us=slot->output_ready_us;
                }
                slot->output_crc32 = fptz_crc32(slot->output,
                                                  slot->output_len);
                slot->crc_done_us=s->ops.now_us(s->ops.ctx);
                if (!s->return_len)
                    (void)fptz_server_cache_return_jpeg(s, slot->output,
                                                       slot->output_len);
                s->capacity_sample = slot;
                slot->tx_count = (uint16_t)((slot->output_len +
                                  s->max_payload - 1u) / s->max_payload);
                slot->tx_next = 0;
                slot->last_tx_us = 0;
                slot->state = FPTZ_SLOT_OUTPUT;
                send_frame_result(s, slot);
                slot->output_prepared_us=s->ops.now_us(s->ops.ctx);
                if (s->flow_active && slot->flow_generation==s->flow_generation) {
                    ++s->flow_crc_samples;
                    s->flow_crc_us+=slot->crc_done_us-slot->output_ready_us;
                    s->flow_prepare_us+=slot->output_prepared_us-slot->crc_done_us;
                }
            }
        }
    }
    /* Observe every hardware completion before selecting an unsent output.
     * A low-index old OUTPUT must not start sending before a later-index
     * RUNNING slot becomes OUTPUT in this same pump. Once started, a JPEG
     * remains pinned until complete, as required by the Host collector. */
    for (uint8_t i = 0; i < s->slot_count; ++i)
        if (s->slots[i].state == FPTZ_SLOT_OUTPUT)
            pump_output(s, &s->slots[i], &output_batch_used);
    pump_return_output(s, &output_batch_used);
    if (s->switch_pending) {
        for (uint8_t i = 0; i < s->slot_count; ++i)
            if (s->slots[i].state == FPTZ_SLOT_RUNNING) return;
        if (s->ops.lut_published(s->ops.ctx,
                                 s->lut_banks[s->candidate_bank],
                                 FPTZ_LUT_BYTES) < 0) {
            s->switch_pending = 0;
            s->last_ptz_state = PTZ_REJECTED;
            s->last_ptz_reason = PTZ_REASON_COMPUTE;
            (void)send_ptz_status(s, s->pending_ptz_txn,
                                  PTZ_REJECTED, PTZ_REASON_COMPUTE);
            s->fault = 1;
            return;
        }
        /* Single-threaded publication after old hardware frames drain:
         * one complete XF/YF/XR/YR/W bank, pose and epoch become active together. */
        s->active_pan_mdeg=s->pending_pan_mdeg;s->active_tilt_mdeg=s->pending_tilt_mdeg;
        s->lut_last_generate_us=s->lut_candidate_generate_us;
        s->lut_last_validate_us=s->lut_candidate_validate_us;
        {
            uint64_t activated=s->ops.now_us(s->ops.ctx);
            s->lut_last_wait_us=saturate_us(activated-s->lut_validated_us);
            s->lut_last_switch_us=saturate_us(activated-s->lut_started_us);
        }
        for (unsigned j=0;j<FPTZ_LENS_COUNT;++j)
            s->active_lens_span[j]=s->lut_check.stats.lens[j].max_span;
        s->active_bank = s->candidate_bank;
        s->active_lut_valid = 1;
        s->switch_pending = 0;
        ++s->active_epoch;
        if (!s->active_epoch) ++s->active_epoch;
        s->last_ptz_state = PTZ_ACTIVE;
        (void)send_ptz_status(s, s->pending_ptz_txn,
                              PTZ_ACTIVE, PTZ_REASON_NONE);
    }
}
