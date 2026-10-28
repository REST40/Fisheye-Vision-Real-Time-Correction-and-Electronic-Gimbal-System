#include "xparameters.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xemacps.h"

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"
#include "fptz_board.h"
#include "fptz_scheduler.h"
#include "xtime_l.h"

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
extern enum ethernet_link_status eth_link_status;
/* Xilinx standalone timer hooks are exported by lwIP, but not in tcp.h. */
void tcp_fasttmr(void);
void tcp_slowtmr(void);

static struct netif server_netif;
struct netif *echo_netif = &server_netif; /* Xilinx platform timer uses this name. */

static int receive_one(void *context) {
    int received = xemacif_input((struct netif *)context);
    if (received > 0 && fptz_board_rx_needs_pump()) return FPTZ_RX_YIELD;
    return received;
}

static uint64_t scheduler_now_us(void *context) {
    XTime ticks;
    (void)context;
    XTime_GetTime(&ticks);
    return ((uint64_t)ticks / COUNTS_PER_SECOND) * 1000000ull +
           ((uint64_t)ticks % COUNTS_PER_SECOND) * 1000000ull /
               COUNTS_PER_SECOND;
}

static int check_board_phy(void) {
    struct xemac_s *mac = (struct xemac_s *)echo_netif->state;
    XEmacPs mdio = {0};
    u16 id1 = 0, id2 = 0, status = 0;
    if (!mac || mac->type != xemac_type_emacps || !mac->state) return -1;
    mdio.Config.BaseAddress = PLATFORM_EMAC_BASEADDR;
    if (XEmacPs_PhyRead(&mdio, 1, 2, &id1) != XST_SUCCESS ||
        XEmacPs_PhyRead(&mdio, 1, 3, &id2) != XST_SUCCESS ||
        XEmacPs_PhyRead(&mdio, 1, 1, &status) != XST_SUCCESS ||
        XEmacPs_PhyRead(&mdio, 1, 1, &status) != XST_SUCCESS)
        return -1;
    xil_printf("GEM3 PHY1 ID=%04x:%04x BMSR=%04x\r\n", id1, id2, status);
    if (id1 != 0x001c || (id2 & 0xfff0) != 0xc910 ||
        !(status & 0x0004) || eth_link_status != ETH_LINK_UP) return -1;
    return 0;
}

int main(void) {
    ip_addr_t board_ip, netmask, gateway;
    unsigned char mac_address[6] = {0x00, 0x0a, 0x35, 0x00, 0x01, 0x02};
    /* Keep the existing board-specific GEM3 MDIO pin sanity check. */
    if ((Xil_In32(0xff180130u) & 0xfeu) != 0xc0u ||
        (Xil_In32(0xff180134u) & 0xfeu) != 0xc0u ||
        (Xil_In32(0xff18020cu) & 0x3000u) != 0u) {
        xil_printf("GEM3 MDIO MIO mismatch: use matching FSBL/XSA\r\n");
        return -1;
    }
    init_platform();
    IP4_ADDR(&board_ip, 192, 168, 10, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 192, 168, 10, 1);
    lwip_init();
    if (!xemac_add(echo_netif, &board_ip, &netmask, &gateway,
                   mac_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("GEM3 lwIP netif initialization failed\r\n");
        return -1;
    }
    if (check_board_phy()) {
        xil_printf("GEM3 PHY/link check failed\r\n");
        return -1;
    }
    netif_set_default(echo_netif);
    platform_enable_interrupts();
    netif_set_up(echo_netif);
    if (fptz_board_init()) {
        xil_printf("FPTZ UDP service initialization failed\r\n");
        return -1;
    }
    xil_printf("FPTZ_SCHED_V2 rx_batch=%u rx_budget_us=%u lut_pixels=%u pbuf_pool=%u frame_yield=1\r\n",
               FPTZ_RX_BATCH_PACKETS, FPTZ_RX_BUDGET_US,
               FPTZ_LUT_SLICE_PIXELS, PBUF_POOL_SIZE);
    while (1) {
        if (TcpFastTmrFlag) {
            tcp_fasttmr();
            TcpFastTmrFlag = 0;
        }
        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;
        }
        (void)fptz_service_rx(receive_one, scheduler_now_us, echo_netif);
        fptz_board_pump();
    }
}
