/*
 * Raw Ethernet through the STM32 HAL ETH driver (the same HAL the Zephyr
 * Ethernet driver uses underneath), on the LAN8742 PHY of the STM32H7S78-DK.
 *
 * Receive: the HAL hands the DMA a buffer through HAL_ETH_RxAllocateCallback
 * and links what it filled through HAL_ETH_RxLinkCallback; HAL_ETH_ReadData
 * returns the chain of one frame. A frame fits one buffer (the buffers are
 * larger than a frame), so a chain is one buffer long here.
 * Transmit: one frame at a time, waited for - the GPU board sends little.
 *
 * Descriptors and buffers are in non-cacheable SRAM (CONFIG_NOCACHE_MEMORY),
 * so no cache maintenance is needed on either side.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <soc.h>

#include <stm32h7rsxx_hal.h>

#include "eth_raw.h"

LOG_MODULE_REGISTER(eth_raw, LOG_LEVEL_INF);

#define ETH_NODE DT_NODELABEL(mac)
#define MDIO_NODE DT_NODELABEL(mdio)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(ETH_NODE), "the board's Ethernet node is off");
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(MDIO_NODE), "the board's MDIO node is off");

/* the RMII pins are the MAC's; MDC (PC1, behind JP6) and MDIO (PA2) are the MDIO node's */
PINCTRL_DT_DEFINE(ETH_NODE);
PINCTRL_DT_DEFINE(MDIO_NODE);

#define PHY_ADDR 0U  /* LAN8742A on the DK: address 0 (see the board devicetree) */

/* IEEE 802.3 registers, and the LAN8742's speed/duplex status */
#define PHY_BMCR 0U
#define PHY_BMSR 1U
#define PHY_SCSR 31U
#define PHY_BMCR_RESET BIT(15)
#define PHY_BMCR_AUTONEG BIT(12)
#define PHY_BMCR_RESTART_AN BIT(9)
#define PHY_BMSR_AN_DONE BIT(5)
#define PHY_BMSR_LINK BIT(2)
#define PHY_SCSR_SPEED_MASK (7U << 2)
#define PHY_SCSR_100M BIT(3)
#define PHY_SCSR_FULL BIT(4)

#define RX_BUF_SIZE 1536U  /* > 1514 + CRC, a multiple of 32 */
#define RX_BUFS (ETH_RX_DESC_CNT * 2)
#define TX_BUF_SIZE 1536U

static ETH_HandleTypeDef heth;
static ETH_DMADescTypeDef rx_desc[ETH_RX_DESC_CNT] __nocache __aligned(32);
static ETH_DMADescTypeDef tx_desc[ETH_TX_DESC_CNT] __nocache __aligned(32);
/* the receive buffers are in the PSRAM (there are many, so that a burst at line
   rate never runs the DMA out of them); the CPU only ever reads them, after an
   invalidate, so the cache never holds anything the DMA could be overwritten by */
static uint8_t rx_buf[RX_BUFS][RX_BUF_SIZE] __attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram)))))
	__aligned(32);
static uint8_t tx_buf[TX_BUF_SIZE] __nocache __aligned(32);
static uint32_t rx_no_desc; /* the DMA had no descriptor for a frame (receive buffer unavailable) */

/* what the HAL chains: one per receive buffer */
struct rx_chunk {
	struct rx_chunk *next;
	uint16_t len;
	bool used;
};

static struct rx_chunk rx_chunk[RX_BUFS];
static uint8_t mac_addr[6];
static bool link_up;
static bool started;
static K_SEM_DEFINE(rx_sem, 0, 1);
static K_SEM_DEFINE(tx_sem, 0, 1);
static K_MUTEX_DEFINE(tx_mtx);
static struct eth_raw_stats stats;

/* ---- HAL callbacks ------------------------------------------------------- */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
	for (int i = 0; i < RX_BUFS; i++) {
		if (!rx_chunk[i].used) {
			rx_chunk[i].used = true;
			rx_chunk[i].next = NULL;
			rx_chunk[i].len = 0;
			*buff = rx_buf[i];
			return;
		}
	}
	stats.rx_dropped++;
	*buff = NULL;
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
	int i = (int)((buff - &rx_buf[0][0]) / RX_BUF_SIZE);
	struct rx_chunk *c = &rx_chunk[i];

	c->len = Length;
	c->next = NULL;
	if (*pStart == NULL) {
		*pStart = c;
	} else {
		((struct rx_chunk *)*pEnd)->next = c;
	}
	*pEnd = c;
}

void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
	ARG_UNUSED(buff);
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	k_sem_give(&rx_sem);
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *h)
{
	ARG_UNUSED(h);
	k_sem_give(&tx_sem);
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *h)
{
	if ((HAL_ETH_GetDMAError(h) & ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG) != 0U) {
		rx_no_desc++;
	}
	stats.tx_errors++;
	/* a receive buffer shortage stops the DMA: it goes on once buffers are back */
	k_sem_give(&rx_sem);
}

static void eth_isr(const void *arg)
{
	ARG_UNUSED(arg);
	stats.irqs++;
	HAL_ETH_IRQHandler(&heth);
}

/* ---- PHY ------------------------------------------------------------------ */

static int phy_read(uint32_t reg, uint32_t *val)
{
	return HAL_ETH_ReadPHYRegister(&heth, PHY_ADDR, reg, val) == HAL_OK ? 0 : -EIO;
}

static int phy_write(uint32_t reg, uint32_t val)
{
	return HAL_ETH_WritePHYRegister(&heth, PHY_ADDR, reg, val) == HAL_OK ? 0 : -EIO;
}

/* the PHY's registers on the console: identity, negotiation, the LAN8742's speed word */
static void phy_dump(const char *when)
{
	static const uint32_t regs[] = {0, 1, 2, 3, 4, 5, 17, 18, 27, 31};
	char line[160];
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		uint32_t v = 0;

		if (phy_read(regs[i], &v)) {
			n += snprintf(line + n, sizeof(line) - n, " r%u=????", (unsigned)regs[i]);
		} else {
			n += snprintf(line + n, sizeof(line) - n, " r%u=%04x", (unsigned)regs[i], (unsigned)v);
		}
		if (n >= sizeof(line) - 12) {
			break;
		}
	}
	LOG_INF("PHY %s:%s", when, line);
}

static int phy_init(void)
{
	uint32_t v;

	if (phy_write(PHY_BMCR, PHY_BMCR_RESET)) {
		return -EIO;
	}
	for (int i = 0; i < 100; i++) {
		k_msleep(5);
		if (phy_read(PHY_BMCR, &v) == 0 && (v & PHY_BMCR_RESET) == 0U) {
			break;
		}
	}
	if (phy_read(PHY_BMCR, &v) || (v & PHY_BMCR_RESET)) {
		LOG_ERR("PHY does not answer (BMCR %08x) - is JP6 in the PC1 position?", v);
		phy_dump("dead");
		return -EIO;
	}
	int ret = phy_write(PHY_BMCR, PHY_BMCR_AUTONEG | PHY_BMCR_RESTART_AN);

	phy_dump("after init");
	return ret;
}

/* the link came up: the MAC takes the negotiated speed and duplex, and runs */
static int mac_start(void)
{
	ETH_MACConfigTypeDef mac;
	uint32_t scsr = 0;

	(void)phy_read(PHY_SCSR, &scsr);
	if (HAL_ETH_GetMACConfig(&heth, &mac) != HAL_OK) {
		return -EIO;
	}
	mac.Speed = (scsr & PHY_SCSR_100M) ? ETH_SPEED_100M : ETH_SPEED_10M;
	mac.DuplexMode = (scsr & PHY_SCSR_FULL) ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;
	if (HAL_ETH_SetMACConfig(&heth, &mac) != HAL_OK) {
		return -EIO;
	}
	if (HAL_ETH_Start_IT(&heth) != HAL_OK) {
		LOG_ERR("HAL_ETH_Start_IT failed");
		return -EIO;
	}
	started = true;
	LOG_INF("link up: %s %s duplex", (scsr & PHY_SCSR_100M) ? "100 Mbit" : "10 Mbit",
		(scsr & PHY_SCSR_FULL) ? "full" : "half");
	return 0;
}

static void mac_stop(void)
{
	if (started) {
		(void)HAL_ETH_Stop_IT(&heth);
		started = false;
	}
	LOG_INF("link down");
}

bool eth_raw_link_poll(void)
{
	uint32_t bmsr = 0;
	static uint32_t polls;

	if (phy_read(PHY_BMSR, &bmsr)) {
		if ((++polls % 50) == 0) {
			LOG_WRN("PHY MDIO read fails - is JP6 in the PC1 position?");
		}
		return link_up;
	}
	if (!(bmsr & PHY_BMSR_LINK) && ((++polls % 50) == 0)) {
		phy_dump("no link");
	}
	/* the link bit latches low: read twice to see the current state. A low
	   first read while the link is up here means it went down and came back
	   between two polls - renegotiated, possibly to another duplex (a partner
	   that was not negotiating yet gives half duplex): the MAC has to take the
	   new result, or every frame collides with a full duplex partner */
	bool flapped = false;

	if ((bmsr & PHY_BMSR_LINK) == 0U) {
		flapped = link_up;
		(void)phy_read(PHY_BMSR, &bmsr);
	}
	bool up = (bmsr & PHY_BMSR_LINK) != 0U && (bmsr & PHY_BMSR_AN_DONE) != 0U;

	if (up && link_up && flapped) {
		mac_stop();
		link_up = false;
	}
	if (up && !link_up) {
		if (mac_start() == 0) {
			link_up = true;
		}
	} else if (!up && link_up) {
		mac_stop();
		link_up = false;
	}
	return link_up;
}

bool eth_raw_link_up(void)
{
	return link_up;
}

/* ---- init ----------------------------------------------------------------- */

int eth_raw_init(const uint8_t mac[6])
{
	static const struct stm32_pclken pclken[] = STM32_DT_CLOCKS(ETH_NODE);
	const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	int ret;

	memcpy(mac_addr, mac, 6);

	if (!device_is_ready(clk)) {
		return -ENODEV;
	}
	for (size_t i = 0; i < ARRAY_SIZE(pclken); i++) {
		ret = clock_control_on(clk, (clock_control_subsys_t)&pclken[i]);
		if (ret) {
			LOG_ERR("clock %u: %d", (unsigned)i, ret);
			return ret;
		}
	}
	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(ETH_NODE), PINCTRL_STATE_DEFAULT);
	if (ret) {
		LOG_ERR("pinctrl (mac): %d", ret);
		return ret;
	}
	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(MDIO_NODE), PINCTRL_STATE_DEFAULT);
	if (ret) {
		LOG_ERR("pinctrl (mdio): %d", ret);
		return ret;
	}

	heth.Instance = ETH;
	heth.Init.MACAddr = mac_addr;
	heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
	heth.Init.TxDesc = tx_desc;
	heth.Init.RxDesc = rx_desc;
	heth.Init.RxBuffLen = RX_BUF_SIZE;

	if (HAL_ETH_Init(&heth) != HAL_OK) {
		LOG_ERR("HAL_ETH_Init failed (%08x)", HAL_ETH_GetError(&heth));
		return -EIO;
	}

	IRQ_CONNECT(DT_IRQN(ETH_NODE), DT_IRQ(ETH_NODE, priority), eth_isr, NULL, 0);
	irq_enable(DT_IRQN(ETH_NODE));

	ret = phy_init();
	if (ret) {
		return ret;
	}
	LOG_INF("Ethernet ready, MAC %02x:%02x:%02x:%02x:%02x:%02x, waiting for the link", mac[0],
		mac[1], mac[2], mac[3], mac[4], mac[5]);
	return 0;
}

/* ---- data ----------------------------------------------------------------- */

int eth_raw_receive(eth_raw_sink_t sink, k_timeout_t timeout)
{
	void *chain = NULL;
	int n = 0;

	if (!started) {
		k_sleep(timeout);
		return 0;
	}
	(void)k_sem_take(&rx_sem, timeout);

	while (HAL_ETH_ReadData(&heth, &chain) == HAL_OK) {
		struct rx_chunk *c = chain;
		size_t len = 0;

		for (struct rx_chunk *p = c; p != NULL; p = p->next) {
			len += p->len;
		}
		if (c != NULL && c->next == NULL) {
			sys_cache_data_invd_range((void *)rx_buf[c - rx_chunk], c->len);
			sink(rx_buf[c - rx_chunk], c->len);
		} else if (c != NULL) {
			/* longer than a buffer: cannot be one of ours */
			stats.rx_dropped++;
		}
		for (struct rx_chunk *p = c; p != NULL;) {
			struct rx_chunk *nx = p->next;

			p->used = false;
			p = nx;
		}
		stats.rx_frames++;
		stats.rx_bytes += len;
		n++;
		chain = NULL;
	}
	return n;
}

int eth_raw_send(const void *frame, size_t len)
{
	ETH_BufferTypeDef buf = {
		.buffer = tx_buf,
		.len = len,
		.next = NULL,
	};
	ETH_TxPacketConfigTypeDef cfg = {
		.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD,
		.CRCPadCtrl = ETH_CRC_PAD_INSERT,
		.ChecksumCtrl = ETH_CHECKSUM_DISABLE,
		.Length = len,
		.TxBuffer = &buf,
		.pData = NULL,
	};
	int ret = 0;

	if (!started || len > TX_BUF_SIZE) {
		return -ENETDOWN;
	}
	k_mutex_lock(&tx_mtx, K_FOREVER);
	memcpy(tx_buf, frame, len);
	k_sem_reset(&tx_sem);
	if (HAL_ETH_Transmit_IT(&heth, &cfg) != HAL_OK) {
		stats.tx_errors++;
		ret = -EIO;
	} else if (k_sem_take(&tx_sem, K_MSEC(20)) != 0) {
		stats.tx_errors++;
		ret = -ETIMEDOUT;
	} else {
		stats.tx_frames++;
	}
	(void)HAL_ETH_ReleaseTxPacket(&heth);
	k_mutex_unlock(&tx_mtx);
	return ret;
}

void eth_raw_get_stats(struct eth_raw_stats *st)
{
	stats.rx_no_desc = rx_no_desc;
	*st = stats;
}
