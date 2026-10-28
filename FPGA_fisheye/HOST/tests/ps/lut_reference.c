#include "fptz_lut_generate.h"
#include <limits.h>
#include <math.h>
#include <string.h>
/* Match separate Python binary64 operations before float32 Q6 conversion. */
#if defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif
#define DEG_TO_RAD (3.14159265358979323846 / 180.0)
static int configure(FptzLutGen *g, int32_t pan_mdeg,
                     int32_t tilt_mdeg, uint32_t hfov_mdeg) {
    double pan, tilt, hfov, ca, sa, cb, sb;
    if (pan_mdeg < -180000 || pan_mdeg > 180000 ||
        tilt_mdeg < -90000 || tilt_mdeg > 90000 ||
        hfov_mdeg <= 1000u || hfov_mdeg >= 179000u) return -1;
    pan = ((double)pan_mdeg / 1000.0) * DEG_TO_RAD;
    tilt = ((double)tilt_mdeg / 1000.0) * DEG_TO_RAD;
    hfov = ((double)hfov_mdeg / 2000.0) * DEG_TO_RAD;
    ca = cos(pan); sa = sin(pan); cb = cos(tilt); sb = sin(tilt);
    g->focal = (double)FPTZ_OUTPUT_WIDTH / (2.0 * tan(hfov));
    /* R_yaw @ R_pitch: +pan right, +tilt up, camera Y down. */
    g->r00 = ca; g->r01 = sa * sb; g->r02 = sa * cb;
    g->r11 = cb; g->r12 = -sb;
    g->r20 = -sa; g->r21 = ca * sb; g->r22 = ca * cb;
    return isfinite(g->focal) && g->focal > 0.0 ? 0 : -1;
}
static void world_ray(const FptzLutGen *g, uint32_t col, uint32_t row,
                      double *rx, double *ry, double *rz, double *norm) {
    const double X = ((double)col - FPTZ_PL_CENTER_X) / g->focal;
    const double Y = ((double)row - FPTZ_PL_CENTER_Y) / g->focal;
    *rx = g->r00 * X + g->r01 * Y + g->r02;
    *ry = g->r11 * Y + g->r12;
    *rz = g->r20 * X + g->r21 * Y + g->r22;
    *norm = sqrt(*rx * *rx + *ry * *ry + *rz * *rz);
}
static int project(uint32_t lens, double rx, double ry, double rz, double norm,
                   int32_t *qx, int32_t *qy) {
    const FptzLensCalibration *c = &fptz_camera_calibration[lens];
    double denominator, x, y, r2, radial, xd, yd;
    float mx, my;
    *qx = *qy = INT32_MIN;
    if (lens == FPTZ_LENS_REAR) { rx = -rx; rz = -rz; }
    denominator = rz + c->xi * norm;
    /* Reject the folded Mei branch even if it maps inside the measured mask. */
    if (!isfinite(norm) || norm <= 0.0 || rz / norm <= -1.0 / c->xi ||
        !isfinite(denominator) || denominator <= 0.0) return 0;
    x = rx / denominator; y = ry / denominator;
    r2 = x * x + y * y;
    radial = 1.0 + c->D[0] * r2 + c->D[1] * r2 * r2 +
             c->D[4] * r2 * r2 * r2;
    xd = x * radial + 2.0 * c->D[2] * x * y +
         c->D[3] * (r2 + 2.0 * x * x);
    yd = y * radial + c->D[2] * (r2 + 2.0 * y * y) +
         2.0 * c->D[3] * x * y;
    mx = (float)(c->K[0][0] * xd + c->K[0][2]);
    my = (float)(c->K[1][1] * yd + c->K[1][2]);
    if (!isfinite(mx) || !isfinite(my) ||
        mx < 0.0f || mx >= (float)(FPTZ_SOURCE_WIDTH - 1u) ||
        my < 0.0f || my >= (float)(FPTZ_SOURCE_HEIGHT - 1u)) return 0;
    *qx = (int32_t)(mx * 64.0f);
    *qy = (int32_t)(my * 64.0f);
    return 1;
}
int fptz_lut_project_one_lens(uint32_t lens, int32_t pan, int32_t tilt,
                            uint32_t hfov, uint32_t x, uint32_t y,
                            int32_t *qx, int32_t *qy) {
    FptzLutGen g;
    double rx, ry, rz, norm;
    if (!qx || !qy) return -1;
    *qx = *qy = INT32_MIN;
    if (lens >= FPTZ_LENS_COUNT || x >= FPTZ_OUTPUT_WIDTH ||
        y >= FPTZ_OUTPUT_HEIGHT || configure(&g, pan, tilt, hfov)) return -1;
    world_ray(&g, x, y, &rx, &ry, &rz, &norm);
    return project(lens, rx, ry, rz, norm, qx, qy) ? 0 : -2;
}
int fptz_lut_project_one(int32_t pan, int32_t tilt, uint32_t hfov,
                       uint32_t x, uint32_t y, int32_t *qx, int32_t *qy) {
    return fptz_lut_project_one_lens(FPTZ_LENS_FRONT, pan, tilt, hfov,
                                   x, y, qx, qy);
}
int fptz_lut_gen_begin_dual(FptzLutGen *g, uint8_t *candidate,
                          const uint8_t *front_bits, const uint8_t *rear_bits,
                          int32_t pan, int32_t tilt, uint32_t hfov) {
    if (!g || !candidate || !front_bits || !rear_bits) return -1;
    memset(g, 0, sizeof(*g));
    if (configure(g, pan, tilt, hfov)) { g->failed = -1; return -1; }
    g->lut = candidate; g->scene_bits = front_bits; g->rear_scene_bits = rear_bits;
    for (unsigned j = 0; j < FPTZ_LENS_COUNT; ++j) {
        g->previous_min[j] = g->previous_max[j] = -1;
        g->row_min[j] = FPTZ_SOURCE_HEIGHT; g->row_max[j] = -1;
    }
    return 0;
}
int fptz_lut_gen_begin(FptzLutGen *g, uint8_t *candidate,
                     const uint8_t *front_bits, int32_t pan, int32_t tilt,
                     uint32_t hfov) {
    return fptz_lut_gen_begin_dual(g, candidate, front_bits,
                                 fptz_scene_mask_rear_bits, pan, tilt, hfov);
}
uint16_t fptz_lut_weight_q8(double weight) {
    double scaled = weight * 256.0;
    unsigned integral = (unsigned)scaled;
    double fraction = scaled - integral;
    if (fraction > 0.5 || (fraction == 0.5 && (integral & 1u))) ++integral;
    return (uint16_t)integral;
}
static uint16_t weight(double rz, double norm, int front, int rear) {
    double t, a, b;
    if (!rear) return 256u;
    if (!front) return 0u;
    /* Same 20-degree static seam and normalization order as direct_map. */
    t = 0.5 + 0.5 * (2.0 * (rz / norm)) / (2.0 * sin(10.0 * DEG_TO_RAD));
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    t = t * t * (3.0 - 2.0 * t);
    a = t; b = 1.0 - t;
    return fptz_lut_weight_q8(a / (a + b));
}
static void put32(uint8_t *p, int32_t value) {
    uint32_t v = (uint32_t)value;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int generate_pixels(FptzLutGen *g, uint32_t max_pixels) {
    for (uint32_t processed = 0;
         processed < max_pixels && g->next_row < FPTZ_OUTPUT_HEIGHT; ++processed) {
        uint32_t index = g->next_row * FPTZ_OUTPUT_WIDTH + g->next_col;
        int32_t qx[2], qy[2];
        int valid[2];
        uint16_t w;
        double rx, ry, rz, norm;
        world_ray(g, g->next_col, g->next_row, &rx, &ry, &rz, &norm);
        for (unsigned j = 0; j < FPTZ_LENS_COUNT; ++j) {
            const uint8_t *bits = j ? g->rear_scene_bits : g->scene_bits;
            const uint8_t *pairs = j ? g->rear_scene_pair_bits : g->scene_pair_bits;
            valid[j] = project(j, rx, ry, rz, norm, &qx[j], &qy[j]);
            ++g->projection_count;
            if (valid[j])
                valid[j] = pairs ? fptz_scene_valid_2x2_pair(pairs, qx[j] >> 6, qy[j] >> 6)
                                 : fptz_scene_valid_2x2_fast(bits, qx[j] >> 6, qy[j] >> 6);
            if (!valid[j]) qx[j] = qy[j] = INT32_MIN;
            /* Keep valid coordinates even when that lens has zero weight. */
            put32(g->lut + j * 2u * FPTZ_LUT_PLANE_BYTES + index * 4u, qx[j]);
            put32(g->lut + (j * 2u + 1u) * FPTZ_LUT_PLANE_BYTES + index * 4u, qy[j]);
        }
        if (!valid[0] && !valid[1]) return -2;
        w = weight(rz, norm, valid[0], valid[1]);
        g->lut[FPTZ_LUT_W_OFFSET + index * 2u] = (uint8_t)w;
        g->lut[FPTZ_LUT_W_OFFSET + index * 2u + 1u] = (uint8_t)(w >> 8);
        for (unsigned j = 0; j < FPTZ_LENS_COUNT; ++j) {
            if (j ? w == 256u : w == 0u) continue;
            if ((qy[j] >> 6) < g->row_min[j]) g->row_min[j] = qy[j] >> 6;
            if ((qy[j] >> 6) > g->row_max[j]) g->row_max[j] = qy[j] >> 6;
        }
        ++g->valid_pixels;
        if (++g->next_col == FPTZ_OUTPUT_WIDTH) {
            for (unsigned j = 0; j < FPTZ_LENS_COUNT; ++j) {
                if (g->row_max[j] >= 0) {
                    uint32_t span = (uint32_t)(g->row_max[j] - g->row_min[j] + 2);
                    if (span > g->lens_max_span[j]) g->lens_max_span[j] = span;
                    if (span > g->max_span) g->max_span = span;
                    if (span > FPTZ_CACHE_ROWS) return -2;
                    if (g->previous_min[j] >= 0 && g->row_min[j] < g->previous_min[j])
                        ++g->min_retreats[j];
                    if (g->previous_max[j] >= 0 && g->row_max[j] < g->previous_max[j])
                        ++g->max_retreats[j];
                    g->previous_min[j] = g->row_min[j]; g->previous_max[j] = g->row_max[j];
                }
                g->row_min[j] = FPTZ_SOURCE_HEIGHT; g->row_max[j] = -1;
            }
            g->next_col = 0; ++g->next_row;
        }
    }
    return g->next_row == FPTZ_OUTPUT_HEIGHT ? 1 : 0;
}
int fptz_lut_gen_pixels(FptzLutGen *g, uint32_t max_pixels) {
    uint64_t started, ended;
    int result;
    if (!g || !g->lut || !g->scene_bits || !g->rear_scene_bits || !max_pixels) return -1;
    if (g->failed) return g->failed;
    if (g->next_row == FPTZ_OUTPUT_HEIGHT) return 1;
    if (g->next_row > FPTZ_OUTPUT_HEIGHT) return -1;
    started = g->now_us ? g->now_us(g->clock_ctx) : 0;
    result = generate_pixels(g, max_pixels);
    if (g->now_us) {
        ended = g->now_us(g->clock_ctx);
        if (ended >= started) g->anchor_work_us += ended - started;
    }
    if (result < 0) g->failed = result;
    return result;
}
int fptz_lut_gen_rows(FptzLutGen *g, uint32_t max_rows) {
    uint32_t rows;
    if (!g || !max_rows || g->next_row > FPTZ_OUTPUT_HEIGHT) return -1;
    if (g->failed) return g->failed;
    if (g->next_row == FPTZ_OUTPUT_HEIGHT) return 1;
    rows = FPTZ_OUTPUT_HEIGHT - g->next_row;
    if (max_rows < rows) rows = max_rows;
    return fptz_lut_gen_pixels(g, rows * FPTZ_OUTPUT_WIDTH - g->next_col);
}
