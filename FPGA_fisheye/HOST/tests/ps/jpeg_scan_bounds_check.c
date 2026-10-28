#define _GNU_SOURCE
/* Includes production scanner to test exact bounded loads, without test-only APIs. */
#include "fptz_protocol.c"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int fptz_jpeg_is_source_baseline_reference(const uint8_t *,size_t);
int main(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *map=mmap(NULL,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(map==MAP_FAILED || mprotect(map+page,page,PROT_READ|PROT_WRITE))return 1;
    uint8_t *end=map+2*page;
    memset(map+page,0x35,page);
    for(size_t n=0;n<=page;++n) {
        uint8_t *start=end-n;
        if(jpeg_find_ff(start,n))return 2;
        if(n) {
            end[-1]=0xff;
            if(jpeg_find_ff(start,n)!=end-1)return 3;
            end[-1]=0x35;
            *start=0xff;
            if(jpeg_find_ff(start,n)!=start)return 4;
            *start=0x35;
        }
    }
    /* Valid structural header; no actual JPEG decoding is claimed here. */
    static const uint8_t header[]={
        0xff,0xd8,0xff,0xdb,0,3,0,0xff,0xc4,0,3,0,
        0xff,0xc0,0,17,8,5,48,5,48,3,1,0x22,0,2,0x11,0,3,0x11,0,
        0xff,0xda,0,12,3,1,0,2,0,3,0,0,63,0
    };
    for(size_t n=sizeof(header)+2;n<=page;++n) {
        uint8_t *start=end-n;memset(start,0x35,n);memcpy(start,header,sizeof(header));
        end[-2]=0xff;end[-1]=0xd9;
        if(!fptz_jpeg_is_source_baseline(start,n) ||
           !fptz_jpeg_is_source_baseline_reference(start,n))return 5;
        if(n>=sizeof(header)+4) {
            end[-4]=0xff;end[-3]=0xd0;
            if(fptz_jpeg_is_source_baseline(start,n) ||
               fptz_jpeg_is_source_baseline_reference(start,n))return 6;
        }
    }
    munmap(map,3*page);
    puts("{\"guard_page_all_tail_lengths\":true,\"boundary_jpeg_and_restart_rejection\":true}");
    return 0;
}
