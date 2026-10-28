#include "fptz_media_hw.h"

#include <string.h>

#define DMA_MM2S 0x00u
#define DMA_S2MM 0x30u
#define DMA_ERROR_BITS 0x770u
#define DMA_IOC 0x1000u
#define HW_TIMEOUT_US 5000000u
#define ENC_STREAM_SELECT (1u << 23)
#define ENC_GEOMETRY (((FPTZ_OUTPUT_WIDTH & 0x7FFu) << 1) | \
                      ((FPTZ_OUTPUT_HEIGHT & 0x7FFu) << 12) | \
                      (((FPTZ_OUTPUT_WIDTH >> 11) & 1u) << 24))
_Static_assert((ENC_GEOMETRY | ENC_STREAM_SELECT) == 0x01D304C0u, "encoder geometry ABI4 mismatch");

enum {
    HW_FREE, HW_DEC_READY, HW_DEC_ACTIVE, HW_CORR_READY,
    HW_CORR_ACTIVE, HW_ENC_READY, HW_ENC_RESET, HW_ENC_ACTIVE, HW_DONE
};

static uint32_t read_reg(FptzMediaHw *hw, uint32_t base, uint32_t offset) {
    return hw->ops.read(hw->ops.ctx, base, offset);
}

static void write_reg(FptzMediaHw *hw, uint32_t base, uint32_t offset,
                      uint32_t value) {
    hw->ops.write(hw->ops.ctx, base, offset, value);
}

static uint64_t time_us(FptzMediaHw *hw) {
    return hw->ops.now_us(hw->ops.ctx);
}

static uint32_t queue_wait_us(uint64_t now, uint64_t ready) {
    uint64_t delta = now >= ready ? now - ready : 0;
    return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

/* Fail-stop: buffers remain owned until system reset; never reuse partial data. */
static void stop_on_fault(FptzMediaHw *hw) {
    if (!hw->fault || hw->fault_stopped) return;
    hw->fault_stopped = 1;
    write_reg(hw, FPTZ_GPIO_BASE, 0, 0);
    write_reg(hw, FPTZ_DMA_BASE, DMA_S2MM, 4);
}

static uint32_t corrector_read(void *ctx, uint32_t offset) {
    return read_reg((FptzMediaHw *)ctx, FPTZ_FISH_BASE, offset);
}

static void corrector_write(void *ctx, uint32_t offset, uint32_t value) {
    write_reg((FptzMediaHw *)ctx, FPTZ_FISH_BASE, offset, value);
}

enum { DMA_RGB, DMA_RGB_REAR, DMA_CORRECTED, DMA_OUTPUT };

static int read_only_destination(const FptzMediaHw *hw, uintptr_t address,
                                 uint32_t length, unsigned kind) {
    for (unsigned i = 0; i < FPTZ_FRAME_SLOTS; ++i) {
        const FptzDmaReadOnlySlot *r = &hw->read_only_slots[i];
        const FptzFrameSlot *s = r->slot;
        if (!s || r->rgb != (uintptr_t)s->rgb ||
            r->rgb_rear != (uintptr_t)s->rgb_rear ||
            r->corrected != (uintptr_t)s->corrected ||
            r->output != (uintptr_t)s->output) continue;
        uintptr_t expected = kind == DMA_RGB ? r->rgb :
                             kind == DMA_RGB_REAR ? r->rgb_rear :
                             kind == DMA_CORRECTED ? r->corrected : r->output;
        uint32_t bytes = kind == DMA_OUTPUT ? FPTZ_MAX_JPEG_BYTES :
                         (kind == DMA_RGB || kind == DMA_RGB_REAR) ? FPTZ_SOURCE_RGBX_BYTES : FPTZ_OUTPUT_RGBX_BYTES;
        if (address == expected && length == bytes) return 1;
    }
    return 0;
}

static void corrector_invalidate(void *ctx, uint32_t address,
                                 uint32_t length) {
    FptzMediaHw *hw = (FptzMediaHw *)ctx;
    if (!read_only_destination(hw, (uintptr_t)address, length, DMA_CORRECTED))
        hw->ops.invalidate(hw->ops.ctx, (uintptr_t)address, length);
}

static uint64_t corrector_now(void *ctx) {
    return time_us((FptzMediaHw *)ctx);
}

static int valid_address(uintptr_t address, uint32_t length) {
    return address && !(address & 63u) &&
           (uint64_t)address + length <= 0x80000000ull;
}

int fptz_media_init(FptzMediaHw *hw, const FptzMediaOps *ops) {
    if (!hw || !ops || !ops->read || !ops->write || !ops->flush ||
        !ops->invalidate || !ops->now_us) return -1;
    memset(hw, 0, sizeof(*hw));
    hw->ops = *ops;
    hw->dec_active[0] = hw->dec_active[1] = hw->corr_active = hw->enc_active = -1;
    hw->corrector_ops.read = corrector_read;
    hw->corrector_ops.write = corrector_write;
    hw->corrector_ops.invalidate = corrector_invalidate;
    hw->corrector_ops.now_us = corrector_now;
    hw->corrector_ops.ctx = hw;
    if (fptz_corrector_check_capabilities(&hw->corrector_ops)) {
        hw->fault = 1;
        return -2; /* No frame/DMA admission with the old bitstream. */
    }
    return 0;
}

int fptz_media_prepare_read_only_slot(FptzMediaHw *hw, FptzFrameSlot *slot) {
    int index = -1;
    if (!hw || hw->fault || !slot || (hw->dec_active[0] >= 0 || hw->dec_active[1] >= 0) ||
        hw->corr_active >= 0 || hw->enc_active >= 0 ||
        !valid_address((uintptr_t)slot->rgb, FPTZ_SOURCE_RGBX_BYTES) ||
        !valid_address((uintptr_t)slot->rgb_rear, FPTZ_SOURCE_RGBX_BYTES) ||
        !valid_address((uintptr_t)slot->corrected, FPTZ_OUTPUT_RGBX_BYTES) ||
        !valid_address((uintptr_t)slot->output, FPTZ_MAX_JPEG_BYTES)) return -1;
    for (unsigned i = 0; i < FPTZ_FRAME_SLOTS; ++i) {
        if (hw->frames[i].stage != HW_FREE) return -1;
        if (hw->read_only_slots[i].slot == slot) index = (int)i;
    }
    if (index < 0)
        for (unsigned i = 0; i < FPTZ_FRAME_SLOTS; ++i)
            if (!hw->read_only_slots[i].slot) { index = (int)i; break; }
    if (index < 0) return -1;
    /* Remove startup .bss dirt and any prior CPU writes before the first DMA.
     * BSP flush supplies the barrier. Subsequent CPU JPEG reads create only
     * clean lines, so they cannot overwrite PL data on eviction. Still
     * invalidate the actual JPEG range after EVERY completed S2MM transfer. */
    hw->ops.flush(hw->ops.ctx, (uintptr_t)slot->rgb, FPTZ_SOURCE_RGBX_BYTES);
    hw->ops.flush(hw->ops.ctx, (uintptr_t)slot->rgb_rear, FPTZ_SOURCE_RGBX_BYTES);
    hw->ops.flush(hw->ops.ctx, (uintptr_t)slot->corrected, FPTZ_OUTPUT_RGBX_BYTES);
    hw->ops.flush(hw->ops.ctx, (uintptr_t)slot->output, FPTZ_MAX_JPEG_BYTES);
    hw->read_only_slots[index].slot = slot;
    hw->read_only_slots[index].rgb = (uintptr_t)slot->rgb;
    hw->read_only_slots[index].rgb_rear = (uintptr_t)slot->rgb_rear;
    hw->read_only_slots[index].corrected = (uintptr_t)slot->corrected;
    hw->read_only_slots[index].output = (uintptr_t)slot->output;
    return 0;
}

void fptz_media_lut_published(FptzMediaHw *hw, const uint8_t *lut,
                              uint32_t length) {
    if (!hw || !lut || length != FPTZ_LUT_BYTES ||
        !valid_address((uintptr_t)lut, length)) {
        if (hw) hw->fault = 1;
        return;
    }
    hw->ops.flush(hw->ops.ctx, (uintptr_t)lut, length);
    hw->lut = lut;
}

static int valid_slot(const FptzFrameSlot *slot, const uint8_t *lut) {
    uintptr_t address[6];
    uint32_t bytes[6];
    if (!slot || !slot->input || !slot->pair_valid || !lut ||
        slot->input_len > FPTZ_MAX_INPUT_BYTES || slot->input_len < FPTZ_PAIR_HEADER_BYTES) return 0;
    if (slot->jpeg_pair.offset[0] != FPTZ_PAIR_HEADER_BYTES ||
        slot->jpeg_pair.length[0] < 4u || slot->jpeg_pair.length[1] < 4u ||
        slot->jpeg_pair.length[0] > FPTZ_MAX_JPEG_BYTES ||
        slot->jpeg_pair.length[1] > FPTZ_MAX_JPEG_BYTES ||
        slot->jpeg_pair.offset[1] != FPTZ_PAIR_HEADER_BYTES + FPTZ_PAIR_JPEG_BLOCK(slot->jpeg_pair.length[0]) ||
        slot->input_len != slot->jpeg_pair.offset[1] + FPTZ_PAIR_JPEG_BLOCK(slot->jpeg_pair.length[1])) return 0;
    address[0]=(uintptr_t)slot->input; bytes[0]=slot->input_len;
    address[1]=(uintptr_t)slot->rgb; bytes[1]=FPTZ_SOURCE_RGBX_BYTES;
    address[2]=(uintptr_t)slot->rgb_rear; bytes[2]=FPTZ_SOURCE_RGBX_BYTES;
    address[3]=(uintptr_t)slot->corrected; bytes[3]=FPTZ_OUTPUT_RGBX_BYTES;
    address[4]=(uintptr_t)slot->output; bytes[4]=FPTZ_MAX_JPEG_BYTES;
    address[5]=(uintptr_t)lut; bytes[5]=FPTZ_LUT_BYTES;
    for (unsigned i=0;i<6;++i) {
        if (!valid_address(address[i],bytes[i])) return 0;
        for (unsigned j=0;j<i;++j)
            if ((uint64_t)address[i] < (uint64_t)address[j]+bytes[j] &&
                (uint64_t)address[j] < (uint64_t)address[i]+bytes[i]) return 0;
    }
    /* The server verified CRC/format/padding before marking pair_valid. Input
     * storage remains immutable until both decoders and this frame drain. */
    return fptz_get32(slot->input)==slot->jpeg_pair.length[0] &&
           fptz_get32(slot->input+4)==slot->jpeg_pair.length[1];
}
/* These are scheduler-observed intervals, not hardware cycle counters.
 * Reset anchors at pause/test/session boundaries so idle tests never inflate gaps. */
void fptz_media_set_flow_active(FptzMediaHw *hw, int active) {
    if (!hw) return;
    hw->flow_active = active != 0;
    memset(hw->flow_started_us, 0, sizeof(hw->flow_started_us));
    memset(hw->flow_completed_us, 0, sizeof(hw->flow_completed_us));
}
static void flow_start(FptzMediaHw *hw, unsigned stage, uint64_t now) {
    if (!hw->flow_active) return;
    FptzStageFlow *s=&hw->stats.flow[stage];
    uint64_t previous=hw->flow_completed_us[stage];
    if (previous && now>=previous) {
        uint64_t gap=now-previous;
        s->idle_us+=gap; ++s->gaps;
        uint32_t bounded=gap>UINT32_MAX ? UINT32_MAX:(uint32_t)gap;
        if (bounded>s->max_gap_us) s->max_gap_us=bounded;
    }
    hw->flow_started_us[stage]=now;
}
static void flow_complete(FptzMediaHw *hw, unsigned stage, uint64_t now) {
    uint64_t start=hw->flow_started_us[stage];
    if (!hw->flow_active || !start || now<start) return;
    FptzStageFlow *s=&hw->stats.flow[stage];
    s->busy_us+=now-start; ++s->completed;
    uint64_t previous=hw->flow_completed_us[stage];
    if (previous && now>=previous) { s->interval_us+=now-previous; ++s->intervals; }
    hw->flow_completed_us[stage]=now;
    hw->flow_started_us[stage]=0;
}
int fptz_media_start(FptzMediaHw *hw, FptzFrameSlot *slot, const uint8_t *lut) {
    if (!hw || hw->fault || !slot || hw->lut!=lut || !valid_slot(slot,lut)) return -1;
    if (hw->dec_active[0]>=0 || hw->dec_active[1]>=0) return 1;
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i)
        if (hw->frames[i].stage!=HW_FREE &&
            (hw->frames[i].slot==slot || hw->frames[i].stage==HW_DEC_READY)) return 1;
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i) {
        if (hw->frames[i].stage!=HW_FREE) continue;
        memset(&hw->frames[i],0,sizeof(hw->frames[i]));
        hw->frames[i].slot=slot; hw->frames[i].stage=HW_DEC_READY;
        hw->frames[i].stream=1; hw->frames[i].lut=lut;
        hw->frames[i].order=++hw->next_order;
        hw->frames[i].stage_ready_us=time_us(hw);
        slot->decode_wait_us=slot->correct_wait_us=slot->encode_wait_us=0;
        slot->decode_us=slot->decode_front_us=slot->decode_rear_us=0;
        slot->correct_us=slot->encode_us=slot->output_len=slot->frame_cycles=0;
        slot->sources_decoded=slot->corrected_valid=0;
        return 0;
    }
    return 1;
}
static uint32_t decoder_base(unsigned lens) {
    return lens ? FPTZ_DEC_REAR_BASE : FPTZ_DEC_BASE;
}
static void decoder_fault(FptzMediaHw *hw, unsigned lens, int timeout) {
    if (timeout) { ++hw->stats.decode_timeout; ++hw->stats.decode_lens_timeout[lens]; }
    else { ++hw->stats.decode_failed; ++hw->stats.decode_lens_failed[lens]; }
    hw->fault=1;
}
static void begin_decoder_pair(FptzMediaHw *hw,int index) {
    FptzHwFrame *f=&hw->frames[index];
    FptzFrameSlot *slot=f->slot;
    if (hw->dec_active[0]>=0 || hw->dec_active[1]>=0) { hw->fault=1; return; }
    /* Check both idle before any START. A fault retains every owned buffer. */
    for (unsigned j=0;j<2;++j)
        if (read_reg(hw,decoder_base(j),4)&7u) { decoder_fault(hw,j,0); return; }
    hw->ops.flush(hw->ops.ctx,(uintptr_t)slot->input,slot->input_len);
    slot->sources_decoded=slot->corrected_valid=0;
    f->stage=HW_DEC_ACTIVE;
    slot->decode_wait_us=queue_wait_us(time_us(hw),f->stage_ready_us);
    ++hw->stats.decode_started; /* One logical frame, two hardware starts. */
    if (f->stream) flow_start(hw,0,time_us(hw));
    for (unsigned j=0;j<2;++j) {
        uint32_t base=decoder_base(j);
        uintptr_t target=(uintptr_t)(j ? slot->rgb_rear:slot->rgb);
        FptzDecoderRun *d=&f->decoder[j];
        if (!read_only_destination(hw,target,FPTZ_SOURCE_RGBX_BYTES,j?DMA_RGB_REAR:DMA_RGB))
            hw->ops.invalidate(hw->ops.ctx,target,FPTZ_SOURCE_RGBX_BYTES);
        d->previous_generation=(uint8_t)read_reg(hw,base,0x18u);
        d->generation=(uint8_t)(d->previous_generation+1u);
        write_reg(hw,base,8,(uint32_t)((uintptr_t)slot->input+slot->jpeg_pair.offset[j]));
        write_reg(hw,base,12,(uint32_t)target);
        d->started_us=time_us(hw); d->started=1;
        slot->stage_times_us[j*2]=d->started_us;
        write_reg(hw,base,0,0x80000000u|slot->jpeg_pair.length[j]);
        hw->dec_active[j]=(int8_t)index;
        ++hw->stats.decode_lens_started[j];
    }
}
static void poll_decoders(FptzMediaHw *hw) {
    for (unsigned j=0;j<2;++j) {
        int index=hw->dec_active[j];
        uint32_t base,meta,status,rows;
        uint64_t elapsed;
        FptzHwFrame *f;
        FptzDecoderRun *d;
        if (index<0) continue;
        f=&hw->frames[index]; d=&f->decoder[j]; base=decoder_base(j);
        elapsed=time_us(hw)-d->started_us;
        if (elapsed>HW_TIMEOUT_US) { decoder_fault(hw,j,1); return; }
        meta=read_reg(hw,base,0x18u);
        if ((uint8_t)meta!=d->generation) {
            if ((uint8_t)meta==d->previous_generation && !d->accepted) continue;
            decoder_fault(hw,j,0); return;
        }
        d->accepted=1;
        status=read_reg(hw,base,4); rows=read_reg(hw,base,0x14u);
        if ((status&6u) || (meta&(1u<<9)) || rows>FPTZ_SOURCE_HEIGHT) {
            decoder_fault(hw,j,0); return;
        }
        if ((status&1u) || !(meta&(1u<<8))) continue;
        if (rows!=FPTZ_SOURCE_HEIGHT ||
            read_reg(hw,base,16)!=FPTZ_SOURCE_PIXELS ||
            read_reg(hw,base,12)!=(uint32_t)(uintptr_t)(j?f->slot->rgb_rear:f->slot->rgb)) {
            decoder_fault(hw,j,0); return;
        }
        d->done=1; hw->dec_active[j]=-1;
        f->slot->stage_times_us[j*2+1]=d->started_us+elapsed;
        hw->stats.decode_lens_last_us[j]=(uint32_t)elapsed;
        if (j) f->slot->decode_rear_us=(uint32_t)elapsed;
        else f->slot->decode_front_us=(uint32_t)elapsed;
        ++hw->stats.decode_lens_completed[j];
        if (f->decoder[0].done && f->decoder[1].done) {
            f->slot->sources_decoded=1;
            f->slot->decode_us=(uint32_t)(time_us(hw)-f->decoder[0].started_us);
            f->stage_ready_us=time_us(hw);
            f->stage=HW_CORR_READY;
            ++hw->stats.decode_completed;
            if (f->stream) flow_complete(hw,0,time_us(hw));
        }
    }
}

static void begin_corrector(FptzMediaHw *hw, int index) {
    FptzHwFrame *frame = &hw->frames[index];
    FptzFrameSlot *slot = frame->slot;
    uintptr_t x = (uintptr_t)frame->lut;
    if (!slot->sources_decoded ||
        (frame->stream && (!frame->decoder[0].done || !frame->decoder[1].done))) {
        ++hw->stats.correct_failed; hw->fault=1; return;
    }
    slot->corrected_valid=0;
    const FptzCorrectorFrame addresses = {
        (uint32_t)(uintptr_t)slot->rgb, (uint32_t)(uintptr_t)slot->rgb_rear,
        (uint32_t)(x + FPTZ_LUT_XF_OFFSET), (uint32_t)(x + FPTZ_LUT_YF_OFFSET),
        (uint32_t)(x + FPTZ_LUT_XR_OFFSET), (uint32_t)(x + FPTZ_LUT_YR_OFFSET),
        (uint32_t)(x + FPTZ_LUT_W_OFFSET), (uint32_t)(uintptr_t)slot->corrected
    };
    if (fptz_corrector_start_dual_config(&hw->corrector, &hw->corrector_ops,
                                        &addresses, FPTZ_PL_CENTER_X, FPTZ_PL_CENTER_Y,
                                        frame->stream ? FPTZ_PIPE_STREAM : 0u)) {
        ++hw->stats.correct_failed;
        hw->fault = 1;
        return;
    }
    frame->corr_started_us = time_us(hw);
    slot->stage_times_us[4]=frame->corr_started_us;
    if (frame->stream) flow_start(hw,1,frame->corr_started_us);
    slot->correct_wait_us=queue_wait_us(frame->corr_started_us,frame->stage_ready_us);
    if (!frame->stream) {
        frame->started_us = frame->corr_started_us;
        frame->stage = HW_CORR_ACTIVE;
    }
    frame->corr_started = 1;
    hw->corr_active = (int8_t)index;
    ++hw->stats.correct_started;
}

static void poll_corrector(FptzMediaHw *hw) {
    FptzHwFrame *frame;
    int result;
    if (hw->corr_active < 0) return;
    frame = &hw->frames[hw->corr_active];
    result = fptz_corrector_poll(&hw->corrector, &hw->corrector_ops,
                                 HW_TIMEOUT_US);
    if (result < 0) {
        if (result == -3) ++hw->stats.correct_timeout;
        else ++hw->stats.correct_failed;
        hw->fault = 1;
        return;
    }
    if (result == 1) {
        frame->slot->correct_us = (uint32_t)(time_us(hw) - frame->corr_started_us);
        frame->slot->stage_times_us[5]=frame->corr_started_us+frame->slot->correct_us;
        frame->slot->frame_cycles = hw->corrector.frame_cycles;
        ++hw->stats.correct_completed;
        if (frame->stream) flow_complete(hw,1,time_us(hw));
        frame->corr_done = 1;
        frame->slot->corrected_valid=1;
        if (!frame->stream) {
            frame->stage_ready_us=time_us(hw);
            frame->stage = HW_ENC_READY;
        }
        hw->corr_active = -1;
    }
}

static void begin_encoder(FptzMediaHw *hw, int index) {
    FptzHwFrame *frame = &hw->frames[index];
    /* Select must settle while run=0, before any pixel can enter the encoder. */
    write_reg(hw, FPTZ_GPIO_BASE, 0,
              ENC_GEOMETRY | (frame->stream ? ENC_STREAM_SELECT : 0u));
    write_reg(hw, FPTZ_DMA_BASE, DMA_MM2S, 4);
    frame->deadline_us = time_us(hw) + 100000u;
    frame->stage = HW_ENC_RESET;
    hw->enc_active = (int8_t)index;
}

static void poll_encoder(FptzMediaHw *hw) {
    FptzHwFrame *frame;
    FptzFrameSlot *slot;
    uint64_t now;
    if (hw->enc_active < 0) return;
    frame = &hw->frames[hw->enc_active];
    slot = frame->slot;
    now = time_us(hw);
    if (now > frame->deadline_us) {
        ++hw->stats.encode_timeout;
        hw->fault = 1;
        return;
    }
    if (frame->stage == HW_ENC_RESET) {
        if (read_reg(hw, FPTZ_DMA_BASE, DMA_MM2S) & 4u) return;
        if (!read_only_destination(hw, (uintptr_t)slot->output,
                                   FPTZ_MAX_JPEG_BYTES, DMA_OUTPUT))
            hw->ops.invalidate(hw->ops.ctx, (uintptr_t)slot->output,
                               FPTZ_MAX_JPEG_BYTES);
        write_reg(hw, FPTZ_DMA_BASE, DMA_MM2S + 4, 0x7000u);
        write_reg(hw, FPTZ_DMA_BASE, DMA_S2MM + 4, 0x7000u);
        write_reg(hw, FPTZ_DMA_BASE, DMA_S2MM, 1);
        write_reg(hw, FPTZ_DMA_BASE, DMA_S2MM + 0x18,
                  (uint32_t)(uintptr_t)slot->output);
        write_reg(hw, FPTZ_DMA_BASE, DMA_S2MM + 0x28,
                  FPTZ_MAX_JPEG_BYTES);
        frame->enc_started_us = now;
        slot->stage_times_us[6]=now;
        if (frame->stream) flow_start(hw,2,now);
        slot->encode_wait_us=queue_wait_us(now,frame->stage_ready_us);
        if (!frame->stream) frame->started_us = now;
        write_reg(hw, FPTZ_GPIO_BASE, 0, 1u | ENC_GEOMETRY |
                  (frame->stream ? ENC_STREAM_SELECT : 0u));
        if (!frame->stream) {
            write_reg(hw, FPTZ_DMA_BASE, DMA_MM2S, 1);
            write_reg(hw, FPTZ_DMA_BASE, DMA_MM2S + 0x18,
                      (uint32_t)(uintptr_t)slot->corrected);
            write_reg(hw, FPTZ_DMA_BASE, DMA_MM2S + 0x28,
                      FPTZ_OUTPUT_RGBX_BYTES);
        }
        frame->deadline_us = now + HW_TIMEOUT_US;
        frame->stage = HW_ENC_ACTIVE;
        frame->enc_started = 1;
        ++hw->stats.encode_started;
    } else {
        uint32_t wr = read_reg(hw, FPTZ_DMA_BASE, DMA_S2MM + 4);
        uint32_t rd = frame->stream ? 0u :
                      read_reg(hw, FPTZ_DMA_BASE, DMA_MM2S + 4);
        uint32_t status = read_reg(hw, FPTZ_GPIO_BASE, 8);
        if (((wr | rd) & DMA_ERROR_BITS) || (status & 30u)) {
            ++hw->stats.encode_failed;
            hw->fault = 1;
            return;
        }
        if ((wr & DMA_IOC) && (frame->stream || (rd & DMA_IOC)) &&
            (status & 1u)) {
            slot->encode_us = (uint32_t)(now - frame->enc_started_us);
            slot->stage_times_us[7]=now;
            slot->output_len = read_reg(hw, FPTZ_DMA_BASE,
                                         DMA_S2MM + 0x28);
            if (slot->output_len < 4 ||
                slot->output_len > FPTZ_MAX_JPEG_BYTES) {
                ++hw->stats.encode_failed;
                hw->fault = 1;
                return;
            }
            hw->ops.invalidate(hw->ops.ctx, (uintptr_t)slot->output,
                               slot->output_len);
            frame->enc_done = 1;
            if (!frame->stream) frame->stage = HW_DONE;
            hw->enc_active = -1;
            ++hw->stats.encode_completed;
            if (frame->stream) flow_complete(hw,2,time_us(hw));
        }
    }
}

static int oldest_stage(FptzMediaHw *hw, unsigned stage) {
    int found=-1;
    for (int i=0;i<(int)FPTZ_FRAME_SLOTS;++i) {
        FptzHwFrame *f=&hw->frames[i];
        if (!f->stream || f->stage!=stage) continue;
        if (found<0 || f->order<hw->frames[found].order) found=i;
    }
    return found;
}
static void release_frame(FptzMediaHw *hw,int index) {
    memset(&hw->frames[index],0,sizeof(hw->frames[index]));
    hw->frames[index].stage=HW_FREE;
}
static void pump(FptzMediaHw *hw) {
    int index;
    if (hw->fault) { stop_on_fault(hw); return; }
    poll_decoders(hw);
    if (hw->fault) { stop_on_fault(hw); return; }
    poll_corrector(hw);
    if (hw->fault) { stop_on_fault(hw); return; }
    poll_encoder(hw);
    if (hw->fault) { stop_on_fault(hw); return; }
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i) {
        FptzHwFrame *f=&hw->frames[i];
        if (f->stage!=HW_FREE && f->stream && f->decoder[0].done &&
            f->decoder[1].done && f->corr_done && f->enc_done) f->stage=HW_DONE;
    }
    /* Encoder/corrector consume one paired frame together. No source watermarks
     * are consulted after decode: WAIT_SOURCE=0 permits decoding the next slot. */
    if (hw->corr_active<0 && hw->enc_active<0) {
        index=oldest_stage(hw,HW_CORR_READY);
        if (index>=0) {
            begin_encoder(hw,index);
            /* One non-blocking check: if reset has already cleared, arm S2MM
             * now so correction can start in this same pump. Otherwise the
             * existing reset state/deadline is retained for the next poll. */
            poll_encoder(hw);
            if (hw->fault) { stop_on_fault(hw); return; }
        }
    }
    if (hw->enc_active>=0) {
        index=hw->enc_active;
        FptzHwFrame *f=&hw->frames[index];
        if (f->enc_started && !f->corr_started && hw->corr_active<0 &&
            f->decoder[0].done && f->decoder[1].done) begin_corrector(hw,index);
    }
    if (!hw->fault && hw->dec_active[0]<0 && hw->dec_active[1]<0) {
        index=oldest_stage(hw,HW_DEC_READY);
        if (index>=0) begin_decoder_pair(hw,index);
    }
    stop_on_fault(hw);
}
int fptz_media_poll(FptzMediaHw *hw,FptzFrameSlot *slot) {
    if (!hw || !slot) return -1;
    if (hw->fault) { stop_on_fault(hw); return -1; }
    pump(hw);
    if (hw->fault) return -1;
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i) {
        if (hw->frames[i].slot!=slot) continue;
        if (hw->frames[i].stage==HW_DONE) { release_frame(hw,(int)i); return 1; }
        return 0;
    }
    return -1;
}
int fptz_media_capacity_start(FptzMediaHw *hw,uint8_t stage,
                              FptzFrameSlot *slot,const uint8_t *lut) {
    unsigned initial;
    if (!hw || hw->fault || hw->lut!=lut || !valid_slot(slot,lut) ||
        stage<FPTZ_CAPACITY_DECODE || stage>FPTZ_CAPACITY_ENCODE ||
        hw->dec_active[0]>=0 || hw->dec_active[1]>=0 ||
        hw->corr_active>=0 || hw->enc_active>=0 ||
        (stage==FPTZ_CAPACITY_CORRECT && !slot->sources_decoded) ||
        (stage==FPTZ_CAPACITY_ENCODE && !slot->corrected_valid)) return -1;
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i)
        if (hw->frames[i].stage!=HW_FREE) return -1;
    initial=stage==FPTZ_CAPACITY_DECODE ? HW_DEC_READY :
            stage==FPTZ_CAPACITY_CORRECT ? HW_CORR_READY:HW_ENC_READY;
    memset(&hw->frames[0],0,sizeof(hw->frames[0]));
    hw->frames[0].slot=slot; hw->frames[0].lut=lut;
    hw->frames[0].stage=(uint8_t)initial;
    return 0;
}
int fptz_media_capacity_poll(FptzMediaHw *hw,uint8_t stage,FptzFrameSlot *slot) {
    FptzHwFrame *f=NULL;
    if (!hw || !slot || hw->fault || stage<FPTZ_CAPACITY_DECODE ||
        stage>FPTZ_CAPACITY_ENCODE) return -1;
    for (unsigned i=0;i<FPTZ_FRAME_SLOTS;++i)
        if (hw->frames[i].slot==slot && !hw->frames[i].stream) { f=&hw->frames[i]; break; }
    if (!f) return -1;
    if (stage==FPTZ_CAPACITY_DECODE) poll_decoders(hw);
    else if (stage==FPTZ_CAPACITY_CORRECT) poll_corrector(hw);
    else poll_encoder(hw);
    if (hw->fault) { stop_on_fault(hw); return -1; }
    if ((stage==FPTZ_CAPACITY_DECODE && f->stage==HW_CORR_READY) ||
        (stage==FPTZ_CAPACITY_CORRECT && f->stage==HW_ENC_READY) ||
        (stage==FPTZ_CAPACITY_ENCODE && f->stage==HW_DONE)) {
        release_frame(hw,(int)(f-hw->frames)); return 1;
    }
    if (stage==FPTZ_CAPACITY_DECODE && f->stage==HW_DEC_READY)
        begin_decoder_pair(hw,(int)(f-hw->frames));
    else if (stage==FPTZ_CAPACITY_CORRECT && f->stage==HW_CORR_READY)
        begin_corrector(hw,(int)(f-hw->frames));
    else if (stage==FPTZ_CAPACITY_ENCODE && f->stage==HW_ENC_READY)
        begin_encoder(hw,(int)(f-hw->frames));
    stop_on_fault(hw);
    return hw->fault ? -1:0;
}
void fptz_media_get_stats(const FptzMediaHw *hw,FptzPipelineStats *stats) {
    if (!stats) return;
    if (!hw) memset(stats,0,sizeof(*stats));
    else *stats=hw->stats;
}
