/*
 * The little the emulator's gpu.c wants from the rest of the emulator:
 * a logger and the VRAM allocation.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <soc.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/printk.h>

#include "gpu.h"
#include "log.h"
#include "psx_gpu_platform.h"

static uint16_t vram_mem[PSX_GPU_VRAM_SIZE / 2]
	__attribute__((section(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(psram))))) __aligned(64);

void *psxgpu_vram_alloc(size_t size)
{
	return (size <= sizeof(vram_mem)) ? vram_mem : NULL;
}

/* rxi's log.h: warnings and up, the rest is noise at emulation speed */
static int log_level = LOG_WARN;

void log_log(int32_t level, const char *file, int32_t line, const char *fmt, ...)
{
	static const char *const names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
	char buf[160];
	va_list ap;

	if (level < log_level) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printk("%s %s:%d: %s\n", names[level], file, line, buf);
}

void log_set_level(int32_t level)
{
	log_level = level;
}

void log_set_quiet(bool enable)
{
	log_level = enable ? LOG_FATAL + 1 : LOG_WARN;
}

const char *log_level_string(int32_t level)
{
	ARG_UNUSED(level);
	return "";
}

/* ---- the rasterizer profile (PSX_PROFILE, prof.h) ------------------------------ */

#include "prof.h"

psx_prof_t g_prof;

void psxgpu_prof_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	memset(&g_prof, 0, sizeof(g_prof));
}

/* cycles per class since the last call, as milliseconds, with the primitive counts */
void psxgpu_prof_report(void)
{
	static const char *const names[8] = {"cull", "flat", "blend", "tex", "tex15", "gour", "gtex", "rect"};
	char line[200];
	size_t n = 0;
	const uint32_t mhz = sys_clock_hw_cycles_per_sec() / 1000000u;

	for (int k = 0; k < 8; k++) {
		n += snprintf(line + n, sizeof(line) - n, " %s %ums/%u", names[k],
			      (unsigned)(g_prof.ras_cyc[k] / (mhz * 1000u)), (unsigned)g_prof.ras_cnt[k]);
		if (n >= sizeof(line) - 24) {
			break;
		}
	}
	printk("raster:%s | gp0 cmds %u px %uk fast %uk transp %uk pageswitch %u\n", line, (unsigned)g_prof.gp0cmds,
	       (unsigned)(g_prof.pixels / 1000u), (unsigned)(g_prof.px_fast / 1000u),
	       (unsigned)(g_prof.px_transp / 1000u), (unsigned)g_prof.tex_switch);
	memset(&g_prof, 0, sizeof(g_prof));
}

/* ---- how fast is the memory the VRAM lives in ---------------------------------- */

static uint32_t bench_sram[8192] __aligned(32); /* 32 KB in the internal SRAM, for comparison */

static uint32_t line_fill_cycles(volatile uint32_t *base, uint32_t words, uint32_t iters)
{
	/* cold cache lines, a random walk over the region, one word per line */
	uint32_t x = 12345u;
	uint32_t sum = 0;

	sys_cache_data_invd_range((void *)base, words * 4u);

	const uint32_t t0 = DWT->CYCCNT;

	for (uint32_t i = 0; i < iters; i++) {
		x = x * 1664525u + 1013904223u;
		sum += base[(x >> 8) % words & ~7u];
	}
	const uint32_t t = DWT->CYCCNT - t0;

	(void)sum;
	return t / iters;
}

void psxgpu_mem_bench(void)
{
	const uint32_t psram = line_fill_cycles((volatile uint32_t *)vram_mem, PSX_GPU_VRAM_SIZE / 4u, 20000u);
	const uint32_t sram = line_fill_cycles(bench_sram, 8192u, 20000u);

	/* sequential: a 256 KB read and a 256 KB write of VRAM */
	uint32_t t0 = DWT->CYCCNT;
	uint32_t acc = 0;

	for (uint32_t i = 0; i < 65536u; i += 8u) {
		acc += ((volatile uint32_t *)vram_mem)[i];
	}
	const uint32_t rd = DWT->CYCCNT - t0;

	t0 = DWT->CYCCNT;
	memset(vram_mem, 0, 262144u);
	sys_cache_data_flush_range(vram_mem, 262144u);
	const uint32_t wr = DWT->CYCCNT - t0;

	(void)acc;
	printk("memory: PSRAM %u cycles per cold line, SRAM %u | 256 KB sequential: read %u us, write+flush %u us\n",
	       (unsigned)psram, (unsigned)sram, (unsigned)k_cyc_to_us_floor32(rd),
	       (unsigned)k_cyc_to_us_floor32(wr));
}
