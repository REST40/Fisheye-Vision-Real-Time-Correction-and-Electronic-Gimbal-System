#include "fptz_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t clock_us=100;static int capability,accepted,ready,active,published,wait_verified,legacy_heartbeat,legacy_result,stage_result,complete_frame,hardware_fault,flow_verified;
static uint64_t now(void *ctx){(void)ctx;return clock_us+=10;}
static int send_packet(void *ctx,uint16_t local,uint16_t remote,const uint8_t *p,uint16_t n){
 FptzHeader h;(void)ctx;(void)local;(void)remote;
 if(fptz_parse_packet(p,n,&h))return -1;
 if(h.msg_type==FPTZ_HEARTBEAT && h.payload_len==96)legacy_heartbeat=1;
 if(h.msg_type==FPTZ_HEARTBEAT && h.payload_len==280 &&
    fptz_get32(p+48)==0x464c4f57u && fptz_get32(p+52)==2 &&
    fptz_get32(p+56)==1 && fptz_get32(p+48+80)==3 &&
    fptz_get32(p+48+32)==1 && fptz_get32(p+48+36)==25 &&
    fptz_get32(p+48+208)==7 && fptz_get32(p+48+216)==1 && fptz_get32(p+48+220)==123 && fptz_get32(p+48+256)==4 &&
    fptz_get32(p+48+264)==1 && fptz_get32(p+48+268)==9 && fptz_get32(p+48+276)==20) flow_verified=1;
 if(h.msg_type==FPTZ_FRAME_RESULT && h.payload_len==36)legacy_result=1;
 if(h.msg_type==FPTZ_FRAME_RESULT && h.payload_len==48 &&
    fptz_get32(p+48+36)==100 && fptz_get32(p+48+40)==200 && fptz_get32(p+48+44)==300)stage_result=1;
 if(h.msg_type==FPTZ_HEARTBEAT && h.payload_len==152 &&
    fptz_get32(p+48+96)==0x57414954u && fptz_get32(p+48+100)==2 &&
    fptz_get32(p+48+112)==1 && fptz_get32(p+48+116)==25)wait_verified=1;
 if(h.msg_type==FPTZ_ERROR && h.payload_len==8 && h.txn_id==0 &&
    fptz_get16(p+48)==FPTZ_ERR_HARDWARE_FAULT && p[50]==1 && p[51]==3 &&
    fptz_get32(p+52)==544)hardware_fault=1;
 if(h.msg_type==FPTZ_HELLO_ACK)capability=fptz_get16(p+48+18)==1;
 if(h.msg_type==FPTZ_PTZ_STATUS){accepted|=p[48]==1;ready|=p[48]==2;active|=p[48]==3;}
 return n;
}
static void get_stats(void *ctx,FptzPipelineStats *stats) {
 (void)ctx;memset(stats,0,sizeof(*stats));
 stats->flow[2].completed=7;stats->flow[2].busy_us=((uint64_t)1<<32)+123;
}
static int start(void *ctx,FptzFrameSlot *slot,const uint8_t *lut){(void)ctx;(void)slot;(void)lut;return 1;}
static int poll(void *ctx,FptzFrameSlot *slot){(void)ctx;(void)slot;return complete_frame;}
static int publish(void *ctx,const uint8_t *lut,uint32_t n){(void)ctx;(void)lut;published=n==FPTZ_LUT_BYTES;return 0;}
static int command(FptzServer *s,uint8_t type,uint32_t txn,const uint8_t *body,uint16_t n){
 uint8_t packet[80]={0};FptzHeader h={0};h.msg_type=type;h.payload_len=n;h.session_id=type==1?0:s->session_id;
 h.txn_id=txn;h.chunk_count=1;h.object_len=n;h.object_crc32=fptz_crc32(body,n);
 fptz_write_header(packet,&h);memcpy(packet+48,body,n);return fptz_server_on_packet(s,5500,packet,48+n);
}
int main(void){
 FptzServer s;FptzFrameSlot slot={0};uint8_t dummy[4]={0};uint8_t *a=aligned_alloc(64,FPTZ_LUT_BYTES),*b=aligned_alloc(64,FPTZ_LUT_BYTES);
 slot.input=slot.output=slot.input_bitmap=slot.output_bitmap=dummy;
 FptzServerOps ops={0};ops.send=send_packet;ops.start_frame=start;ops.poll_frame=poll;ops.lut_published=publish;ops.now_us=now;ops.get_pipeline_stats=get_stats;
 if(fptz_server_init(&s,&slot,1,a,b,fptz_scene_mask_front_bits,&ops))return 1;
 uint8_t hello[8]={0};fptz_put16(hello,1400);fptz_put16(hello+2,64);fptz_put16(hello+4,4);
 if(command(&s,FPTZ_HELLO,1,hello,8)||!capability)return 2;
 uint8_t ptz[12]={0};fptz_put32(ptz+8,360000);
 if(command(&s,FPTZ_SET_PTZ,2,ptz,12)<0)return 3;
 for(int i=0;i<10000&&!active;++i)fptz_server_pump(&s);
 if(!accepted||!ready||!active||!published||!s.active_lut_valid||s.fault)return 4;
 uint8_t stream[4]={1,1,0,0};if(command(&s,FPTZ_STREAM_CTRL,3,stream,4)<0||!s.streaming)return 5;
 s.latency.samples=2;s.latency.sum_us[FPTZ_LAT_READY]=((uint64_t)1<<32)+25;
 s.flow_input_crc_samples=4;s.flow_input_crc_us=((uint64_t)1<<32)+9;s.flow_input_parse_us=20;
 s.ready_superseded_drop=3;s.flow_crc_us=((uint64_t)1<<32)+25;
 clock_us+=1000000;fptz_server_pump(&s);if(!wait_verified||!legacy_heartbeat||!flow_verified)return 6;
 slot.state=FPTZ_SLOT_RUNNING;slot.ptz_epoch=s.active_epoch;slot.frame_id=7;slot.output_len=4;
 dummy[0]=255;dummy[1]=216;dummy[2]=255;dummy[3]=217;
 slot.latency_valid=1;slot.flow_generation=s.flow_generation;
 slot.first_rx_us=slot.input_ready_us=slot.admitted_us=clock_us;
 slot.decode_wait_us=100;slot.correct_wait_us=200;slot.encode_wait_us=300;
 complete_frame=1;fptz_server_pump(&s);if(!legacy_result||!stage_result)return 7;
 if(s.flow_crc_samples!=1 || s.flow_sent_samples!=1 || !s.flow_credit_events)return 9;
 /* Exercise real input completion: successful CRC with bad format, then bad CRC.
  * Both release the slot; format parsing must not run after CRC rejection. */
 uint8_t input[64]={0},bitmap[FPTZ_INPUT_BITMAP_BYTES]={0},packet[112]={0};
 slot.input=input;slot.input_bitmap=bitmap;slot.pair_valid=0;
 FptzHeader ih={0};ih.msg_type=FPTZ_INPUT_JPEG_PAIR;ih.payload_len=64;
 ih.session_id=s.session_id;ih.ptz_epoch=s.active_epoch;ih.chunk_count=1;ih.object_len=64;
 ih.frame_id=100;ih.object_crc32=fptz_crc32(input,64);
 fptz_write_header(packet,&ih);memcpy(packet+48,input,64);
 if(fptz_server_on_packet(&s,5501,packet,sizeof(packet)) || slot.state!=FPTZ_SLOT_FREE ||
    s.flow_input_crc_samples!=5 || s.flow_input_crc_us!=((uint64_t)1<<32)+19 ||
    s.flow_input_parse_us!=30 || s.input_crc_drop!=1 || s.fault)return 10;
 ih.frame_id=101;ih.object_crc32^=1;fptz_write_header(packet,&ih);
 if(fptz_server_on_packet(&s,5501,packet,sizeof(packet)) || slot.state!=FPTZ_SLOT_FREE ||
    s.flow_input_crc_samples!=6 || s.flow_input_crc_us!=((uint64_t)1<<32)+29 ||
    s.flow_input_parse_us!=30 || s.input_crc_drop!=2 || s.fault)return 11;
 if(fptz_server_report_hardware_fault(&s,1,3,544)<0||!hardware_fault||!s.fault)return 8;
 printf("{\"panorama_capability\":true,\"accepted_ready_active\":true,\"lut_published\":true,\"stream_started\":true,\"wait_telemetry_verified\":true,\"flow_wire_and_output_accounting_verified\":true,\"input_crc_timing_and_rejection_verified\":true,\"hardware_fault_notification_verified\":true,\"epoch\":%u}\n",s.active_epoch);
 free(a);free(b);return 0;
}
