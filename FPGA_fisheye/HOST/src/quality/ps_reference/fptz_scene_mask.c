#include "fptz_scene_mask.h"
#include <string.h>

static int scene_bit(const uint8_t *bits, int x, int y) {
    uint32_t index = (uint32_t)y * FPTZ_SOURCE_WIDTH + (uint32_t)x;
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

int fptz_scene_valid_2x2(const uint8_t *bits, int x, int y) {
    if (!bits || x < 0 || x >= (int)FPTZ_SOURCE_WIDTH - 1 || y < 0 || y >= (int)FPTZ_SOURCE_HEIGHT - 1) return 0;
    return scene_bit(bits, x, y) && scene_bit(bits, x + 1, y) &&
           scene_bit(bits, x, y + 1) && scene_bit(bits, x + 1, y + 1);
}

void fptz_scene_pair_build(const uint8_t *bits, uint8_t *pairs) {
    if (!bits || !pairs) return;
    memset(pairs, 0, FPTZ_SCENE_PAIR_BYTES);
    for (uint32_t y = 0; y + 1u < FPTZ_SOURCE_HEIGHT; ++y) {
        for (uint32_t x = 0; x + 1u < FPTZ_SOURCE_WIDTH; ++x) {
            if (fptz_scene_valid_2x2_fast(bits, (int)x, (int)y)) {
                const uint32_t index = y * FPTZ_SOURCE_WIDTH + x;
                pairs[index >> 3] |= (uint8_t)(1u << (index & 7u));
            }
        }
    }
}

const uint8_t *fptz_scene_mask_for_lens(uint32_t lens_id) {
    return lens_id == FPTZ_LENS_FRONT ? fptz_scene_mask_front_bits :
           lens_id == FPTZ_LENS_REAR ? fptz_scene_mask_rear_bits : NULL;
}
