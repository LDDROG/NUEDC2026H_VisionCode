#include "nmbs/nmbs_ring_queue.h"

#include <string.h>

static uint8_t NMBSRingQueueIsPowerOfTwo(uint16_t value)
{
    if (value == 0U)
    {
        return 0U;
    }

    return (uint8_t)((value & (uint16_t)(value - 1U)) == 0U);
}

uint8_t NMBSRingQueueInit(NMBSRingQueue *queue, uint8_t *buffer, uint16_t capacity)
{
    if ((queue == 0) || (buffer == 0) || (!NMBSRingQueueIsPowerOfTwo(capacity)))
    {
        return 0U;
    }

    queue->buffer = buffer;
    queue->capacity = capacity;
    queue->mask = (uint16_t)(capacity - 1U);
    queue->head = 0U;
    queue->tail = 0U;

    return 1U;
}

void NMBSRingQueueReset(NMBSRingQueue *queue)
{
    if (queue == 0)
    {
        return;
    }

    queue->head = 0U;
    queue->tail = 0U;
}

uint16_t NMBSRingQueueAvailable(const NMBSRingQueue *queue)
{
    if (queue == 0)
    {
        return 0U;
    }

    return (uint16_t)(queue->head - queue->tail);
}

uint16_t NMBSRingQueueFree(const NMBSRingQueue *queue)
{
    if (queue == 0)
    {
        return 0U;
    }

    return (uint16_t)(queue->capacity - NMBSRingQueueAvailable(queue));
}

uint8_t NMBSRingQueuePush(NMBSRingQueue *queue, uint8_t byte)
{
    uint16_t used;

    if (queue == 0)
    {
        return 0U;
    }

    used = (uint16_t)(queue->head - queue->tail);
    if (used >= queue->capacity)
    {
        return 0U;
    }

    queue->buffer[queue->head & queue->mask] = byte;
    queue->head++;

    return 1U;
}

uint8_t NMBSRingQueuePop(NMBSRingQueue *queue, uint8_t *out_byte)
{
    if ((queue == 0) || (out_byte == 0))
    {
        return 0U;
    }

    if (queue->head == queue->tail)
    {
        return 0U;
    }

    *out_byte = queue->buffer[queue->tail & queue->mask];
    queue->tail++;

    return 1U;
}

uint16_t NMBSRingQueuePushBytes(NMBSRingQueue *queue, const uint8_t *data, uint16_t size)
{
    uint16_t free_bytes;
    uint16_t write_pos;
    uint16_t first_chunk;

    if ((queue == 0) || (data == 0))
    {
        return 0U;
    }

    free_bytes = (uint16_t)(queue->capacity - (queue->head - queue->tail));
    if (free_bytes == 0U)
    {
        return 0U;
    }

    if (size > free_bytes)
    {
        size = free_bytes;
    }

    write_pos = queue->head & queue->mask;
    first_chunk = queue->capacity - write_pos;

    if (size <= first_chunk)
    {
        memcpy(&queue->buffer[write_pos], data, size);
    }
    else
    {
        memcpy(&queue->buffer[write_pos], data, first_chunk);
        memcpy(&queue->buffer[0], &data[first_chunk], (size_t)(size - first_chunk));
    }

    queue->head += size;
    return size;
}

uint16_t NMBSRingQueuePopBytes(NMBSRingQueue *queue, uint8_t *out_data, uint16_t size)
{
    uint16_t available;
    uint16_t read_pos;
    uint16_t first_chunk;

    if ((queue == 0) || (out_data == 0))
    {
        return 0U;
    }

    available = (uint16_t)(queue->head - queue->tail);
    if (available == 0U)
    {
        return 0U;
    }

    if (size > available)
    {
        size = available;
    }

    read_pos = queue->tail & queue->mask;
    first_chunk = queue->capacity - read_pos;

    if (size <= first_chunk)
    {
        memcpy(out_data, &queue->buffer[read_pos], size);
    }
    else
    {
        memcpy(out_data, &queue->buffer[read_pos], first_chunk);
        memcpy(&out_data[first_chunk], &queue->buffer[0], (size_t)(size - first_chunk));
    }

    queue->tail += size;
    return size;
}
