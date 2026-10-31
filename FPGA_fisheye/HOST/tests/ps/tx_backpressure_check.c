/* Exercise the production sender with deterministic DMA pressure and clocks. */
#include "fptz_server.c"
#include <assert.h>
#include <stdio.h>

static uint64_t clock_value = 1000;
static unsigned attempts, accepted, send_cost;
static int block_chunk = -1, block_count, always_block, fatal_send;
static uint16_t order[128];
static uint64_t test_now(void *ctx) { (void)ctx; return ++clock_value; }
static int test_send(void *ctx, uint16_t local, uint16_t remote,
                     const uint8_t *packet, uint16_t length) {
    FptzHeader h;
    (void)ctx; (void)local; (void)remote;
    assert(!fptz_parse_packet(packet, length, &h));
    if (h.msg_type != FPTZ_OUTPUT_JPEG) return length;
    ++attempts;
    clock_value += send_cost;
    if (fatal_send) return -1;
    if (always_block || (h.chunk_index == block_chunk && block_count-- > 0))
        return FPTZ_SEND_AGAIN;
    assert(accepted < sizeof(order)/sizeof(order[0]));
    order[accepted++] = h.chunk_index;
    return length; /* Existing byte-count adapters remain compatible. */
}
static uint8_t image[(FPTZ_VIDEO_TX_BURST + 3) * FPTZ_MAX_PAYLOAD];
static void prepare(FptzServer *s, FptzFrameSlot *slots) {
    memset(s, 0, sizeof(*s));
    memset(slots, 0, 2 * sizeof(*slots));
    s->slots = slots; s->slot_count = 2; s->max_payload = FPTZ_MAX_PAYLOAD;
    s->session_id = 1; s->active_epoch = 2;
    s->ops.send = test_send; s->ops.now_us = test_now;
    slots[0].state = FPTZ_SLOT_OUTPUT;
    slots[0].frame_id = 7; slots[0].ptz_epoch = 2;
    slots[0].output = image; slots[0].output_len = sizeof(image);
    slots[0].tx_count = FPTZ_VIDEO_TX_BURST + 3;
    slots[0].output_crc32 = fptz_crc32(image, sizeof(image));
    attempts = accepted = send_cost = 0;
    block_chunk = -1; block_count = always_block = fatal_send = 0;
}
static void pump(FptzServer *s, FptzFrameSlot *slot) {
    uint8_t used = 0;
    clock_value = s->next_video_tx_us + 1;
    pump_output(s, slot, &used);
}
int main(void) {
    FptzServer s; FptzFrameSlot slots[2];
    prepare(&s, slots);
    block_chunk = 3; block_count = 1;
    pump(&s, slots);
    assert(slots[0].tx_next == 3 && accepted == 3 && !s.output_send_drop);
    unsigned previous = attempts;
    uint8_t used = 0;
    pump_output(&s, slots, &used);
    assert(attempts == previous); /* Respect the retry gap. */
    slots[1].state = FPTZ_SLOT_OUTPUT; slots[1].frame_id = 8;
    for (int n = 0; n < 10 && slots[0].state != FPTZ_SLOT_FREE; ++n)
        pump(&s, slots);
    assert(s.output_complete_count == 1 && !s.output_send_drop && !s.output_superseded_drop);
    assert(accepted == FPTZ_VIDEO_TX_BURST + 3);
    for (unsigned n = 0; n < accepted; ++n) assert(order[n] == n);

    prepare(&s, slots); always_block = 1;
    pump(&s, slots);
    clock_value = slots[0].tx_retry_started_us + FPTZ_VIDEO_TX_RETRY_US + 1;
    used = 0; pump_output(&s, slots, &used);
    assert(slots[0].state == FPTZ_SLOT_FREE && s.output_send_drop == 1 && !accepted);

    prepare(&s, slots); fatal_send = 1;
    pump(&s, slots);
    assert(slots[0].state == FPTZ_SLOT_FREE && s.output_send_drop == 1);

    prepare(&s, slots); send_cost = FPTZ_VIDEO_TX_BUDGET_US + 1;
    pump(&s, slots);
    assert(slots[0].tx_next == 1 && accepted == 1); /* Yield even before burst limit. */

    prepare(&s, slots);
    s.return_active = s.streaming = s.paused = 1;
    s.return_jpeg = image; s.return_len = sizeof(image);
    s.return_tx_count = FPTZ_VIDEO_TX_BURST + 3;
    block_chunk = 0; block_count = 2;
    used = 0; pump_return_output(&s, &used);
    assert(!s.return_tx_next && s.return_frames_started == 1 && !s.return_send_fail);
    for (int n = 0; n < 10 && !s.return_frames_completed; ++n) {
        clock_value = s.next_video_tx_us + 1;
        used = 0; pump_return_output(&s, &used);
    }
    assert(s.return_frames_started == 1 && s.return_frames_completed == 1 && !s.return_send_fail);
    for (unsigned n = 0; n < accepted; ++n) assert(order[n] == n);
    puts("{\"transient_retry_exact_chunk\":true,\"started_frame_pinned\":true,\"bounded_retry\":true,\"tx_budget_yields\":true,\"capacity_sender_retry\":true}");
    return 0;
}
