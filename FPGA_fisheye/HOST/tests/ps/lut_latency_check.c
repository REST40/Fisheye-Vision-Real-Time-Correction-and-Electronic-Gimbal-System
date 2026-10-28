#include "fptz_lut_generate.h"
#include "fptz_lut.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int fptz_lut_gen_begin_dual_reference(FptzLutGen *,uint8_t *,const uint8_t *,const uint8_t *,int32_t,int32_t,uint32_t);
int fptz_lut_gen_pixels_reference(FptzLutGen *,uint32_t);
int main(void){
 uint8_t *a=malloc(FPTZ_LUT_BYTES),*b=malloc(FPTZ_LUT_BYTES);int tested=0;
 const int poses[][2]={{0,0},{45000,0},{90000,0},{135000,0},{180000,0},{-45000,0},{-90000,0},{-135000,0},{0,20000},{0,-20000},{45000,30000},{-45000,-30000},{90000,45000},{-90000,-45000},{0,90000},{0,-90000}};
 for(unsigned i=0;i<sizeof(poses)/sizeof(poses[0]);++i){
  FptzLutGen ga,gb;memset(a,0xa5,FPTZ_LUT_BYTES);memset(b,0xa5,FPTZ_LUT_BYTES);
  int ra=fptz_lut_gen_begin_dual_reference(&ga,a,fptz_scene_mask_front_bits,fptz_scene_mask_rear_bits,poses[i][0],poses[i][1],90000);
  int rb=fptz_lut_gen_begin_dual(&gb,b,fptz_scene_mask_front_bits,fptz_scene_mask_rear_bits,poses[i][0],poses[i][1],90000);
  while(!ra||!rb){if(!ra)ra=fptz_lut_gen_pixels_reference(&ga,997);if(!rb)rb=fptz_lut_gen_pixels(&gb,997);}
  if(ra!=rb||memcmp(a,b,FPTZ_LUT_BYTES)||ga.next_row!=gb.next_row||ga.next_col!=gb.next_col||ga.valid_pixels!=gb.valid_pixels||ga.projection_count!=gb.projection_count||memcmp(ga.lens_max_span,gb.lens_max_span,sizeof(ga.lens_max_span))){fprintf(stderr,"mismatch pose %u\n",i);return 1;}
  ++tested;
 }
 FptzLutGen g;FptzLutStats stats;int r=fptz_lut_gen_begin_panorama(&g,a,fptz_scene_mask_front_bits,fptz_scene_mask_rear_bits);
 while(!r)r=fptz_lut_gen_pixels(&g,997);
 if(r!=1||fptz_lut_validate(a,FPTZ_LUT_BYTES,&stats))return 2;
 printf("{\"perspective_poses_byte_identical\":%d,\"panorama_pixels\":%u,\"panorama_front_max_span\":%u,\"panorama_rear_max_span\":%u,\"full_validation_passed\":true}\n",tested,g.valid_pixels,g.lens_max_span[0],g.lens_max_span[1]);
 free(a);free(b);return 0;
}
