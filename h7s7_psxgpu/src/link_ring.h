/*
 * A ring of received frames: one producer (the Ethernet receive thread), one
 * consumer (the GPU thread). Entries are a 32 bit length and the payload,
 * padded to a word; a length of 0xffffffff means "wrapped, start over".
 */
#ifndef LINK_RING_H_
#define LINK_RING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/barrier.h>

struct link_ring {
	uint8_t *buf;
	uint32_t size;
	volatile uint32_t head;  /* producer writes here   */
	volatile uint32_t tail;  /* consumer reads here    */
	uint32_t high_water;
	uint32_t errors;         /* entries that made no sense: the ring was reset */
};

#define LINK_RING_WRAP 0xffffffffu

static inline void link_ring_init(struct link_ring *r, uint8_t *buf, uint32_t size)
{
	r->buf = buf;
	r->size = size;
	r->head = 0;
	r->tail = 0;
	r->high_water = 0;
	r->errors = 0;
}

static inline uint32_t link_ring_used(const struct link_ring *r)
{
	uint32_t h = r->head, t = r->tail;

	return h >= t ? h - t : r->size - t + h;
}

static inline uint32_t link_ring_free(const struct link_ring *r)
{
	return r->size - 1u - link_ring_used(r);
}

/* false when it does not fit: the caller drops the frame (flow control should prevent it) */
static inline bool link_ring_put(struct link_ring *r, const void *data, uint32_t len)
{
	uint32_t need = 4u + ((len + 3u) & ~3u);
	uint32_t h = r->head;
	uint32_t t = r->tail;

	if (h + need > r->size) {
		/* wrap: the marker needs 4 bytes at the end, the entry goes to the start */
		if (t > h || t == 0u) {
			return false; /* the consumer is between here and the end, or at 0 */
		}
		if (need >= t) {
			return false;
		}
		if (h + 4u <= r->size) {
			*(volatile uint32_t *)&r->buf[h] = LINK_RING_WRAP;
		}
		h = 0;
	} else {
		uint32_t free_now = (h >= t) ? (r->size - h + t) : (t - h);

		if (need >= free_now) {
			return false;
		}
	}
	memcpy(&r->buf[h + 4u], data, len);
	*(volatile uint32_t *)&r->buf[h] = len;
	barrier_dmem_fence_full();
	/* an entry that ends exactly at the end: the next one starts at 0, no marker */
	r->head = (h + need == r->size) ? 0u : h + need;
	uint32_t used = link_ring_used(r);

	if (used > r->high_water) {
		r->high_water = used;
	}
	return true;
}

/* the oldest entry, or NULL; the consumer calls link_ring_drop() when done with it */
static inline const uint8_t *link_ring_peek(struct link_ring *r, uint32_t *len)
{
	uint32_t t = r->tail;

	if (t == r->size) {
		/* the last entry ended exactly at the end (see link_ring_put) */
		r->tail = 0;
		t = 0;
	}
	if (t == r->head) {
		return NULL;
	}
	uint32_t l = *(volatile uint32_t *)&r->buf[t];

	if (l == LINK_RING_WRAP) {
		barrier_dmem_fence_full();
		r->tail = 0;
		t = 0;
		if (t == r->head) {
			return NULL;
		}
		l = *(volatile uint32_t *)&r->buf[t];
	}
	if (l > r->size - 8u) {
		/* not a length: the ring is out of step - start over rather than fault */
		barrier_dmem_fence_full();
		r->tail = r->head;
		r->errors++;
		return NULL;
	}
	barrier_dmem_fence_full();
	*len = l;
	return &r->buf[t + 4u];
}

static inline void link_ring_drop(struct link_ring *r, uint32_t len)
{
	barrier_dmem_fence_full();
	r->tail = r->tail + 4u + ((len + 3u) & ~3u);
}

#endif /* LINK_RING_H_ */
