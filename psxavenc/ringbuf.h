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

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	void *data;
	size_t item_size;
	int capacity;

	int head;
	int tail;
	int count;
} ring_buffer_t;

void init_ring_buffer(ring_buffer_t *buf, size_t item_size, int initial_capacity);
void destroy_ring_buffer(ring_buffer_t *buf);
void ring_buffer_append(ring_buffer_t *buf, int count);
void ring_buffer_remove(ring_buffer_t *buf, int count);
void *ring_buffer_get_head(const ring_buffer_t *buf, int offset);
void *ring_buffer_get_tail(const ring_buffer_t *buf, int offset);
bool ring_buffer_is_contiguous(const ring_buffer_t *buf);
int ring_buffer_get_contiguous_span(const ring_buffer_t *buf);
