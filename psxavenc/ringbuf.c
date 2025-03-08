/*
psxavenc: MDEC video + SPU/XA-ADPCM audio encoder frontend

Copyright (c) 2025 spicyjpeg

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ringbuf.h"

void init_ring_buffer(ring_buffer_t *buf, size_t item_size, int initial_capacity) {
	assert(item_size > 0);

	if (initial_capacity > 0) {
		buf->data = malloc(item_size * initial_capacity);
		assert(buf->data != NULL);
	} else {
		buf->data = NULL;
	}

	buf->item_size = item_size;
	buf->capacity = initial_capacity;

	buf->head = 0;
	buf->tail = 0;
	buf->count = 0;
}

void destroy_ring_buffer(ring_buffer_t *buf) {
	if (buf->data != NULL) {
		free(buf->data);
		buf->data = NULL;
		buf->capacity = 0;
	}
}

#if 0
void grow_ring_buffer(ring_buffer_t *buf, int new_capacity) {
	assert(new_capacity >= buf->capacity && buf->item_size > 0);

	void *old_buffer = buf->data;
	void *old_items = (uint8_t *)buf->data + (buf->item_size * buf->head);

	buf->data = malloc(buf->item_size * new_capacity);
	assert(buf->data != NULL);

	void *new_items = (uint8_t *)buf->data + (buf->item_size * (new_capacity - buf->count));

	if (ring_buffer_is_contiguous(buf)) {
		memcpy(new_items, old_items, buf->item_size * buf->count);
	} else if (buf->count > 0) {
		// If the old buffer wraps around, join all items back into a single
		// contiguous region in the new buffer.
		size_t start_size = buf->item_size * buf->tail;
		size_t end_size = buf->item_size * (buf->capacity - buf->head);

		memcpy(new_items, old_items, end_size);
		memcpy((uint8_t *)new_items + end_size, old_buffer, start_size);
	}

	free(old_buffer);
	buf->capacity = new_capacity;
	buf->head = 0;
	buf->tail = buf->count;
}
#endif

void ring_buffer_append(ring_buffer_t *buf, int count) {
	//fprintf(stderr, "%d/%d +%d\n", buf->count, buf->capacity, count);
	assert(count >= 0 && count <= (buf->capacity - buf->count));

	buf->tail = (buf->tail + count) % buf->capacity;
	buf->count += count;
}

void ring_buffer_remove(ring_buffer_t *buf, int count) {
	//fprintf(stderr, "%d/%d -%d\n", buf->count, buf->capacity, count);
	assert(count >= 0 && count <= buf->count);

	buf->head = (buf->head + count) % buf->capacity;
	buf->count -= count;
}

void *ring_buffer_get_head(const ring_buffer_t *buf, int offset) {
	assert(offset <= buf->count && buf->count > 0);
	offset = (buf->head + offset) % buf->capacity;

	return (uint8_t *)buf->data + (buf->item_size * offset);
}

void *ring_buffer_get_tail(const ring_buffer_t *buf, int offset) {
	assert(offset <= buf->count);
	offset = (buf->tail - offset) % buf->capacity;

	return (uint8_t *)buf->data + (buf->item_size * offset);
}

bool ring_buffer_is_contiguous(const ring_buffer_t *buf) {
	return
		(buf->head + buf->count) == buf->tail ||
		(buf->head + buf->count) == buf->capacity;
}

int ring_buffer_get_contiguous_span(const ring_buffer_t *buf) {
	if (ring_buffer_is_contiguous(buf))
		return buf->capacity - buf->tail;
	else
		return buf->head - buf->tail;
}
