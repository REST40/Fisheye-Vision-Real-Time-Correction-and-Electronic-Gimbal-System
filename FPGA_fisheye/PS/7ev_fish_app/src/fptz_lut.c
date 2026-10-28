#include "fptz_lut.h"
#include "fptz_scene_mask.h"
#include <string.h>
static int32_t read_q6(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)v;
}
int fptz_lut_check_begin(FptzLutCheck *g,const uint8_t *raw,size_t bytes,
                        const uint8_t *front_bits,const uint8_t *rear_bits) {
    if (!g) return -1;
    memset(g,0,sizeof(*g));
    g->failed=-1;
    if (!raw || !front_bits || !rear_bits || bytes!=FPTZ_LUT_BYTES) return -1;
    g->raw=raw;g->mask[0]=front_bits;g->mask[1]=rear_bits;g->failed=0;
    for (unsigned j=0;j<FPTZ_LENS_COUNT;++j) {
        g->row_min[j]=FPTZ_SOURCE_HEIGHT;g->row_max[j]=-1;
        g->previous_min[j]=g->previous_max[j]=-1;
    }
    return 0;
}
int fptz_lut_check_pixels(FptzLutCheck *g,uint32_t budget) {
    if (!g || !g->raw || !g->mask[0] || !g->mask[1] || !budget) return -1;
    if (g->failed) return g->failed;
    if (g->next_row>FPTZ_OUTPUT_HEIGHT) return -1;
    for (uint32_t processed=0;processed<budget && g->next_row<FPTZ_OUTPUT_HEIGHT;++processed) {
        uint32_t index=g->next_row*FPTZ_OUTPUT_WIDTH+g->next_col;
        const uint8_t *wp=g->raw+FPTZ_LUT_W_OFFSET+index*2u;
        uint16_t w=(uint16_t)((uint16_t)wp[0]|((uint16_t)wp[1]<<8));
        if (w>256u) {g->failed=-1;return -1;}
        for (unsigned j=0;j<FPTZ_LENS_COUNT;++j) {
            int32_t qx,qy;
            if (j ? w==256u:w==0u) continue;
            qx=read_q6(g->raw+j*2u*FPTZ_LUT_PLANE_BYTES+index*4u);
            qy=read_q6(g->raw+(j*2u+1u)*FPTZ_LUT_PLANE_BYTES+index*4u);
            if (qx<0 || qx>=(int32_t)FPTZ_Q6_X_LIMIT ||
                qy<0 || qy>=(int32_t)FPTZ_Q6_Y_LIMIT ||
                !fptz_scene_valid_2x2_fast(g->mask[j],qx>>6,qy>>6)) {
                g->failed=-1;return -1;
            }
            if ((qy>>6)<g->row_min[j]) g->row_min[j]=qy>>6;
            if ((qy>>6)>g->row_max[j]) g->row_max[j]=qy>>6;
            ++g->stats.lens[j].sampled_pixels;
        }
        ++g->stats.valid_pixels;
        if (w && w<256u) ++g->stats.blended_pixels;
        if (++g->next_col!=FPTZ_OUTPUT_WIDTH) continue;
        for (unsigned j=0;j<FPTZ_LENS_COUNT;++j) {
            FptzLensLutStats *l=&g->stats.lens[j];
            if (g->row_max[j]<0) continue;
            uint32_t span=(uint32_t)(g->row_max[j]-g->row_min[j]+2);
            if (span>l->max_span) l->max_span=span;
            if (span>g->stats.max_span) g->stats.max_span=span;
            if (g->previous_min[j]>=0 && g->row_min[j]<g->previous_min[j]) {
                ++l->min_retreats;++g->stats.min_retreats;
            }
            if (g->previous_max[j]>=0 && g->row_max[j]<g->previous_max[j]) {
                ++l->max_retreats;++g->stats.max_retreats;
            }
            g->previous_min[j]=g->row_min[j];g->previous_max[j]=g->row_max[j];
            ++l->sampled_rows;
        }
        ++g->stats.valid_rows;
        if (g->stats.max_span>FPTZ_CACHE_ROWS) {g->failed=-1;return -1;}
        for (unsigned j=0;j<FPTZ_LENS_COUNT;++j) {
            g->row_min[j]=FPTZ_SOURCE_HEIGHT;g->row_max[j]=-1;
        }
        g->next_col=0;++g->next_row;
    }
    return g->next_row==FPTZ_OUTPUT_HEIGHT ? 1:0;
}
int fptz_lut_validate_masks(const uint8_t *raw,size_t bytes,
                           const uint8_t *front_bits,const uint8_t *rear_bits,
                           FptzLutStats *stats) {
    FptzLutCheck g;
    int result;
    if (!stats) return -1;
    memset(stats,0,sizeof(*stats));
    if (fptz_lut_check_begin(&g,raw,bytes,front_bits,rear_bits)) return -1;
    do {result=fptz_lut_check_pixels(&g,4096u);} while (!result);
    *stats=g.stats;
    return result==1 ? 0:-1;
}
int fptz_lut_validate(const uint8_t *raw,size_t bytes,FptzLutStats *stats) {
    return fptz_lut_validate_masks(raw,bytes,fptz_scene_mask_front_bits,
                                  fptz_scene_mask_rear_bits,stats);
}
