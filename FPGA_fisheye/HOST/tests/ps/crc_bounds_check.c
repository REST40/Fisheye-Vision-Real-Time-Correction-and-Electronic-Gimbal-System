#define _GNU_SOURCE
#include "fptz_protocol.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
uint32_t fptz_crc32_baseline(const void *,size_t);
int main(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *map=mmap(NULL,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(map==MAP_FAILED || mprotect(map+page,page,PROT_READ|PROT_WRITE))return 1;
    for(size_t i=0;i<page;++i)map[page+i]=(uint8_t)(i*29+i/251);
    for(size_t n=0;n<=page;++n) {
        const uint8_t *p=map+2*page-n;
        if(fptz_crc32(p,n)!=fptz_crc32_baseline(p,n))return 2;
    }
    FptzRx rx={0};rx.storage=map+page;rx.object.object_len=(uint32_t)page;
    rx.received_count=rx.object.chunk_count=1;
    rx.object.object_crc32=fptz_crc32_baseline(rx.storage,page);
    if(fptz_rx_verify(&rx))return 3;
    rx.storage[page/2]^=1;
    if(fptz_rx_verify(&rx)!=-1)return 4;
    rx.storage[page/2]^=1;rx.received_count=0;
    if(fptz_rx_verify(&rx)!=-1)return 5;
    munmap(map,3*page);
    puts("{\"guard_page_all_tails_verified\":true,\"corruption_and_incomplete_rejected\":true}");
    return 0;
}
