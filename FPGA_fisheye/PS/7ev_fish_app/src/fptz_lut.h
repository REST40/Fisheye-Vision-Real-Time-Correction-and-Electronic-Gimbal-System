#ifndef FPTZ_LUT_H
#define FPTZ_LUT_H
#include <stddef.h>
#include <stdint.h>
#include "fptz_protocol.h"
typedef struct {
    uint32_t sampled_pixels, sampled_rows, max_span, min_retreats, max_retreats;
} FptzLensLutStats;
typedef struct {
    uint32_t valid_pixels, valid_rows, max_span, min_retreats, max_retreats;
    uint32_t blended_pixels;
    FptzLensLutStats lens[FPTZ_LENS_COUNT];
} FptzLutStats;
typedef struct {
    const uint8_t *raw, *mask[FPTZ_LENS_COUNT];
    FptzLutStats stats;
    uint32_t next_row, next_col;
    int row_min[FPTZ_LENS_COUNT], row_max[FPTZ_LENS_COUNT];
    int previous_min[FPTZ_LENS_COUNT], previous_max[FPTZ_LENS_COUNT];
    int failed;
} FptzLutCheck;
/* Little endian XF,YF,XR,YR signed Q6 planes, then uint16 Q8 W.
 * Only nonzero-weight lenses require legal coordinates and four neighbours.
 * Row retreat is supported by PL tags/reload and never causes rejection. */
int fptz_lut_check_begin(FptzLutCheck *check, const uint8_t *raw, size_t bytes,
                        const uint8_t *front_bits, const uint8_t *rear_bits);
/* Bounded work: 0 pending, 1 complete, -1 invalid/unsafe; failure is latched. */
int fptz_lut_check_pixels(FptzLutCheck *check, uint32_t max_pixels);
int fptz_lut_validate_masks(const uint8_t *raw, size_t bytes,
                           const uint8_t *front_bits, const uint8_t *rear_bits,
                           FptzLutStats *stats);
int fptz_lut_validate(const uint8_t *raw, size_t bytes, FptzLutStats *stats);
#endif
