#ifndef FPTZ_LUT_GENERATE_H
#define FPTZ_LUT_GENERATE_H
#include <stdint.h>
#include "fptz_camera.h"
#include "fptz_scene_mask.h"
typedef struct {
    uint8_t *lut;
    const uint8_t *scene_bits, *rear_scene_bits;
    const uint8_t *scene_pair_bits, *rear_scene_pair_bits;
    uint32_t next_row, next_col, valid_pixels, projection_count;
    /* Retained diagnostic name; measures exact per-pixel projection work. */
    uint64_t anchor_work_us;
    uint64_t (*now_us)(void *ctx);
    void *clock_ctx;
    uint32_t max_span, lens_max_span[FPTZ_LENS_COUNT];
    uint32_t min_retreats[FPTZ_LENS_COUNT], max_retreats[FPTZ_LENS_COUNT];
    int previous_min[FPTZ_LENS_COUNT], previous_max[FPTZ_LENS_COUNT];
    int row_min[FPTZ_LENS_COUNT], row_max[FPTZ_LENS_COUNT];
    int failed;
    uint8_t panorama;
    double focal;
    double r00, r01, r02, r11, r12, r20, r21, r22;
} FptzLutGen;
/* Projection uses world rays and nominal rear diag(-1,1,-1); local Q6 X. */
int fptz_lut_project_one_lens(uint32_t lens_id, int32_t pan_mdeg,
                            int32_t tilt_mdeg, uint32_t hfov_mdeg,
                            uint32_t x, uint32_t y, int32_t *qx, int32_t *qy);
int fptz_lut_project_one(int32_t pan_mdeg, int32_t tilt_mdeg,
                       uint32_t hfov_mdeg, uint32_t x, uint32_t y,
                       int32_t *qx, int32_t *qy);
int fptz_lut_gen_begin_dual(FptzLutGen *gen, uint8_t *candidate,
                          const uint8_t *front_bits, const uint8_t *rear_bits,
                          int32_t pan_mdeg, int32_t tilt_mdeg,
                          uint32_t hfov_mdeg);
/* Fixed world-aligned full 360x180 panorama in the existing output raster.
 * Raster remains 1920x1080 for the PL encoder; display uses angular coordinates. */
int fptz_lut_gen_begin_panorama(FptzLutGen *gen, uint8_t *candidate,
                               const uint8_t *front_bits, const uint8_t *rear_bits);
/* Compatibility entry: supplied front mask plus calibrated rear mask. */
int fptz_lut_gen_begin(FptzLutGen *gen, uint8_t *candidate,
                     const uint8_t *scene_bits, int32_t pan_mdeg,
                     int32_t tilt_mdeg, uint32_t hfov_mdeg);
/* Explicit nearest-even quantization independent of the FPU rounding mode.
 * Input must be a finite normalized weight in [0,1]. */
uint16_t fptz_lut_weight_q8(double weight);
/* 0: more work; 1: complete and safe; -1: bad input; -2: unsafe view.
 * Unsafe candidates are never published; failure remains latched until begin. */
int fptz_lut_gen_rows(FptzLutGen *gen, uint32_t max_rows);
int fptz_lut_gen_pixels(FptzLutGen *gen, uint32_t max_pixels);
#endif
