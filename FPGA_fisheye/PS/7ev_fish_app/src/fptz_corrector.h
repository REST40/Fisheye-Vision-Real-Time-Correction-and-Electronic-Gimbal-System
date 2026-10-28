#ifndef FPTZ_CORRECTOR_H
#define FPTZ_CORRECTOR_H

#include <stdint.h>
#include "fptz_geometry.h"
/* ACTIVE_CROP must be zero for the full 1328x1328 source. */
#define FPTZ_IP_STATUS 0x04u
#define FPTZ_IP_CENTER_X 0x1cu
#define FPTZ_IP_CENTER_Y 0x20u
#define FPTZ_IP_PIPE_CFG 0x24u
#define FPTZ_IP_ACTIVE_CROP 0x28u
#define FPTZ_IP_SOURCE_PROGRESS 0x2cu
#define FPTZ_IP_SOURCE_SIZE 0x30u
#define FPTZ_IP_OUTPUT_SIZE 0x34u
#define FPTZ_IP_CACHE_ROWS 0x38u
#define FPTZ_IP_ABI_VERSION 0x3cu
#define FPTZ_IP_REAR_SOURCE 0x40u
#define FPTZ_IP_REAR_MAP_X 0x44u
#define FPTZ_IP_REAR_MAP_Y 0x48u
#define FPTZ_IP_WEIGHT 0x4cu
#define FPTZ_IP_REAR_PROGRESS 0x50u
#define FPTZ_IP_LENS_COUNT 0x54u
#define FPTZ_IP_LUT_FORMAT 0x58u
#define FPTZ_IP_CACHE_MODE 0x5cu
#define FPTZ_PIPE_WAIT_SOURCE 1u
#define FPTZ_PIPE_STREAM 2u
#define FPTZ_STATUS_FATAL (4u | 8u | 128u | 256u)

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*invalidate)(void *ctx, uint32_t address, uint32_t bytes);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
} FptzCorrectorOps;

typedef struct {
    uint64_t started_us;
    uint32_t frame_cycles;
    uint32_t pipe_cfg;
    uint32_t expected_crop;
    uint32_t last_status;
    uint64_t last_poll_us;
    uint32_t poll_count;
    uint32_t max_poll_gap_us;
    uint8_t busy_seen;
    uint8_t active;
    uint8_t fault;
} FptzCorrector;

/* 0 matches; -2 means wrong/missing PL geometry, capacity or ABI. */
int fptz_corrector_check_capabilities(const FptzCorrectorOps *ops);
typedef struct {
    uint32_t source_front, source_rear;
    uint32_t map_x_front, map_y_front, map_x_rear, map_y_rear;
    uint32_t weight, target;
} FptzCorrectorFrame;
/* All eight regions must be disjoint, 64-byte aligned and below 2 GiB.
 * 0 after BUSY; -1 invalid/busy; -2 incompatible PL; -4 missing START ACK. */
int fptz_corrector_start_dual(FptzCorrector *corrector,
                              const FptzCorrectorOps *ops,
                              const FptzCorrectorFrame *frame);
int fptz_corrector_start_dual_config(FptzCorrector *corrector,
                                     const FptzCorrectorOps *ops,
                                     const FptzCorrectorFrame *frame,
                                     uint32_t center_x, uint32_t center_y,
                                     uint32_t pipe_cfg);
/* Return 0 while waiting, 1 on DONE, negative on fault or timeout. */
int fptz_corrector_poll(FptzCorrector *corrector,
                        const FptzCorrectorOps *ops, uint64_t timeout_us);

#endif
