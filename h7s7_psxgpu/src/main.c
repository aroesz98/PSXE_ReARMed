/*
 * PSX GPU on the STM32H7S78-DK.
 *
 * The emulator on the i.MX RT1050 sends its GPU commands over Ethernet (see
 * PSXE/link/psxe_link.h); this board keeps the VRAM, rasterizes with the very
 * same gpu.c and shows the picture on its panel.
 *
 *   receive thread   Ethernet frames -> the receive ring (link_ring.h)
 *   main thread      ring -> records -> gpu.c; vertical blank -> present;
 *                    STATUS frames back (flow control), statistics on the UART
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

#include "eth_raw.h"
#include "gpu.h"
#include "psx_gpu_platform.h"
#include "link_ring.h"
#include "nema_port.h"
#include "present.h"
#include "psxe_link.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const uint8_t mac_gpu[6] = PSXE_LINK_MAC_GPU;
static const uint8_t mac_cpu[6] = PSXE_LINK_MAC_CPU;

static uint8_t ring_mem[PSXE_LINK_RING_BYTES]
	__attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram))))) __aligned(64);
static struct link_ring ring;
static K_SEM_DEFINE(ring_sem, 0, 1);

static psx_gpu_t *gpu;

/* link state */
static uint32_t consumed;         /* payload bytes of frames fully processed     */
static uint32_t consumed_told;    /* ... as of the last STATUS                    */
static int64_t status_at;         /* when the last STATUS went out                */
static uint16_t rx_seq;
static bool rx_seq_known;
static bool resync;               /* frames were lost: wait for a command boundary */
static uint32_t errors;
static uint32_t lost_frames, resyncs, ring_full, bad_frames;
static uint32_t dropped_bytes; /* frames the ring had no room for: never stored, so consumed at once */
static uint32_t boot_id;
static uint32_t frames_in, words_gp0, gp1_words, vblanks;
static uint32_t busy_cyc;         /* cycles spent processing frames since the last report */
static int64_t last_frame_at;

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

/* ---- receive thread ---------------------------------------------------------- */

static void rx_sink(const uint8_t *frame, size_t len)
{
	if (len < PSXE_LINK_ETH_HDR + PSXE_LINK_HDR) {
		return;
	}
	if (frame[12] != (PSXE_LINK_ETHERTYPE >> 8) || frame[13] != (PSXE_LINK_ETHERTYPE & 0xffu)) {
		return; /* not ours */
	}
	if (!link_ring_put(&ring, frame + PSXE_LINK_ETH_HDR, (uint32_t)(len - PSXE_LINK_ETH_HDR))) {
		ring_full++;
		errors++;
		dropped_bytes += (uint32_t)(len - PSXE_LINK_ETH_HDR);
		return;
	}
	k_sem_give(&ring_sem);
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		(void)eth_raw_receive(rx_sink, K_MSEC(100));
	}
}

K_THREAD_DEFINE(rx_tid, 4096, rx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, 0);

/* ---- sending ---------------------------------------------------------------- */

/* the payload 4 byte aligned: the Ethernet header is 14 bytes */
static uint8_t tx_frame_mem[2 + PSXE_LINK_FRAME_MAX] __aligned(4);
#define tx_frame (tx_frame_mem + 2)
static uint16_t tx_seq;

static uint32_t *tx_begin(void)
{
	memcpy(tx_frame, mac_cpu, 6);
	memcpy(tx_frame + 6, mac_gpu, 6);
	tx_frame[12] = PSXE_LINK_ETHERTYPE >> 8;
	tx_frame[13] = PSXE_LINK_ETHERTYPE & 0xffu;
	tx_frame[14] = PSXE_LINK_VERSION;
	tx_frame[15] = 0;
	tx_frame[16] = (uint8_t)tx_seq;
	tx_frame[17] = (uint8_t)(tx_seq >> 8);
	tx_seq++;
	return (uint32_t *)(tx_frame + PSXE_LINK_ETH_HDR + PSXE_LINK_HDR);
}

static void tx_end(uint32_t *end)
{
	size_t len = (size_t)((uint8_t *)end - tx_frame);

	if (len < 60) {
		memset(tx_frame + len, 0, 60 - len);
		len = 60;
	}
	(void)eth_raw_send(tx_frame, len);
}

static void send_status(void)
{
	struct present_stats ps;
	uint32_t *w = tx_begin();

	present_get_stats(&ps);
	w[0] = PSXE_REC_HDR(PSXE_REC_STATUS, 0, PSXE_STATUS_WORDS);
	w[1 + PSXE_STATUS_CONSUMED] = consumed + dropped_bytes;
	w[1 + PSXE_STATUS_FLAGS] = PSXE_STATUS_FLAG_READY | (resync ? PSXE_STATUS_FLAG_RESYNC : 0u);
	w[1 + PSXE_STATUS_RING] = PSXE_LINK_RING_BYTES;
	w[1 + PSXE_STATUS_PRESENTED] = ps.frames;
	w[1 + PSXE_STATUS_ERRORS] = errors;
	w[1 + PSXE_STATUS_BOOT] = boot_id;
	tx_end(w + 1 + PSXE_STATUS_WORDS);
	consumed_told = consumed;
	status_at = k_uptime_get();
}

/* the words a VRAM -> CPU transfer returns, as GPUREAD would give them */
static void send_vram_read(void)
{
	while (gpu->c0_tsiz > 0) {
		uint32_t n = (uint32_t)(gpu->c0_tsiz + 1) / 2u;

		if (n > PSXE_LINK_PAYLOAD_WORDS - 1u) {
			n = PSXE_LINK_PAYLOAD_WORDS - 1u;
		}
		uint32_t *w = tx_begin();

		w[0] = PSXE_REC_HDR(PSXE_REC_VRAM, 0, n);
		for (uint32_t i = 0; i < n; i++) {
			w[1 + i] = psx_gpu_read32(gpu, 0);
		}
		tx_end(w + 1 + n);
	}
}

/* ---- the command stream ------------------------------------------------------- */

static void gpu_reset(void)
{
	psx_gpu_init(gpu, NULL);
	resync = false;
	LOG_INF("GPU reset by the CPU board");
}

static void feed_gp0(const uint32_t *w, uint32_t n)
{
	while (n) {
		if (gpu->state == GPU_STATE_RECV_DATA) {
			/* whole rows of an upload straight into VRAM */
			uint32_t done = psx_gpu_write_bulk(gpu, w, n);

			if (done) {
				w += done;
				n -= done;
				continue;
			}
		}
		psx_gpu_write32(gpu, 0, *w++);
		n--;
		if (gpu->c0_tsiz > 0) {
			send_vram_read();
		}
	}
}

static void process_frame(const uint8_t *p, uint32_t len)
{
	if (PSXE_LINK_HDR_VERSION(p) != PSXE_LINK_VERSION) {
		bad_frames++;
		errors++;
		return;
	}
	uint16_t seq = PSXE_LINK_HDR_SEQ(p);

	if (rx_seq_known && seq != rx_seq) {
		lost_frames += (uint16_t)(seq - rx_seq);
		errors++;
		resync = true;
	}
	rx_seq = (uint16_t)(seq + 1);
	rx_seq_known = true;
	frames_in++;

	const uint32_t *w = (const uint32_t *)(p + PSXE_LINK_HDR);
	uint32_t words = (len - PSXE_LINK_HDR) / 4u;

	while (words >= 1u) {
		const uint32_t h = *w++;
		const uint32_t n = PSXE_REC_LEN(h);

		words--;
		if (n > words) {
			bad_frames++;
			errors++;
			break;
		}
		switch (PSXE_REC_TYPE(h)) {
		case PSXE_REC_GP0:
			if (resync) {
				if (!(PSXE_REC_FLAGS(h) & PSXE_REC_GP0_BOUNDARY)) {
					break; /* mid-command: wait for a boundary */
				}
				gpu->state = GPU_STATE_RECV_CMD;
				resync = false;
				resyncs++;
			}
			feed_gp0(w, n);
			words_gp0 += n;
			break;
		case PSXE_REC_GP1:
			if (n >= 1u) {
				psx_gpu_write32(gpu, 4, w[0]);
				gp1_words++;
			}
			break;
		case PSXE_REC_VBLANK:
			if (n >= 1u) {
				psx_gpu_set_field(gpu, PSXE_VBLANK_FIELD(w[0]));
				vblanks++;
				present_vblank(gpu);
			}
			break;
		case PSXE_REC_RESET:
			gpu_reset();
			break;
		default:
			break;
		}
		w += n;
		words -= n;
	}
	consumed += len;
}

/* ---- main --------------------------------------------------------------------- */

int main(void)
{
	printk("\n*** PSX GPU board - %s ***\n", CONFIG_BOARD_TARGET);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif

	link_ring_init(&ring, ring_mem, sizeof(ring_mem));
	boot_id = k_cycle_get_32() | 1u;

	if (present_init() != 0) {
		LOG_ERR("display init failed");
		return 0;
	}
	psxgpu_prof_init();
	gpu = psx_gpu_create();
	psx_gpu_init(gpu, NULL);
	psxgpu_mem_bench();
	present_bench(gpu);
	if (gpu->vram == NULL) {
		LOG_ERR("no VRAM");
		return 0;
	}
	present_text("PSX GPU", "waiting for the CPU board (Ethernet)");

	if (eth_raw_init(mac_gpu) != 0) {
		present_text("PSX GPU", "Ethernet init failed");
		return 0;
	}

	int64_t t_log = k_uptime_get();
	int64_t t_link = 0;
	bool shown_waiting = true;
	bool was_up = false;

	while (1) {
		int64_t now = k_uptime_get();

		/* the PHY, every 200 ms */
		if (now - t_link >= 200) {
			t_link = now;
			bool up = eth_raw_link_poll();

			if (up != was_up) {
				was_up = up;
				if (up) {
					send_status();
				}
			}
		}

		/* everything the ring holds */
		uint32_t len;
		const uint8_t *p;
		int n = 0;

		while ((p = link_ring_peek(&ring, &len)) != NULL) {
			const uint32_t t0 = k_cycle_get_32();

			process_frame(p, len);
			busy_cyc += k_cycle_get_32() - t0;
			link_ring_drop(&ring, len);
			last_frame_at = now;
			shown_waiting = false;
			if (++n >= 64) {
				break; /* let the status go out */
			}
		}

		/* flow control: what was consumed, promptly; and a heartbeat */
		if (eth_raw_link_up()) {
			if ((consumed - consumed_told) >= 16u * 1024u ||
			    ((consumed != consumed_told) && (now - status_at >= 2)) ||
			    (now - status_at >= 250)) {
				send_status();
			}
		}

		if (n == 0) {
			(void)k_sem_take(&ring_sem, K_MSEC(2));
		}

		now = k_uptime_get();
		if (!shown_waiting && now - last_frame_at > 3000) {
			present_text("PSX GPU", eth_raw_link_up() ? "no commands from the CPU board"
							       : "no Ethernet link");
			shown_waiting = true;
		}

		if (now - t_log >= 5000) {
			struct eth_raw_stats es;
			struct present_stats ps;
			struct nema_port_stats ns;

			const uint32_t window_ms = (uint32_t)(now - t_log);
			const uint32_t busy_ms = k_cyc_to_ms_floor32(busy_cyc);

			t_log = now;
			eth_raw_get_stats(&es);
			present_get_stats(&ps);
			nema_port_get_stats(&ns);
			printk("link %s: rx %u frames %u KB (drop %u nodesc %u) tx %u (err %u) irq %u | "
			       "gp0 %u words, gp1 %u, vblank %u, lost %u, resync %u, ring full %u, "
			       "bad %u, ring %u/%u KB | presented %u (skipped %u) %ux%u mode %03x "
			       "repack %u us gpu %u us | busy %u%% (present %u%%) | gpu2d irq %u err %u timeouts %u\n",
			       eth_raw_link_up() ? "up" : "down", es.rx_frames, es.rx_bytes / 1024u,
			       es.rx_dropped, es.rx_no_desc, es.tx_frames, es.tx_errors, es.irqs, words_gp0, gp1_words,
			       vblanks, lost_frames, resyncs, ring_full, bad_frames,
			       ring.high_water / 1024u, ring.size / 1024u, ps.frames, ps.skipped,
			       ps.src_w, ps.src_h, ps.mode, ps.repack_us, ps.gpu_us,
			       (busy_ms * 100u) / (window_ms ? window_ms : 1u),
			       (ps.cpu_us_acc / 10u) / (window_ms ? window_ms : 1u), ns.cl_irqs, ns.err_irqs,
			       ns.timeouts);
			ring.high_water = 0;
			busy_cyc = 0;
			present_stats_reset();
			psxgpu_prof_report();
#if DT_NODE_EXISTS(DT_ALIAS(led0))
			if (gpio_is_ready_dt(&led)) {
				gpio_pin_toggle_dt(&led);
			}
#endif
		}
	}
	return 0;
}
