#ifndef FPTZ_SCHEDULER_H
#define FPTZ_SCHEDULER_H

#include <stdint.h>

#define FPTZ_RX_BATCH_PACKETS 32u
#define FPTZ_RX_BUDGET_US 250u
#define FPTZ_LUT_SLICE_PIXELS 1024u
#define FPTZ_RX_YIELD 2

/* Non-blocking RX callback: 1 means one packet, 2 one packet plus media yield,
 * zero means empty. A yield packet is counted before ending the batch.
 * Limits prevent continuous arrivals starving LUT/media work. A single callback
 * can exceed the time budget; it is not preempted. */
static inline uint32_t fptz_service_rx(int (*receive)(void *),
                                      uint64_t (*now_us)(void *), void *ctx) {
    const uint64_t start = now_us(ctx);
    uint32_t count = 0;
    while (count < FPTZ_RX_BATCH_PACKETS) {
        int received = receive(ctx);
        if (received <= 0) break;
        ++count;
        if (received == FPTZ_RX_YIELD) break;
        if (now_us(ctx) - start >= FPTZ_RX_BUDGET_US) break;
    }
    return count;
}

#endif
