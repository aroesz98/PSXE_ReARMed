/*
 * Raw Ethernet on the STM32H7S78-DK: the HAL ETH driver and the LAN8742 PHY,
 * frames in and out, nothing above them. The link protocol does not need IP,
 * and the Zephyr network stack would copy every frame twice and schedule it
 * through two threads on the way.
 */
#ifndef ETH_RAW_H_
#define ETH_RAW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* a received frame, handed to the sink whole (Ethernet header included) */
typedef void (*eth_raw_sink_t)(const uint8_t *frame, size_t len);

int eth_raw_init(const uint8_t mac[6]);

/* PHY link state; polls the PHY, so call it every few hundred milliseconds */
bool eth_raw_link_poll(void);
bool eth_raw_link_up(void);

/* blocks until the frames the DMA has received are handed to the sink, or
   until the timeout; returns the number of frames */
int eth_raw_receive(eth_raw_sink_t sink, k_timeout_t timeout);

/* copies the frame into a DMA buffer and sends it; blocks until the DMA has
   taken it over */
int eth_raw_send(const void *frame, size_t len);

struct eth_raw_stats {
	uint32_t rx_frames;
	uint32_t rx_bytes;
	uint32_t rx_dropped;
	uint32_t rx_no_desc; /* frames the DMA had no descriptor for */   /* no free DMA buffer */
	uint32_t tx_frames;
	uint32_t tx_errors;
	uint32_t irqs;
};

void eth_raw_get_stats(struct eth_raw_stats *st);

#endif /* ETH_RAW_H_ */
