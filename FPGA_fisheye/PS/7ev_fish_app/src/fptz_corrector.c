#include "fptz_corrector.h"

#define IP_STATUS 0x04u
#define IP_SOURCE 0x08u
#define IP_MAP_X 0x0cu
#define IP_MAP_Y 0x10u
#define IP_TARGET 0x14u
#define IP_CYCLES 0x18u

int fptz_corrector_check_capabilities(const FptzCorrectorOps *ops) {
    if (!ops || !ops->read) return -2;
    return ops->read(ops->ctx, FPTZ_IP_SOURCE_SIZE) == FPTZ_PL_SOURCE_SIZE &&
           ops->read(ops->ctx, FPTZ_IP_OUTPUT_SIZE) == FPTZ_PL_OUTPUT_SIZE &&
           ops->read(ops->ctx, FPTZ_IP_CACHE_ROWS) == FPTZ_CACHE_ROWS &&
           ops->read(ops->ctx, FPTZ_IP_ABI_VERSION) == FPTZ_PL_ABI_VERSION &&
           ops->read(ops->ctx, FPTZ_IP_LENS_COUNT) == FPTZ_LENS_COUNT &&
           ops->read(ops->ctx, FPTZ_IP_LUT_FORMAT) == FPTZ_PL_LUT_FORMAT &&
           ops->read(ops->ctx, FPTZ_IP_CACHE_MODE) == FPTZ_PL_CACHE_MODE ? 0 : -2;
}

static int valid_regions(const uint32_t address[8]) {
    const uint32_t bytes[8] = {
        FPTZ_SOURCE_RGBX_BYTES, FPTZ_SOURCE_RGBX_BYTES,
        FPTZ_LUT_PLANE_BYTES, FPTZ_LUT_PLANE_BYTES,
        FPTZ_LUT_PLANE_BYTES, FPTZ_LUT_PLANE_BYTES,
        FPTZ_LUT_WEIGHT_BYTES, FPTZ_OUTPUT_RGBX_BYTES
    };
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t end = (uint64_t)address[i] + bytes[i];
        if (!address[i] || (address[i] & 63u) || end > 0x80000000ull) return 0;
        for (unsigned j = 0; j < i; ++j) {
            uint64_t prior_end = (uint64_t)address[j] + bytes[j];
            if ((uint64_t)address[i] < prior_end && (uint64_t)address[j] < end)
                return 0;
        }
    }
    return 1;
}
int fptz_corrector_start_dual(FptzCorrector *hw, const FptzCorrectorOps *ops,
                              const FptzCorrectorFrame *frame) {
    return fptz_corrector_start_dual_config(hw, ops, frame,
                                           FPTZ_PL_CENTER_X, FPTZ_PL_CENTER_Y, 0);
}
int fptz_corrector_start_dual_config(FptzCorrector *hw,
                                     const FptzCorrectorOps *ops,
                                     const FptzCorrectorFrame *frame,
                                     uint32_t center_x, uint32_t center_y,
                                     uint32_t pipe_cfg) {
    uint32_t address[8];
    const uint32_t offsets[8] = {
        IP_SOURCE, FPTZ_IP_REAR_SOURCE, IP_MAP_X, IP_MAP_Y,
        FPTZ_IP_REAR_MAP_X, FPTZ_IP_REAR_MAP_Y, FPTZ_IP_WEIGHT, IP_TARGET
    };
    if (!frame) return -1;
    address[0] = frame->source_front; address[1] = frame->source_rear;
    address[2] = frame->map_x_front; address[3] = frame->map_y_front;
    address[4] = frame->map_x_rear; address[5] = frame->map_y_rear;
    address[6] = frame->weight; address[7] = frame->target;
    if (!hw || !ops || !ops->read || !ops->write || !ops->invalidate ||
        !ops->now_us || (pipe_cfg & ~3u) || hw->active || hw->fault ||
        !valid_regions(address) || (ops->read(ops->ctx, IP_STATUS) & 1u)) return -1;
    if (fptz_corrector_check_capabilities(ops)) return -2;
    ops->invalidate(ops->ctx, frame->target, FPTZ_OUTPUT_RGBX_BYTES);
    for (unsigned i = 0; i < 8; ++i)
        ops->write(ops->ctx, offsets[i], address[i]);
    ops->write(ops->ctx, FPTZ_IP_CENTER_X, center_x);
    ops->write(ops->ctx, FPTZ_IP_CENTER_Y, center_y);
    ops->write(ops->ctx, FPTZ_IP_PIPE_CFG, pipe_cfg);
    for (unsigned i = 0; i < 8; ++i)
        if (ops->read(ops->ctx, offsets[i]) != address[i]) return -2;
    /* Reject a bitstream without the new configuration registers before
     * START: otherwise the encoder would wait forever on an absent stream. */
    if (ops->read(ops->ctx, FPTZ_IP_PIPE_CFG) != pipe_cfg ||
        ops->read(ops->ctx, FPTZ_IP_CENTER_X) != center_x ||
        ops->read(ops->ctx, FPTZ_IP_CENTER_Y) != center_y) return -2;
    hw->started_us = ops->now_us(ops->ctx);
    hw->frame_cycles = 0;
    hw->pipe_cfg = pipe_cfg;
    hw->expected_crop = 0;
    hw->last_status = 0;
    hw->busy_seen = 0;
    hw->poll_count = 0;
    hw->max_poll_gap_us = 0;
    hw->last_poll_us = hw->started_us;
    hw->active = 1;
    ops->write(ops->ctx, 0x00u, 1u);
    /* Confirm START acceptance here, before cache/network/LUT work can delay
     * the media pump past the entire frame. Never accept old sticky DONE as
     * an ACK. Fixed 1080p processing cannot finish in this short ACK window. */
    for (unsigned attempt = 0; attempt < 256u; ++attempt) {
        uint32_t status = ops->read(ops->ctx, IP_STATUS);
        hw->last_status = status;
        if (status & 1u) {
            hw->busy_seen = 1;
            return 0;
        }
        if (ops->now_us(ops->ctx) - hw->started_us >= 1000u) break;
    }
    hw->active = 0;
    hw->fault = 1;
    return -4; /* START was not acknowledged; do not consume previous DONE. */
}

int fptz_corrector_poll(FptzCorrector *hw, const FptzCorrectorOps *ops,
                        uint64_t timeout_us) {
    uint32_t status;
    if (!hw || !ops || !hw->active) return -1;
    {
        uint64_t now = ops->now_us(ops->ctx);
        uint64_t gap = now - hw->last_poll_us;
        if (gap > hw->max_poll_gap_us)
            hw->max_poll_gap_us = gap > 0xffffffffull ? 0xffffffffu : (uint32_t)gap;
        hw->last_poll_us = now;
        ++hw->poll_count;
    }
    status = ops->read(ops->ctx, IP_STATUS);
    hw->last_status = status;
    if (status & 1u) hw->busy_seen = 1;
    if (status & FPTZ_STATUS_FATAL) {
        hw->fault = 1;
        if (!(status & 1u)) hw->active = 0;
        return -2;
    }
    /* Do not accept the previous frame's sticky DONE immediately after START. */
    if (hw->busy_seen &&
        (status & 3u) &&
        ops->read(ops->ctx, FPTZ_IP_ACTIVE_CROP) != hw->expected_crop) {
        hw->fault = 1;
        return -2;
    }
    if (!(status & 1u) && (status & 2u) &&
        hw->busy_seen) {
        if ((hw->pipe_cfg & FPTZ_PIPE_STREAM) && !(status & 16u)) {
            hw->fault = 1;
            hw->active = 0;
            return -2;
        }
        hw->frame_cycles = ops->read(ops->ctx, IP_CYCLES);
        hw->active = 0;
        return 1;
    }
    if (ops->now_us(ops->ctx) - hw->started_us > timeout_us) {
        hw->fault = 1;
        return -3;
    }
    return 0;
}
