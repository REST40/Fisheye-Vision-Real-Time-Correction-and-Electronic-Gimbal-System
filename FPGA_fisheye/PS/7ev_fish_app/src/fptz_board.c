/* ZynqMP standalone/lwIP adapter for the portable UDP and media state machines. */
#include "fptz_board.h"

#include <stdint.h>
#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xtime_l.h"

#include "fptz_media_hw.h"
#include "fptz_camera.h"
#include "fptz_scene_mask.h"
#include "fptz_server.h"

typedef struct {
    uint8_t input[FPTZ_MAX_INPUT_BYTES] __attribute__((aligned(64)));
    uint8_t rgb[FPTZ_SOURCE_RGBX_BYTES] __attribute__((aligned(64)));
    uint8_t rgb_rear[FPTZ_SOURCE_RGBX_BYTES] __attribute__((aligned(64)));
    uint8_t corrected[FPTZ_OUTPUT_RGBX_BYTES] __attribute__((aligned(64)));
    uint8_t output[FPTZ_MAX_JPEG_BYTES] __attribute__((aligned(64)));
    uint8_t input_bitmap[FPTZ_INPUT_BITMAP_BYTES];
    uint8_t output_bitmap[FPTZ_OUTPUT_BITMAP_BYTES];
} BoardFrameMemory;

static BoardFrameMemory frame_memory[FPTZ_FRAME_SLOTS]
    __attribute__((aligned(64)));
static uint8_t lut_memory_a[FPTZ_LUT_BYTES] __attribute__((aligned(64)));
static uint8_t lut_memory_b[FPTZ_LUT_BYTES] __attribute__((aligned(64)));
static uint8_t scene_pair_memory[FPTZ_SCENE_PAIR_BYTES] __attribute__((aligned(64)));
static uint8_t rear_scene_pair_memory[FPTZ_SCENE_PAIR_BYTES] __attribute__((aligned(64)));
static uint8_t return_test_jpeg[FPTZ_MAX_JPEG_BYTES] __attribute__((aligned(64)));
static FptzFrameSlot slots[FPTZ_FRAME_SLOTS];
static FptzMediaHw media_hw;
static FptzServer server;
static struct udp_pcb *control_pcb, *input_pcb, *output_pcb;
static ip_addr_t host_ip;
static uint8_t rx_packet[FPTZ_HEADER_SIZE + FPTZ_MAX_PAYLOAD];
static uint8_t fault_reported;

static uint64_t board_now_us(void *ctx) {
    XTime ticks;
    (void)ctx;
    XTime_GetTime(&ticks);
    return ((uint64_t)ticks / COUNTS_PER_SECOND) * 1000000ull +
           ((uint64_t)ticks % COUNTS_PER_SECOND) * 1000000ull /
               COUNTS_PER_SECOND;
}

static uint32_t board_read(void *ctx, uint32_t base, uint32_t offset) {
    (void)ctx;
    return Xil_In32((UINTPTR)base + offset);
}

static void board_write(void *ctx, uint32_t base, uint32_t offset,
                        uint32_t value) {
    (void)ctx;
    Xil_Out32((UINTPTR)base + offset, value);
}

static void board_flush(void *ctx, uintptr_t address, uint32_t bytes) {
    (void)ctx;
    Xil_DCacheFlushRange((UINTPTR)address, bytes);
}

static void board_invalidate(void *ctx, uintptr_t address, uint32_t bytes) {
    (void)ctx;
    Xil_DCacheInvalidateRange((UINTPTR)address, bytes);
}

static int board_send(void *ctx, uint16_t local_port,
                      uint16_t remote_port, const uint8_t *packet,
                      uint16_t length) {
    struct udp_pcb *pcb = local_port == FPTZ_CONTROL_PORT
                        ? control_pcb : output_pcb;
    struct pbuf *buffer;
    err_t result;
    (void)ctx;
    if (!pcb || (local_port != FPTZ_CONTROL_PORT &&
                 local_port != FPTZ_OUTPUT_PORT) ||
        length > FPTZ_HEADER_SIZE + FPTZ_MAX_PAYLOAD) return -1;
    buffer = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
    if (!buffer) return -1;
    result = pbuf_take(buffer, packet, length);
    if (result == ERR_OK)
        result = udp_sendto(pcb, buffer, &host_ip, remote_port);
    pbuf_free(buffer);
    return result == ERR_OK ? 0 : -1;
}

static int board_start_frame(void *ctx, FptzFrameSlot *slot,
                             const uint8_t *lut) {
    (void)ctx;
    return fptz_media_start(&media_hw, slot, lut);
}

static int board_poll_frame(void *ctx, FptzFrameSlot *slot) {
    (void)ctx;
    return fptz_media_poll(&media_hw, slot);
}

static void board_set_flow_active(void *ctx,int active) {
    (void)ctx; fptz_media_set_flow_active(&media_hw,active);
}
static int board_start_capacity_stage(void *ctx, uint8_t stage,
                                      FptzFrameSlot *slot,
                                      const uint8_t *lut) {
    (void)ctx;
    return fptz_media_capacity_start(&media_hw, stage, slot, lut);
}

static int board_poll_capacity_stage(void *ctx, uint8_t stage,
                                     FptzFrameSlot *slot) {
    (void)ctx;
    return fptz_media_capacity_poll(&media_hw, stage, slot);
}

static int board_lut_published(void *ctx, const uint8_t *lut,
                               uint32_t bytes) {
    (void)ctx;
    fptz_media_lut_published(&media_hw, lut, bytes);
    return media_hw.fault ? -1 : 0;
}

static void board_pipeline_stats(void *ctx, FptzPipelineStats *stats) {
    (void)ctx;
    fptz_media_get_stats(&media_hw, stats);
}

static void board_receive(void *ctx, struct udp_pcb *pcb,
                          struct pbuf *buffer, const ip_addr_t *remote,
                          u16_t remote_port) {
    uint16_t local_port = (uint16_t)(uintptr_t)ctx;
    (void)pcb;
    if (!buffer) return;
    if (IP_IS_V4(remote) &&
        ip4_addr_cmp(ip_2_ip4(remote), ip_2_ip4(&host_ip)) &&
        remote_port == local_port &&
        buffer->tot_len <= sizeof(rx_packet) &&
        pbuf_copy_partial(buffer, rx_packet, buffer->tot_len, 0) ==
            buffer->tot_len) {
        (void)fptz_server_on_packet(&server, local_port, rx_packet,
                                    buffer->tot_len);
    }
    pbuf_free(buffer);
}

static struct udp_pcb *bind_udp(uint16_t port) {
    struct udp_pcb *pcb = udp_new();
    if (!pcb) return NULL;
    if (udp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        udp_remove(pcb);
        return NULL;
    }
    if (port != FPTZ_OUTPUT_PORT)
        udp_recv(pcb, board_receive, (void *)(uintptr_t)port);
    return pcb;
}

int fptz_board_init(void) {
    FptzMediaOps media_ops = {
        board_read, board_write, board_flush, board_invalidate,
        board_now_us, NULL
    };
    FptzServerOps server_ops = {
        board_send, board_start_frame, board_poll_frame,
        board_lut_published, board_now_us, NULL, board_pipeline_stats,
        board_start_capacity_stage, board_poll_capacity_stage, board_set_flow_active
    };
    IP4_ADDR(&host_ip, 192, 168, 10, 100);
    for (uint8_t i = 0; i < FPTZ_FRAME_SLOTS; ++i) {
        slots[i].input = frame_memory[i].input;
        slots[i].rgb = frame_memory[i].rgb;
        slots[i].rgb_rear = frame_memory[i].rgb_rear;
        slots[i].corrected = frame_memory[i].corrected;
        slots[i].output = frame_memory[i].output;
        slots[i].input_bitmap = frame_memory[i].input_bitmap;
        slots[i].output_bitmap = frame_memory[i].output_bitmap;
    }
    if (fptz_media_init(&media_hw, &media_ops)) {
        xil_printf("FPTZ_PL_MISMATCH src=%08x out=%08x rows=%u abi=%u lenses=%u lut=%u cache=%u\r\n",
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_SOURCE_SIZE),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_OUTPUT_SIZE),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_CACHE_ROWS),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_ABI_VERSION),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_LENS_COUNT),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_LUT_FORMAT),
                   board_read(NULL, FPTZ_FISH_BASE, FPTZ_IP_CACHE_MODE));
        return -1;
    }
    if (fptz_server_init(&server, slots, FPTZ_FRAME_SLOTS,
                         lut_memory_a, lut_memory_b,
                         fptz_scene_mask_bits, &server_ops) ||
        fptz_server_configure_return_test(&server, return_test_jpeg,
                                          sizeof(return_test_jpeg))) return -1;
    /* These fixed DMA destinations are never written by the CPU. The CPU
     * reads output JPEGs only after media_hw invalidates the completed range.
     * Keep the one-time full cleanup before UDP/frame admission begins. */
    for (uint8_t i = 0; i < FPTZ_FRAME_SLOTS; ++i)
        if (fptz_media_prepare_read_only_slot(&media_hw, &slots[i])) return -1;
    fptz_scene_pair_build(fptz_scene_mask_bits, scene_pair_memory);
    server.scene_pair_bits = scene_pair_memory;
    fptz_scene_pair_build(fptz_scene_mask_rear_bits, rear_scene_pair_memory);
    server.rear_scene_pair_bits = rear_scene_pair_memory;
    control_pcb = bind_udp(FPTZ_CONTROL_PORT);
    input_pcb = bind_udp(FPTZ_INPUT_PORT);
    output_pcb = bind_udp(FPTZ_OUTPUT_PORT);
    if (!control_pcb || !input_pcb || !output_pcb) return -1;
    Xil_Out32(FPTZ_GPIO_BASE, 0);
    xil_printf("FPTZ_UDP_READY 192.168.10.10:5500,5501,5502\r\n");
    xil_printf("FPTZ_VIDEO_TX burst=%u gap_us=%u\r\n",
               FPTZ_VIDEO_TX_BURST, FPTZ_VIDEO_TX_GAP_US);
    xil_printf("FPTZ_PIPELINE axis=1 wait_rows=0 sources=2x%ux%u output=%ux%u crop=0,0 rows=%u abi=%u mm2s=off\r\n",
               FPTZ_SOURCE_WIDTH, FPTZ_SOURCE_HEIGHT,
               FPTZ_OUTPUT_WIDTH, FPTZ_OUTPUT_HEIGHT, FPTZ_CACHE_ROWS, FPTZ_PL_ABI_VERSION);
    xil_printf("FPTZ_PL_CAPS source=%08x output=%08x rows=%u abi=%u lenses=%u lut=%u cache_mode=%u\r\n",
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_SOURCE_SIZE),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_OUTPUT_SIZE),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_CACHE_ROWS),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_ABI_VERSION),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_LENS_COUNT),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_LUT_FORMAT),
               board_read(NULL,FPTZ_FISH_BASE,FPTZ_IP_CACHE_MODE));
    xil_printf("FPTZ_CALIB lenses=front,rear model=Mei_Brown5 sha256=%s\r\n",
               FPTZ_CALIBRATION_SHA256);
    xil_printf("FPTZ_PS3 paired_media=enabled protocol=v3 input_type=0x23 pan=360 tilt=+-90 hfov=90 grid=0.1\r\n");
    xil_printf("FPTZ_PS_FIX start_ack=1 descriptor_clear=1\r\n");
    xil_printf("FPTZ_PERF crc_table=1 dma_read_only=1 resume_stops_return=1\r\n");
    xil_printf("FPTZ_JPEG_SCAN word_bytes=8 strict_markers=1\r\n");
    xil_printf("FPTZ_CRC_FAST slicing=8 encoder_reset_fast_poll=1\r\n");
    xil_printf("FPTZ_FLOW_DIAG version=2 heartbeat=280 normal_only=1\r\n");
    xil_printf("FPTZ_TX_FIX finish_started=1 latest_pending=1 gap_us=1000\r\n");
    xil_printf("FPTZ_LAT_FIX collect_before_tx=1 local_clock=1 pause_report=1\r\n");
    return 0;
}

static void print_latency_stats(void) {
    const FptzLatencyStats *stats = &server.latency;
    uint32_t avg[FPTZ_LAT_PARTS] = {0};
    for (unsigned i = 0; i < FPTZ_LAT_PARTS; ++i)
        if (stats->samples) avg[i] = (uint32_t)(stats->sum_us[i] / stats->samples);
    xil_printf("FPTZ_LATENCY samples=%u frame=%u avg_us rx=%u ready=%u pipe=%u outq=%u tx=%u total=%u\r\n",
               stats->samples, stats->last_frame, avg[0], avg[1], avg[2],
               avg[3], avg[4], avg[5]);
    xil_printf("FPTZ_LATENCY_MAX us rx=%u ready=%u pipe=%u outq=%u tx=%u total=%u\r\n",
               stats->max_us[0], stats->max_us[1], stats->max_us[2],
               stats->max_us[3], stats->max_us[4], stats->max_us[5]);
    xil_printf("FPTZ_PTZ epoch=%u pan_mdeg=%d tilt_mdeg=%d hfov_mdeg=%u rows=%u/%u\r\n",
               server.active_epoch,server.active_pan_mdeg,server.active_tilt_mdeg,
               FPTZ_PTZ_HFOV_MDEG,server.active_lens_span[0],server.active_lens_span[1]);
    xil_printf("FPTZ_PTZ_TIMING_US generate=%u validate=%u ready_to_active=%u total_switch=%u\r\n",
               server.lut_last_generate_us,server.lut_last_validate_us,
               server.lut_last_wait_us,server.lut_last_switch_us);
}

/* Fault-only snapshot: never print in the steady-state media path. */
static void print_decoder_fault(unsigned lens) {
    const uint32_t base=lens ? FPTZ_DEC_REAR_BASE:FPTZ_DEC_BASE;
    const int index=media_hw.dec_active[lens];
    const FptzHwFrame *f=index>=0 && index<FPTZ_FRAME_SLOTS ? &media_hw.frames[index]:NULL;
    const FptzDecoderRun *d=f ? &f->decoder[lens]:NULL;
    uint64_t elapsed=d && d->started ? board_now_us(NULL)-d->started_us:0;
    xil_printf("FPTZ_DEC_DIAG lens=%u active=%d status=%08x meta=%08x rows=%u pixels=%u input=%08x output=%08x expected_gen=%u previous_gen=%u accepted=%u elapsed_ms=%u\r\n",
               lens,index,board_read(NULL,base,4),board_read(NULL,base,0x18),
               board_read(NULL,base,0x14),board_read(NULL,base,0x10),
               board_read(NULL,base,8),board_read(NULL,base,12),
               d ? d->generation:0,d ? d->previous_generation:0,d ? d->accepted:0,
               (uint32_t)(elapsed/1000u));
    if (f && f->slot) {
        const FptzFrameSlot *slot=f->slot;
        xil_printf("FPTZ_DEC_FRAME lens=%u frame=%u epoch=%u stage=%u jpeg_len=%u jpeg_addr=%08x rgb_addr=%08x sources_done=%u corr=%u/%u enc=%u/%u\r\n",
                   lens,slot->frame_id,slot->ptz_epoch,f->stage,slot->jpeg_pair.length[lens],
                   (uint32_t)((uintptr_t)slot->input+slot->jpeg_pair.offset[lens]),
                   (uint32_t)(uintptr_t)(lens ? slot->rgb_rear:slot->rgb),
                   slot->sources_decoded,f->corr_started,f->corr_done,f->enc_started,f->enc_done);
    }
}

void fptz_board_pump(void) {
    fptz_server_pump(&server);
    if (server.stop_diagnostics_pending) {
        server.stop_diagnostics_pending = 0;
        /* STOP/PAUSE only: never print during steady-state media or heartbeat. */
        print_latency_stats();
        xil_printf("FPTZ_DECODE_LENSES completed=%u/%u last_us=%u/%u failed=%u/%u timeout=%u/%u\r\n",
                   media_hw.stats.decode_lens_completed[0],media_hw.stats.decode_lens_completed[1],
                   media_hw.stats.decode_lens_last_us[0],media_hw.stats.decode_lens_last_us[1],
                   media_hw.stats.decode_lens_failed[0],media_hw.stats.decode_lens_failed[1],
                   media_hw.stats.decode_lens_timeout[0],media_hw.stats.decode_lens_timeout[1]);
        xil_printf("FPTZ_DROP ready=%u output=%u tx_fail=%u incomplete=%u crc=%u stale=%u no_slot=%u\r\n",
                   server.ready_superseded_drop, server.output_superseded_drop,
                   server.output_send_drop, server.input_incomplete_drop,
                   server.input_crc_drop, server.stale_epoch_drop,
                   server.no_slot_drop);
    }
    if (!fault_reported && (server.fault || media_hw.fault)) {
        fault_reported = 1;
        /* Notify before slow UART output so the Host stops feeding a faulted board. */
        uint8_t stage = 0, reason = 0;
        int active = -1;
        if (media_hw.stats.decode_failed || media_hw.stats.decode_timeout) {
            unsigned lens = (media_hw.stats.decode_lens_failed[1] ||
                             media_hw.stats.decode_lens_timeout[1]) ? 1u : 0u;
            stage = 1; reason = (uint8_t)((lens << 1) |
                       (media_hw.stats.decode_timeout ? 1u : 0u));
            active = media_hw.dec_active[lens];
        } else if (media_hw.stats.correct_failed || media_hw.stats.correct_timeout) {
            stage = 2; reason = media_hw.stats.correct_timeout ? 1u : 0u;
            active = media_hw.corr_active;
        } else if (media_hw.stats.encode_failed || media_hw.stats.encode_timeout) {
            stage = 3; reason = media_hw.stats.encode_timeout ? 1u : 0u;
            active = media_hw.enc_active;
        }
        uint32_t frame = active >= 0 && active < FPTZ_FRAME_SLOTS &&
                         media_hw.frames[active].slot ? media_hw.frames[active].slot->frame_id : 0;
        int notified = stage ? fptz_server_report_hardware_fault(&server,stage,reason,frame) : -1;
        xil_printf("FPTZ_FAULT_NOTIFY stage=%u reason=%u frame=%u sent=%u rc=%d\r\n",
                   stage,reason,frame,notified >= 0 ? 1u : 0u,notified);
        xil_printf("FPTZ hardware fault; stop frame admission and reboot\r\n");
        xil_printf("FPTZ_PIPELINE_FAULT corr_last=%08x dec_failed=%u corr_failed=%u enc_failed=%u timeout=%u/%u/%u\r\n",
                   media_hw.corrector.last_status,
                   media_hw.stats.decode_failed, media_hw.stats.correct_failed,
                   media_hw.stats.encode_failed, media_hw.stats.decode_timeout,
                   media_hw.stats.correct_timeout, media_hw.stats.encode_timeout);
        xil_printf("FPTZ_DECODE_COUNTS start=%u/%u complete=%u/%u timeout=%u/%u\r\n",
                   media_hw.stats.decode_lens_started[0],media_hw.stats.decode_lens_started[1],
                   media_hw.stats.decode_lens_completed[0],media_hw.stats.decode_lens_completed[1],
                   media_hw.stats.decode_lens_timeout[0],media_hw.stats.decode_lens_timeout[1]);
        print_decoder_fault(0);
        print_decoder_fault(1);
        xil_printf("FPTZ_ENCODER_DIAG after_stop=%u gpio=%08x mm2s_control=%08x mm2s_status=%08x s2mm_control=%08x s2mm_status=%08x s2mm_length=%u\r\n",
                   media_hw.fault_stopped,board_read(NULL,FPTZ_GPIO_BASE,0),
                   board_read(NULL,FPTZ_DMA_BASE,0),board_read(NULL,FPTZ_DMA_BASE,4),
                   board_read(NULL,FPTZ_DMA_BASE,0x30),board_read(NULL,FPTZ_DMA_BASE,0x34),
                   board_read(NULL,FPTZ_DMA_BASE,0x58));
        xil_printf("FPTZ_CORR_DIAG busy_seen=%u polls=%u max_gap_us=%u cycles=%u\r\n",
                   media_hw.corrector.busy_seen, media_hw.corrector.poll_count,
                   media_hw.corrector.max_poll_gap_us,
                   board_read(NULL, FPTZ_FISH_BASE, 0x18u));
    }
}

int fptz_board_rx_needs_pump(void) {
    return fptz_server_rx_needs_pump(&server);
}
