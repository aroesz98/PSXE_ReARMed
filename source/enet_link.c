/*
    Raw Ethernet on the EVKB: the SDK ENET driver in polling mode, buffers in the
    non-cacheable region, the KSZ8081 PHY through the SDK's PHY component.

    The pins (RMII on GPIO_B1_04..11, MDC/MDIO on GPIO_EMC_40/41, PHY reset on
    GPIO_AD_B0_09, PHY interrupt on GPIO_AD_B0_10) are the EVKB's own; none of
    them is used by the LCD, the SD card or the pad bridge. The 50 MHz RMII
    reference clock comes from the ENET PLL, which clock_config.c switches off,
    so it is brought up here.
*/

#include <string.h>

#include "enet_link.h"

#include "fsl_clock.h"
#include "fsl_debug_console.h"
#include "fsl_enet.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_phyksz8081.h"

#include "FreeRTOS.h"
#include "task.h"

#define PHY_ADDR 0x02U

#define RX_BD 16
#define TX_BD 192 /* a video frame (230 KB) queues up without stalling the emulator */
#define BUF_SIZE SDK_SIZEALIGN(ENET_FRAME_MAX_FRAMELEN, ENET_BUFF_ALIGNMENT)

/* descriptors and buffers where the DMA and the core see the same bytes */
AT_NONCACHEABLE_SECTION_ALIGN(enet_rx_bd_struct_t g_enet_rx_bd[RX_BD], ENET_BUFF_ALIGNMENT);
AT_NONCACHEABLE_SECTION_ALIGN(enet_tx_bd_struct_t g_enet_tx_bd[TX_BD], ENET_BUFF_ALIGNMENT);
AT_NONCACHEABLE_SECTION_ALIGN(uint8_t g_enet_rx_buf[RX_BD][BUF_SIZE], ENET_BUFF_ALIGNMENT);
AT_NONCACHEABLE_SECTION_ALIGN(uint8_t g_enet_tx_buf[TX_BD][BUF_SIZE], ENET_BUFF_ALIGNMENT);

static enet_handle_t s_handle;
static phy_handle_t s_phy;
static phy_ksz8081_resource_t s_phy_res;
static bool s_up;
static bool s_inited;

static status_t mdio_write(uint8_t phyAddr, uint8_t regAddr, uint16_t data)
{
    return ENET_MDIOWrite(ENET, phyAddr, regAddr, data);
}

static status_t mdio_read(uint8_t phyAddr, uint8_t regAddr, uint16_t *pData)
{
    return ENET_MDIORead(ENET, phyAddr, regAddr, pData);
}

/* the PHY's registers on the console: what it is, what it negotiated, which clock mode it is in */
static void phy_dump(const char *when)
{
    static const uint8_t regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x1e, 0x1f};

    PRINTF("enet: PHY %s:", when);

    for (uint32_t i = 0; i < sizeof(regs); i++)
    {
        uint16_t v = 0xffff;

        if (mdio_read(PHY_ADDR, regs[i], &v) != kStatus_Success)
            PRINTF(" r%02x=????", regs[i]);
        else
            PRINTF(" r%02x=%04x", regs[i], v);
    }

    /* and the clock the PHY lives on: the ENET PLL and the direction of ENET_REF_CLK */
    PRINTF(" | pll_enet=%08x gpr1=%08x mux_b1_10=%08x pad_b1_10=%08x\r\n", (unsigned)CCM_ANALOG->PLL_ENET,
           (unsigned)IOMUXC_GPR->GPR1, (unsigned)IOMUXC->SW_MUX_CTL_PAD[kIOMUXC_SW_MUX_CTL_PAD_GPIO_B1_10],
           (unsigned)IOMUXC->SW_PAD_CTL_PAD[kIOMUXC_SW_PAD_CTL_PAD_GPIO_B1_10]);
}

static void pins_init(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_10_GPIO1_IO10, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_04_ENET_RX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_05_ENET_RX_DATA01, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_06_ENET_RX_EN, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_07_ENET_TX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_08_ENET_TX_DATA01, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_09_ENET_TX_EN, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_10_ENET_REF_CLK, 1U); /* SION: the clock is an output */
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_11_ENET_RX_ER, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_40_ENET_MDC, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_41_ENET_MDIO, 0U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0xB0A9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_10_GPIO1_IO10, 0xB0A9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_04_ENET_RX_DATA00, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_05_ENET_RX_DATA01, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_06_ENET_RX_EN, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_07_ENET_TX_DATA00, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_08_ENET_TX_DATA01, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_09_ENET_TX_EN, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_10_ENET_REF_CLK, 0x31U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_11_ENET_RX_ER, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_40_ENET_MDC, 0xB0E9U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_41_ENET_MDIO, 0xB829U);
}

static void phy_reset(void)
{
    gpio_pin_config_t out = {kGPIO_DigitalOutput, 0, kGPIO_NoIntmode};

    /* the interrupt pin is a strap of the KSZ8081: high while it comes out of reset */
    GPIO_PinInit(GPIO1, 10, &out);
    GPIO_PinWrite(GPIO1, 10, 1);

    GPIO_PinInit(GPIO1, 9, &out);
    GPIO_PinWrite(GPIO1, 9, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    GPIO_PinWrite(GPIO1, 9, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
}

int enet_link_init(const uint8_t mac[6])
{
    /* the 50 MHz RMII reference clock, out of the chip to the PHY */
    const clock_enet_pll_config_t pll = {.enableClkOutput = true, .enableClkOutput25M = false, .loopDivider = 1};

    CLOCK_InitEnetPll(&pll);
    IOMUXC_EnableMode(IOMUXC_GPR, kIOMUXC_GPR_ENET1TxClkOutputDir, true);

    pins_init();
    phy_reset();

    CLOCK_EnableClock(kCLOCK_Enet);
    ENET_SetSMI(ENET, CLOCK_GetFreq(kCLOCK_IpgClk), false);

    s_phy_res.read = mdio_read;
    s_phy_res.write = mdio_write;

    phy_config_t phy_cfg;

    memset(&phy_cfg, 0, sizeof(phy_cfg));
    phy_cfg.phyAddr = PHY_ADDR;
    phy_cfg.ops = &phyksz8081_ops;
    phy_cfg.resource = &s_phy_res;
    phy_cfg.autoNeg = true;

    status_t st = PHY_Init(&s_phy, &phy_cfg);

    if (st != kStatus_Success)
    {
        PRINTF("enet: PHY init failed (%d)\r\n", (int)st);
        phy_dump("after failed init");
        return -1;
    }

    phy_dump("after init");

    enet_buffer_config_t bufs = {
        .rxBdNumber = RX_BD,
        .txBdNumber = TX_BD,
        .rxBuffSizeAlign = BUF_SIZE,
        .txBuffSizeAlign = BUF_SIZE,
        .rxBdStartAddrAlign = &g_enet_rx_bd[0],
        .txBdStartAddrAlign = &g_enet_tx_bd[0],
        .rxBufferAlign = &g_enet_rx_buf[0][0],
        .txBufferAlign = &g_enet_tx_buf[0][0],
        .rxMaintainEnable = false,
        .txMaintainEnable = false,
        .txFrameInfo = NULL,
    };

    enet_config_t cfg;

    ENET_GetDefaultConfig(&cfg);
    cfg.miiMode = kENET_RmiiMode;
    cfg.miiSpeed = kENET_MiiSpeed100M;
    cfg.miiDuplex = kENET_MiiFullDuplex;
    cfg.rxMaxFrameLen = ENET_FRAME_MAX_FRAMELEN;
    cfg.interrupt = 0; /* polled */
    cfg.callback = NULL;

    uint8_t mac_copy[6];

    memcpy(mac_copy, mac, 6);

    st = ENET_Init(ENET, &s_handle, &cfg, &bufs, mac_copy, CLOCK_GetFreq(kCLOCK_IpgClk));

    if (st != kStatus_Success)
    {
        PRINTF("enet: MAC init failed (%d)\r\n", (int)st);
        return -1;
    }

    /* the driver enabled the vector; nothing is unmasked, but keep it below the kernel */
    NVIC_SetPriority(ENET_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1);

    ENET_ActiveRead(ENET);

    s_inited = true;

    PRINTF("enet: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, waiting for the link\r\n", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);

    return 0;
}

bool enet_link_poll_phy(void)
{
    if (!s_inited)
        return false;

    bool link = false;
    static uint32_t polls = 0;

    if (PHY_GetLinkStatus(&s_phy, &link) != kStatus_Success)
        return s_up;

    /* while there is no link: the registers every ten seconds, to see why */
    if (!link && ((++polls % 100u) == 0u))
        phy_dump("no link");

    if (link && !s_up)
    {
        phy_speed_t speed = kPHY_Speed100M;
        phy_duplex_t duplex = kPHY_FullDuplex;

        (void)PHY_GetLinkSpeedDuplex(&s_phy, &speed, &duplex);

        ENET_SetMII(ENET, (speed == kPHY_Speed100M) ? kENET_MiiSpeed100M : kENET_MiiSpeed10M,
                    (duplex == kPHY_FullDuplex) ? kENET_MiiFullDuplex : kENET_MiiHalfDuplex);

        PRINTF("enet: link up, %s %s duplex\r\n", (speed == kPHY_Speed100M) ? "100 Mbit" : "10 Mbit",
               (duplex == kPHY_FullDuplex) ? "full" : "half");
    }
    else if (!link && s_up)
    {
        PRINTF("enet: link down\r\n");
    }

    s_up = link;

    return s_up;
}

bool enet_link_up(void)
{
    return s_up;
}

int enet_link_send(const void *frame, uint32_t len)
{
    if (!s_up)
        return ENET_LINK_DOWN;

    const status_t st = ENET_SendFrame(ENET, &s_handle, (const uint8_t *)frame, len, 0, false, NULL);

    if (st == kStatus_Success)
        return 0;

    if (st == kStatus_ENET_TxFrameBusy)
        return ENET_LINK_BUSY;

    return ENET_LINK_DOWN;
}

int enet_link_recv(uint8_t *buf, uint32_t max)
{
    if (!s_inited)
        return 0;

    uint32_t len = 0;
    const status_t st = ENET_GetRxFrameSize(&s_handle, &len, 0);

    if (st == kStatus_ENET_RxFrameEmpty)
        return 0;

    if ((st != kStatus_Success) || (len > max) || !len)
    {
        /* an error, or too big: the driver drops it on a NULL read */
        (void)ENET_ReadFrame(ENET, &s_handle, NULL, 0, 0, NULL);

        return -1;
    }

    if (ENET_ReadFrame(ENET, &s_handle, buf, len, 0, NULL) != kStatus_Success)
        return -1;

    return (int)len;
}
