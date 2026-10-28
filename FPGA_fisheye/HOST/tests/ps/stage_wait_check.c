/* Production scheduler with deterministic register/clock mocks; no PL timing claim. */
#include "fptz_media_hw.c"
#include <stdio.h>
#include <assert.h>
static uint64_t test_now;
static uint32_t registers[4][64];
static int decoded,reset_busy;
static unsigned write_step,s2mm_step,encoder_step,corrector_step;
static unsigned engine(uint32_t base) {
    return base==FPTZ_DEC_BASE ? 0 : base==FPTZ_DEC_REAR_BASE ? 1 : base==FPTZ_FISH_BASE ? 2 : 3;
}
static uint32_t mock_read(void *ctx,uint32_t base,uint32_t offset) {
    (void)ctx;
    unsigned e=engine(base);
    if(e<2 && offset==0x18) return (decoded & (1u<<e)) ? 0x101u : 0;
    if(e<2 && offset==0x14) return (decoded & (1u<<e)) ? FPTZ_SOURCE_HEIGHT : 0;
    if(e<2 && offset==16) return (decoded & (1u<<e)) ? FPTZ_SOURCE_PIXELS : 0;
    if(e==3 && offset==0) return reset_busy ? 4u:0u; /* DMA reset completed */
    return registers[e][offset/4];
}
static void mock_write(void *ctx,uint32_t base,uint32_t offset,uint32_t value) {
    (void)ctx;unsigned e=engine(base);registers[e][offset/4]=value;
    ++write_step;
    if(base==FPTZ_DMA_BASE && offset==DMA_S2MM && value==1) s2mm_step=write_step;
    if(base==FPTZ_GPIO_BASE && offset==0 && (value&1)) encoder_step=write_step;
    if(base==FPTZ_FISH_BASE && offset==0 && value==1) corrector_step=write_step;
    if(e==2 && offset==0 && value==1) registers[e][1]=1; /* START accepted */
}
static void cache(void *ctx,uintptr_t address,uint32_t length) {(void)ctx;(void)address;(void)length;}
static uint64_t now(void *ctx) {(void)ctx;return test_now;}
static void setup(FptzMediaHw *hw,FptzFrameSlot *slot) {
    memset(hw,0,sizeof(*hw));memset(slot,0,sizeof(*slot));memset(registers,0,sizeof(registers));
    decoded=reset_busy=0;write_step=s2mm_step=encoder_step=corrector_step=0;
    hw->ops=(FptzMediaOps){.read=mock_read,.write=mock_write,.flush=cache,.invalidate=cache,.now_us=now};
    hw->dec_active[0]=hw->dec_active[1]=hw->corr_active=hw->enc_active=-1;
    hw->corrector_ops=(FptzCorrectorOps){.read=corrector_read,.write=corrector_write,.invalidate=corrector_invalidate,.now_us=corrector_now,.ctx=hw};
    registers[2][FPTZ_IP_SOURCE_SIZE/4]=FPTZ_PL_SOURCE_SIZE;
    registers[2][FPTZ_IP_OUTPUT_SIZE/4]=FPTZ_PL_OUTPUT_SIZE;
    registers[2][FPTZ_IP_CACHE_ROWS/4]=FPTZ_CACHE_ROWS;
    registers[2][FPTZ_IP_ABI_VERSION/4]=FPTZ_PL_ABI_VERSION;
    registers[2][FPTZ_IP_LENS_COUNT/4]=FPTZ_LENS_COUNT;
    registers[2][FPTZ_IP_LUT_FORMAT/4]=FPTZ_PL_LUT_FORMAT;
    registers[2][FPTZ_IP_CACHE_MODE/4]=FPTZ_PL_CACHE_MODE;
    slot->input=(uint8_t*)0x00800000;slot->rgb=(uint8_t*)0x01000000;
    slot->rgb_rear=(uint8_t*)0x02000000;slot->output=(uint8_t*)0x03000000;
    slot->corrected=(uint8_t*)0x10000000;
}
static int check_fast_start(int delayed,int timeout) {
    FptzMediaHw hw;FptzFrameSlot slot;setup(&hw,&slot);
    FptzHwFrame *f=&hw.frames[0];f->slot=&slot;f->stream=1;f->stage=HW_CORR_READY;
    f->lut=(uint8_t*)0x20000000;f->decoder[0].done=f->decoder[1].done=1;
    f->stage_ready_us=1000;slot.sources_decoded=1;test_now=2000;reset_busy=delayed;
    pump(&hw);
    if(delayed) {
        if(hw.fault || f->stage!=HW_ENC_RESET || f->enc_started || f->corr_started ||
           s2mm_step || encoder_step || corrector_step)return 1;
        if(timeout) {
            test_now=102001;pump(&hw);
            return !(hw.fault && hw.stats.encode_timeout==1 && !f->corr_started && !corrector_step);
        }
        test_now=3000;reset_busy=0;pump(&hw);
    }
    return hw.fault || !f->enc_started || !f->corr_started || hw.stats.encode_started!=1 ||
        hw.stats.correct_started!=1 || !s2mm_step || !(s2mm_step<encoder_step && encoder_step<corrector_step);
}
static void check_staggered(unsigned first) {
    FptzMediaHw hw;FptzFrameSlot slot;setup(&hw,&slot);
    FptzHwFrame *f=&hw.frames[0];f->slot=&slot;f->stream=1;f->stage=HW_DEC_READY;
    f->lut=(uint8_t*)0x20000000;f->stage_ready_us=100;
    test_now=1000;begin_decoder_pair(&hw,0);
    test_now=2000;decoded=1u<<first;poll_decoders(&hw);
    assert(!slot.sources_decoded && slot.stage_times_us[first*2+1]==2000);
    test_now=3000;decoded=3;poll_decoders(&hw);
    assert(slot.sources_decoded && slot.decode_us==2000 && slot.stage_times_us[(1-first)*2+1]==3000);
}
int main(void) {
    check_staggered(0);check_staggered(1);
    FptzMediaHw hw;FptzFrameSlot slot;setup(&hw,&slot);
    FptzHwFrame *f=&hw.frames[0];f->slot=&slot;f->stream=1;f->stage=HW_DEC_READY;
    f->lut=(uint8_t*)0x20000000;f->stage_ready_us=100;
    fptz_media_set_flow_active(&hw,1);
    test_now=1000;begin_decoder_pair(&hw,0);
    if(slot.decode_wait_us!=900 || hw.fault)return 1;
    test_now=2000;decoded=3;poll_decoders(&hw);
    if(f->stage!=HW_CORR_READY || f->stage_ready_us!=2000 || !slot.sources_decoded ||
       hw.stats.flow[0].completed!=1 || hw.stats.flow[0].busy_us!=1000)return 2;
    begin_encoder(&hw,0);test_now=2500;poll_encoder(&hw);
    if(slot.encode_wait_us!=500 || !f->enc_started || hw.fault)return 3;
    test_now=3000;begin_corrector(&hw,0);
    if(slot.correct_wait_us!=1000 || !f->corr_started || hw.fault)return 4;
    assert(slot.stage_times_us[6]==2500 && slot.stage_times_us[4]==3000);
    test_now=5000;registers[2][1]=18;registers[2][0x18u/4]=1234;poll_corrector(&hw);
    assert(f->corr_done && slot.stage_times_us[5]==5000 && slot.frame_cycles==1234);
    test_now=5500;registers[3][(DMA_S2MM+4)/4]=DMA_IOC;registers[3][2]=1;registers[3][(DMA_S2MM+0x28)/4]=100;poll_encoder(&hw);
    assert(f->enc_done && slot.stage_times_us[7]==5500 && slot.encode_us==3000);
    memset(hw.stats.flow,0,sizeof(hw.stats.flow));
    /* Deterministic scheduler timing, including isolation boundaries. */
    fptz_media_set_flow_active(&hw,1);
    flow_start(&hw,0,10000); flow_complete(&hw,0,30000);
    flow_start(&hw,0,35000); flow_complete(&hw,0,55000);
    FptzStageFlow *t=&hw.stats.flow[0];
    if(t->completed!=2 || t->busy_us!=40000 || t->gaps!=1 || t->idle_us!=5000 ||
       t->intervals!=1 || t->interval_us!=25000 || t->max_gap_us!=5000)return 5;
    fptz_media_set_flow_active(&hw,0);
    flow_start(&hw,0,100000);flow_complete(&hw,0,200000);
    if(t->completed!=2)return 6;
    fptz_media_set_flow_active(&hw,1);
    flow_start(&hw,0,9000000);flow_complete(&hw,0,9020000);
    if(t->completed!=3 || t->gaps!=1 || t->intervals!=1 || t->busy_us!=60000)return 7;
    flow_start(&hw,1,10000000);fptz_media_set_flow_active(&hw,0);
    fptz_media_set_flow_active(&hw,1);flow_complete(&hw,1,11000000);
    if(hw.stats.flow[1].completed)return 8; /* Exclude a frame crossing pause. */
    if(check_fast_start(0,0) || check_fast_start(1,0) || check_fast_start(1,1))return 10;
    puts("{\"decode_wait_us\":900,\"encoder_wait_us\":500,\"corrector_wait_us\":1000,\"overlapping_queues_verified\":true,\"flow_intervals_and_pause_boundaries_verified\":true,\"immediate_and_delayed_reset_order_and_timeout_verified\":true}");
    return 0;
}
