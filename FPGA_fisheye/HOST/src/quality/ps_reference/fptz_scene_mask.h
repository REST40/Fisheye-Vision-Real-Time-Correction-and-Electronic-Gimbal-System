#ifndef FPTZ_SCENE_MASK_H
#define FPTZ_SCENE_MASK_H
#include <stdint.h>
#include "fptz_geometry.h"
#define FPTZ_SCENE_ROW_BYTES (FPTZ_SOURCE_WIDTH / 8u)
#define FPTZ_SCENE_MASK_BYTES (FPTZ_SOURCE_PIXELS / 8u)
#define FPTZ_SCENE_PAIR_BYTES FPTZ_SCENE_MASK_BYTES
extern const uint8_t fptz_scene_mask_front_bits[FPTZ_SCENE_MASK_BYTES];
extern const uint8_t fptz_scene_mask_rear_bits[FPTZ_SCENE_MASK_BYTES];
/* PS-1 default; lens selection over the wire is introduced in PS-2. */
#define fptz_scene_mask_bits fptz_scene_mask_front_bits
const uint8_t *fptz_scene_mask_for_lens(uint32_t lens_id);
int fptz_scene_valid_2x2(const uint8_t *bits, int x, int y);
void fptz_scene_pair_build(const uint8_t *bits, uint8_t *pairs);
static inline int fptz_scene_valid_2x2_pair(const uint8_t *pairs, int x, int y) {
    if (!pairs || x < 0 || x >= (int)FPTZ_SOURCE_WIDTH - 1 ||
        y < 0 || y >= (int)FPTZ_SOURCE_HEIGHT - 1) return 0;
    const uint32_t index = (uint32_t)y * FPTZ_SOURCE_WIDTH + (uint32_t)x;
    return (pairs[index >> 3] >> (index & 7u)) & 1u;
}
static inline int fptz_scene_valid_2x2_fast(const uint8_t *bits, int x, int y) {
    if (!bits || x < 0 || x >= (int)FPTZ_SOURCE_WIDTH - 1 ||
        y < 0 || y >= (int)FPTZ_SOURCE_HEIGHT - 1) return 0;
    const uint32_t byte = (uint32_t)y * FPTZ_SCENE_ROW_BYTES + ((uint32_t)x >> 3);
    const uint32_t shift = (uint32_t)x & 7u;
    uint32_t upper = (uint32_t)bits[byte] >> shift;
    uint32_t lower = (uint32_t)bits[byte + FPTZ_SCENE_ROW_BYTES] >> shift;
    if (shift == 7u) {
        upper |= (uint32_t)bits[byte + 1u] << 1;
        lower |= (uint32_t)bits[byte + FPTZ_SCENE_ROW_BYTES + 1u] << 1;
    }
    return (upper & lower & 3u) == 3u;
}
#endif
