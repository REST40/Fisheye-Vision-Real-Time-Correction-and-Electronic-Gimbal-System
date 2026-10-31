// Deterministically inject a TX interrupt at the heap allocation commit point.
// Compile with the BSP lwIP mem.c through verify_lwip_heap_irq.py.
#include "lwip/mem.h"
#include "lwip/sys.h"
#include <stdio.h>
#include <stdint.h>
static unsigned masked;
static void *to_free, *pending;
sys_prot_t sys_arch_protect(void) {unsigned old=masked;masked=1;return old;}
void sys_arch_unprotect(sys_prot_t old) {
    masked=old;
    if(!masked && pending){void *p=pending;pending=NULL;masked=1;mem_free(p);masked=0;}
}
void irq_test_hook(void) {
    void *p=to_free;
    to_free=NULL;
    if(p){if(masked)pending=p;else{masked=1;mem_free(p);masked=0;}}
}
int main(void) {
    mem_init();
    void *a=mem_malloc(128), *b=mem_malloc(128), *c=mem_malloc(128);
    if(!a||!b||!c)return 2;
    mem_free(b);
    to_free=a;
    void *first=mem_malloc(64), *second=mem_malloc(256);
    if(!first||!second||to_free||pending)return 2;
    uintptr_t x=(uintptr_t)first,y=(uintptr_t)second;
    int overlap=x<y+256 && y<x+64;
    printf("{\"irq_heap_protected\":%d,\"overlapping_allocations\":%s}\n",LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT,overlap?"true":"false");
    return overlap==!LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT?0:1;
}
