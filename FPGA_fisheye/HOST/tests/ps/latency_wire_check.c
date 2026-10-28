/* Tests production serialization and egress; deterministic clocks are not PL measurements. */
#include "fptz_server.c"
#include <assert.h>
#include <stdio.h>
static uint64_t fake_now=10000;
static unsigned events[5],legacy36,legacy48;
static int fail_telemetry,fail_video;
static uint8_t report[160];
static uint64_t clock_us(void* c){(void)c;return fake_now++;}
static int transmit(void* ctx,uint16_t local,uint16_t remote,const uint8_t* packet,uint16_t n){
 (void)ctx;(void)local;(void)remote;FptzHeader h;assert(!fptz_parse_packet(packet,n,&h));
 if(h.msg_type==FPTZ_OUTPUT_JPEG)return fail_video?-1:0;
 if(h.msg_type==FPTZ_FRAME_RESULT){if(h.payload_len==36)++legacy36;else if(h.payload_len==48)++legacy48;else {
  assert(h.payload_len==160&&h.frame_id==7&&h.ptz_epoch==2);memcpy(report,packet+48,160);
  assert(fptz_get32(report)==0x4c415431&&fptz_get32(report+4)==1);
  unsigned e=fptz_get32(report+8);assert(e>=1&&e<=4);++events[e];
  assert(fptz_get32(report+12)==FPTZ_CORRECTOR_CLOCK_HZ);
  if(fail_telemetry)return -1;
 }}return 0;
}
int main(void){
 FptzServer s={0};FptzFrameSlot slots[2]={0};uint8_t jpeg[]={255,216,255,217};
 s.slots=slots;s.slot_count=2;s.session_id=1;s.active_epoch=2;s.max_payload=1400;s.ops.send=transmit;s.ops.now_us=clock_us;
 FptzFrameSlot* f=&slots[0];f->frame_id=7;f->ptz_epoch=2;f->output=jpeg;f->output_len=4;f->input_len=100;
 f->first_rx_us=100;f->input_ready_us=200;f->admitted_us=300;f->output_ready_us=700;f->latency_valid=1;
 const uint64_t stages[]={310,400,320,430,450,650,440,690};memcpy(f->stage_times_us,stages,sizeof(stages));f->frame_cycles=1234;
 send_frame_result(&s,f);assert(legacy36==1&&legacy48==1);
 send_latency_result(&s,f,1);assert(events[1]==1&&fptz_get32(report+72)==400&&fptz_get32(report+88)==210&&fptz_get32(report+112)==340);
 f->state=FPTZ_SLOT_OUTPUT;f->tx_count=1;uint8_t batch=0;pump_output(&s,f,&batch);
 assert(events[2]==1&&s.latency.samples==1&&s.output_complete_count==1);
 uint64_t sum=0;for(unsigned i=0;i<5;++i)sum+=fptz_get32(report+64+4*i);assert(sum==fptz_get32(report+84));
 f->state=FPTZ_SLOT_OUTPUT;f->tx_next=0;slots[1].state=FPTZ_SLOT_OUTPUT;slots[1].frame_id=8;batch=0;pump_output(&s,f,&batch);
 assert(events[3]==1&&fptz_get32(report+84)==0&&s.output_superseded_drop==1);
 slots[1].state=FPTZ_SLOT_FREE;f->state=FPTZ_SLOT_OUTPUT;f->tx_next=0;s.next_video_tx_us=0;batch=0;fail_video=1;pump_output(&s,f,&batch);
 assert(events[4]==1&&s.output_send_drop==1&&fptz_get32(report+84)==0);
 fail_telemetry=1;send_latency_result(&s,f,1);assert(s.telemetry_failures==1&&s.telemetry_attempts==5&&!s.fault);
 assert(s.latency.samples==1);puts("{\"processed_sent_replaced_failed\":true,\"legacy_compatible\":true,\"local_intervals_sum\":true,\"telemetry_failure_isolated\":true}");return 0;
}
