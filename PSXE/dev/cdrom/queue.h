#ifndef PSX_CDROM_QUEUE_H
#define PSX_CDROM_QUEUE_H

#include <stdint.h>
#include <stddef.h>

typedef struct psx_queue_t
{
    uint8_t *buf;
    uint32_t read_index;
    uint32_t write_index;
    uint32_t size;
} psx_queue_t;

psx_queue_t *queue_create(void);
void queue_init(psx_queue_t *queue, uint32_t size);
void queue_push(psx_queue_t *queue, uint8_t value);
uint8_t queue_pop(psx_queue_t *queue);
uint8_t queue_peek(psx_queue_t *queue);
int32_t queue_is_empty(psx_queue_t *queue);
int32_t queue_is_full(psx_queue_t *queue);
void queue_reset(psx_queue_t *queue);
void queue_clear(psx_queue_t *queue);
int32_t queue_size(psx_queue_t *queue);
int32_t queue_max_size(psx_queue_t *queue);
void queue_destroy(psx_queue_t *queue);

#endif
