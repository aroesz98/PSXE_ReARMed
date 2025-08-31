#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "queue.h"

psx_queue_t *queue_create(void)
{
    return malloc(sizeof(psx_queue_t));
}

void queue_init(psx_queue_t *queue, uint32_t size)
{
    queue->buf = malloc(size);
    queue->read_index = 0;
    queue->write_index = 0;
    queue->size = size;
}

void queue_push(psx_queue_t *queue, uint8_t value)
{
    if (queue_is_full(queue))
        return;

    queue->buf[queue->write_index++] = value;
}

uint8_t queue_pop(psx_queue_t *queue)
{
    if (queue_is_empty(queue))
        return 0;

    uint8_t data = queue->buf[queue->read_index++];

    if (queue_is_empty(queue))
        queue_reset(queue);

    return data;
}

uint8_t queue_peek(psx_queue_t *queue)
{
    if (queue_is_empty(queue))
        return 0;

    return queue->buf[queue->read_index];
}

int32_t queue_is_empty(psx_queue_t *queue)
{
    return queue->read_index == queue->write_index;
}

int32_t queue_is_full(psx_queue_t *queue)
{
    return queue->write_index == queue->size;
}

void queue_reset(psx_queue_t *queue)
{
    queue->write_index = 0;
    queue->read_index = 0;
}

void queue_clear(psx_queue_t *queue)
{
    for (int32_t i = 0; i < queue->write_index; i++)
        queue->buf[i] = 0;

    queue_reset(queue);
}

int32_t queue_size(psx_queue_t *queue)
{
    return queue->write_index - queue->read_index;
}

int32_t queue_max_size(psx_queue_t *queue)
{
    return queue->size;
}

void queue_destroy(psx_queue_t *queue)
{
    free(queue->buf);
    free(queue);
}
