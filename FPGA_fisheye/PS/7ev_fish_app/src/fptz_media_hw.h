#ifndef FPTZ_MEDIA_HW_H
#define FPTZ_MEDIA_HW_H

#include <stddef.h>
#include <stdint.h>

#include "fptz_corrector.h"
#include "fptz_server.h"

#define FPTZ_DEC_BASE  0x80000000u
#define FPTZ_DEC_REAR_BASE 0x80040000u
#define FPTZ_DMA_BASE  0x80010000u
#define FPTZ_GPIO_BASE 0x80020000u
#define FPTZ_FISH_BASE 0x80030000u

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t base, uint32_t offset);
    void (*write)(void *ctx, uint32_t base, uint32_t offset, uint32_t value);
    void (*flush)(void *ctx, uintptr_t address, uint32_t length);
    void (*invalidate)(void *ctx, uintptr_t address, uint32_t length);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
} FptzMediaOps;

typedef struct {
    uint64_t started_us;
    uint8_t started, done, accepted;
    uint8_t previous_generation, generation;
} FptzDecoderRun;
typedef struct {
    FptzFrameSlot *slot;
    uint64_t started_us, deadline_us, stage_ready_us;
    uint8_t stage, stream;
    FptzDecoderRun decoder[2];
    uint8_t corr_started, corr_done, enc_started, enc_done;
    uint64_t corr_started_us, enc_started_us;
    uint64_t order;
    const uint8_t *lut;
} FptzHwFrame;

/* Registration survives frame descriptor reuse. CPU reads are allowed; CPU
 * writes to these four DMA destinations require preparation again while idle. */
typedef struct {
    const FptzFrameSlot *slot;
    uintptr_t rgb, rgb_rear, corrected, output;
} FptzDmaReadOnlySlot;

typedef struct {
    FptzMediaOps ops;
    FptzHwFrame frames[FPTZ_FRAME_SLOTS];
    FptzDmaReadOnlySlot read_only_slots[FPTZ_FRAME_SLOTS];
    FptzCorrector corrector;
    FptzCorrectorOps corrector_ops;
    int8_t dec_active[2];
    int8_t corr_active;
    int8_t enc_active;
    uint8_t fault;
    uint8_t fault_stopped;
    uint64_t next_order;
    const uint8_t *lut;
    FptzPipelineStats stats;
    uint8_t flow_active;
    uint64_t flow_started_us[3], flow_completed_us[3];
} FptzMediaHw;

enum FptzCapacityStage {
    FPTZ_CAPACITY_DECODE = 1,
    FPTZ_CAPACITY_CORRECT = 2,
    FPTZ_CAPACITY_ENCODE = 3
};

int fptz_media_init(FptzMediaHw *pipeline, const FptzMediaOps *ops);
/* Opt-in for board buffers written only by PL after this full cache cleanup.
 * Call before admitting frames, with engines and descriptors idle. Aligned,
 * whole cache lines are required. Unregistered/rebound slots retain the
 * conservative per-frame destination invalidation. The caller must enforce
 * the CPU read-only contract for rgb, rgb_rear, corrected and output for its lifetime. */
int fptz_media_prepare_read_only_slot(FptzMediaHw *pipeline, FptzFrameSlot *slot);
void fptz_media_lut_published(FptzMediaHw *pipeline,
                              const uint8_t *lut, uint32_t length);
/* 0 accepted; 1 deferred (leave server slot READY); -1 invalid/fatal. */
int fptz_media_start(FptzMediaHw *pipeline, FptzFrameSlot *slot,
                     const uint8_t *lut);
int fptz_media_poll(FptzMediaHw *pipeline, FptzFrameSlot *slot);
int fptz_media_capacity_start(FptzMediaHw *pipeline, uint8_t stage,
                              FptzFrameSlot *slot, const uint8_t *lut);
int fptz_media_capacity_poll(FptzMediaHw *pipeline, uint8_t stage,
                             FptzFrameSlot *slot);
void fptz_media_get_stats(const FptzMediaHw *pipeline,
                          FptzPipelineStats *stats);
void fptz_media_set_flow_active(FptzMediaHw *pipeline, int active);

#endif
