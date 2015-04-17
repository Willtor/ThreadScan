/*
Copyright (c) 2015 ThreadScan authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef _QUEUE_H_
#define _QUEUE_H_

#include <stddef.h>

/**
 * Queues, in ThreadScan, are circular buffers that are thread-safe,
 * linearizable data structures, assuming single-reader, single-writer
 * usage.  A queue is initialized given the struct and a buffer that will
 * be used to store the values.
 *
 * Note: Unfortunately, C doesn't support templates, and "faking it" with
 * macros is ugly and hard to debug.  So if a type other than size_t is
 * ever needed, some code duplication will have to happen.
 */

/****************************************************************************/
/*                         Defines, typedefs, etc.                          */
/****************************************************************************/

typedef struct queue_t queue_t;

struct queue_t {
    size_t *e;                    // Buffer of elements.
    size_t capacity;              // Max storage.
    unsigned long long idx_head;  // Absolute idx: where values are inserted.
    unsigned long long idx_tail;  // Absolute idx: where values are removed.
};

/****************************************************************************/
/*                         FIFO queue manipulators.                         */
/****************************************************************************/

/**
 * Initialize a queue object.  Queues are implemented as circular buffers.
 */
void threadscan_queue_init (queue_t *q, size_t *buf, size_t capacity);

/**
 * Return 1 if the queue is full, zero otherwise.
 */
int threadscan_queue_is_full (queue_t *q);

/**
 * Push a value onto the head of the queue.  Caller must verify there is
 * space on the queue.
 */
void threadscan_queue_push (queue_t *q, size_t value);

/**
 * Remove a value from the tail of the queue and return it.
 */
size_t threadscan_queue_pop (queue_t *q);

/**
 * Push a block of values onto the queue of count "len".  Caller must verify
 * there is space on the queue.
 */
void threadscan_queue_push_bulk (queue_t *q, size_t values[], size_t len);

/**
 * Pop a block of values from the queue, up to "len" in count.  The values
 * buffer is populated with the removed values.  The total number of values
 * popped is returned.
 */
int threadscan_queue_pop_bulk (size_t values[], size_t len, queue_t *q);

#endif  // !defined _QUEUE_H_
