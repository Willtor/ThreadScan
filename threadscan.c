/*
Copyright (c) 2015 William M. Leiserson <willtor@mit.edu>

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

#define _GNU_SOURCE // For pthread_yield().
#include "alloc.h"
#include <assert.h>
#include "env.h"
#include "proc.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thread.h"
#include <unistd.h>
#include "util.h"

__attribute__((visibility("default")))
void threadscan_collect (void *ptr);

static volatile int self_stacks_searched = 1;

/****************************************************************************/
/*                                 Signals.                                 */
/****************************************************************************/

#define SIGTHREADSCAN SIGUSR1

/****************************************************************************/
/*                            Pointer tracking.                             */
/****************************************************************************/

#define PTR_MASK(v) ((v) & ~3) // Mask off the low two bits.

static int max_ptrs;

static size_t *working_addrs;
static int *working_refs;
static size_t *working_scan_map;

static size_t working_buffer_sz;
static size_t offset_list[2];

static int working_pointers = 1;
static int working_scan_map_ptrs = 1;

static void assign_working_space (char *buf)
{
    working_addrs = (size_t*)buf;
    working_refs = (int*)(buf + offset_list[0]);
    working_scan_map = (size_t*)(buf + offset_list[1]);
}

#ifndef NDEBUG
static void assert_monotonicity (size_t *a, int n)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            threadscan_fatal("The list is not monotonic at position %d "
                             "out of %d\n",
                             i, n);
        }
        last = a[i];
    }
}
#else
#define assert_monotonicity(a, b) /* nothing. */
#endif

typedef struct addr_storage_t addr_storage_t;

struct addr_storage_t {
    addr_storage_t *next;
    size_t length;
    size_t addrs[];
};

static addr_storage_t *storage = (addr_storage_t*)1;

static void store_remaining_addrs (size_t *addrs, int n)
{
    addr_storage_t *tmp;

    if (n == 0) {
        // Nothing remaining, nothing to store.
        threadscan_alloc_munmap(addrs);
        return;
    }

    if (n + 2 > max_ptrs * 2) {
        threadscan_fatal("threadscan internal error: "
                         "Ran out of storage space.\n");
    }

    addrs[n] = addrs[0];
    addrs[n + 1] = addrs[1];

    tmp = (addr_storage_t*)addrs;
    tmp->length = n;
    do {
        tmp->next = storage;
    } while (!BCAS(&storage, tmp->next, tmp));
}

static void add_addrs (int *n, size_t *buf, int max)
{
    int i;

    if (*n + max > max_ptrs * 2) {
        threadscan_diagnostic("*n = %d, max = %d\n", *n, max);
        threadscan_fatal("threadscan internal error: "
                         "overflowed address list.\n");
    }

    for (i = 0; i < max; ++i, ++*n) {
        working_addrs[*n] = buf[i];
        if (working_addrs[*n] == 0) {
            // Don't add zeroes, if it can be helped.
            --*n;
        } else {
            BCAS(&buf[i], working_addrs[*n], 0);
        }
        assert(working_refs[*n] == 0);
    }
}

static int generate_working_pointers_list ()
{
    int n = 0;
    thread_list_t *thread_list = threadscan_proc_get_thread_list();
    thread_data_t *td;

    // Add leftover pointers.
    addr_storage_t *leftovers = __sync_lock_test_and_set(&storage, NULL);

    while (leftovers) {
        add_addrs(&n, leftovers->addrs, leftovers->length);
        addr_storage_t *tmp = leftovers;
        leftovers = leftovers->next;
        threadscan_alloc_munmap(tmp);
    }

    // Add the pointers from each of the individual thread buffers.
    FOREACH_IN_THREAD_LIST(td, thread_list)
        assert(td);
        if (td->ptr_list_write
            > td->ptr_list_end - threadscan_ptrs_per_thread) {
            // Pointers to be collected.
            int ptr_list_write = td->ptr_list_write;
            int write_idx = PTR_LIST_INDEXIFY(ptr_list_write);
            int start_idx = PTR_LIST_INDEXIFY(td->ptr_list_end
                                              - threadscan_ptrs_per_thread);
            int elements;
            // The following is just copying out of the buffer.  Since the
            // buffer is circular, there is the possibility that the start
            // point is actually ahead of the write pointer.
            if (write_idx <= start_idx) {
                elements = threadscan_ptrs_per_thread - start_idx;
                memcpy(&working_addrs[n], &td->ptr_list[start_idx],
                       elements * sizeof(size_t));
                n += elements;
                start_idx = 0;
            }
            elements = write_idx - start_idx;
            if (elements > 0) {
                memcpy(&working_addrs[n], &td->ptr_list[start_idx],
                       elements * sizeof(size_t));
                n += elements;
            }
            // Set the end [absolute] index so the owner of this data can
            // continue adding pointers.
            td->ptr_list_end = ptr_list_write + threadscan_ptrs_per_thread;
        }
    ENDFOREACH_IN_THREAD_LIST(td, thread_list);

    return n;
}

static void generate_scan_map ()
{
    int i;
    working_scan_map_ptrs = 0;
    for (i = 0; i < working_pointers; i += (PAGESIZE / sizeof(size_t))) {
        working_scan_map[working_scan_map_ptrs] = working_addrs[i];
        ++working_scan_map_ptrs;
    }
}

static int iterative_search (size_t val, size_t *a, int min, int max)
{
    if (a[min] > val || min == max) return min;
    for ( ; min < max; ++min) {
        size_t cmp = a[min];
        if (cmp == val) return min;
        if (cmp > val) break;
    }
    return min - 1;
}

#define BINARY_THRESHOLD 32

static int binary_search (size_t val, size_t *a, int min, int max)
{
    if (max - min < BINARY_THRESHOLD) {
        return iterative_search(val, a, min, max);
    }

    int mid = (max + min) / 2;
    size_t cmp = a[mid];
    if (cmp == val) return mid;
    if (cmp > val) return binary_search(val, a, min, mid);
    return binary_search(val, a, mid, max);
}

static int condense (size_t *ptrs, int n)
{
    int i, last;

    last = 0;
    for (i = 0; i < n; ++i) {
        if (ptrs[i] != 0) {
            if (last != i) {
                if (!BCAS(&ptrs[last], 0, ptrs[i])) {
                    ++last;
                    --i;
                    continue;
                }
                if (!BCAS(&ptrs[i], ptrs[last], 0)) {
                    threadscan_diagnostic("Failed my CAS.\n");
                    abort();
                }
            }
            ++last;
        }
    }

#ifndef NDEBUG
    for (i = 1; i < last; ++i) {
        // No duplicates.
        assert(ptrs[i] != ptrs[i - 1]);
    }
#endif

    return last;
}

static void add_ptr (thread_data_t *td, size_t sptr)
{
    assert(td);
    assert(td->ptr_list);
    assert(td->ptr_list_end != td->ptr_list_write);

    // Insert the pointer into the circular buffer.
    td->ptr_list[PTR_LIST_INDEXIFY(td->ptr_list_write)] = sptr;
    ++td->ptr_list_write;
}

/****************************************************************************/
/*                            Search utilities.                             */
/****************************************************************************/

static void do_search (size_t *mem, size_t range_size)
{
    size_t i;
    size_t min_ptr, max_ptr;

    min_ptr = working_addrs[0];
    max_ptr = working_addrs[working_pointers - 1];

    assert(min_ptr <= max_ptr);

    for (i = 0; i < range_size; ++i) {
        size_t cmp = PTR_MASK(mem[i]);

        if (cmp < min_ptr || cmp > max_ptr) continue;
        if (min_ptr == cmp) {
            __sync_fetch_and_add(&working_refs[0], 1);
            continue;
        } else if (max_ptr == cmp) {
            __sync_fetch_and_add(&working_refs[working_pointers - 1], 1);
            continue;
        }

        int v = binary_search(cmp, working_scan_map, 0, working_scan_map_ptrs);
        int loc = binary_search(cmp, working_addrs,
                                v * (PAGESIZE / sizeof(size_t)),
                                v == working_scan_map_ptrs - 1
                                ? working_pointers
                                : (v + 1) * (PAGESIZE / sizeof(size_t)));
        if (working_addrs[loc] == cmp) {
            __sync_fetch_and_add(&working_refs[loc], 1);
        }
#ifndef NDEBUG
        else {
            int loc2 = binary_search(cmp, working_addrs,
                                     0, working_pointers);
            assert(working_addrs[loc2] != cmp);
        }
#endif
    }
}

static void search_range (mem_range_t *mem_range)
{
    size_t *mem;

    assert(mem_range);

    mem = (size_t*)mem_range->low;
    assert_monotonicity(working_addrs, working_pointers);
    do_search(mem, (mem_range->high - mem_range->low) / sizeof(size_t));
    return;
}

/****************************************************************************/
/*                           Post-search analysis                           */
/****************************************************************************/

static int handle_unreferenced_ptrs (size_t *addrs, int *refs, int count)
{
    int write_position;
    int i;

    write_position = 0;
    for (i = 0; i < count; ++i) {
        if (refs[i] == 0) {
            free((void*)addrs[i]);
            addrs[i] = 0;
        } else if (i != write_position) {
            addrs[write_position] = addrs[i];
            addrs[i] = 0;
        }
    }

    return write_position;
}

/****************************************************************************/
/*                             Cleanup thread.                              */
/****************************************************************************/

typedef struct do_cleanup_arg_t do_cleanup_arg_t;

struct do_cleanup_arg_t {
    // Return values:
    size_t *addrs;
    int *refs;
    int count;
};

static void do_cleanup (size_t rsp, do_cleanup_arg_t *do_cleanup_arg)
{
    int sig_count;
    mem_range_t user_stack = threadscan_thread_user_stack();
    mem_range_t stack_search_range = { rsp, user_stack.high };

    // Signal all of the threads that a scan is about to happen.
    self_stacks_searched = 0;
    sig_count = threadscan_thread_signal_all_but_me(SIGTHREADSCAN);

    // Check my stack for references.
    search_range(&stack_search_range);

    while (self_stacks_searched < sig_count) {
        __sync_synchronize(); // mfence.
    }

    do_cleanup_arg->addrs = working_addrs;
    do_cleanup_arg->refs = working_refs;
    do_cleanup_arg->count = working_pointers;
}

static void threadscan_cleanup ()
{
    size_t rsp;
    do_cleanup_arg_t do_cleanup_arg;
    void *working_memory;

    __asm__("movq %%rsp, %0"
            : "=m"(rsp)
            : "r"("%rsp")
            : );

    working_memory = threadscan_alloc_mmap(working_buffer_sz);
    assign_working_space(working_memory);
    working_pointers = generate_working_pointers_list();
    //threadscan_diagnostic("Starting with %d\n", working_pointers);

    // Sort the pointers and remove duplicates.
    threadscan_util_sort(working_addrs, working_pointers);
    condense(working_addrs, working_pointers);
    for ( ;
          working_pointers > 0 && working_addrs[working_pointers - 1] == 0;
          --working_pointers );
    generate_scan_map();

    do_cleanup(rsp, &do_cleanup_arg);
    threadscan_thread_cleanup_release();

    // Check for pointers to free.  w00t!
    assert_monotonicity(do_cleanup_arg.addrs, do_cleanup_arg.count);
    int remaining =
        handle_unreferenced_ptrs(do_cleanup_arg.addrs,
                                 do_cleanup_arg.refs,
                                 do_cleanup_arg.count);
    //threadscan_diagnostic("Remaining: %d\n", remaining);

    threadscan_util_randomize(do_cleanup_arg.addrs, remaining);
    store_remaining_addrs(do_cleanup_arg.addrs, remaining);
}

/**
 * Interface for applications.  "Collecting" a pointer registers it with
 * threadscan.  When a sweep of memory occurs, all registered pointers are
 * sought in memory.  Any that can't be found are free()'d because no
 * remaining threads have pointers to them.
 */
__attribute__((visibility("default")))
void threadscan_collect (void *ptr)
{
    if (NULL == ptr) {
        threadscan_diagnostic("Tried to collect NULL.\n");
        return;
    }

    thread_data_t *td = threadscan_thread_get_td();
    add_ptr(td, (size_t)ptr);
    while (td->ptr_list_write == td->ptr_list_end) {
        // While this thread's local queue of pointers is full, try to cleanup
        // or help with cleanup.  If someone else has already started cleanup,
        // this thread will break out of this loop soon enough.

        threadscan_thread_cleanup_try_acquire()
            ? threadscan_cleanup() // cleanup() will release the cleanup lock.
            : pthread_yield();
    }
}

/****************************************************************************/
/*                            Bystander threads.                            */
/****************************************************************************/

/**
 * Perform a search of the thread stack for pointers to objects that have
 * been removed.
 */
static void *search_self_stack (void *arg)
{
    mem_range_t user_stack = threadscan_thread_user_stack();
    mem_range_t stack_search_range = { (size_t)arg, user_stack.high };

    assert(arg);

    // Search the stack for incriminating references.
    search_range(&stack_search_range);
    __sync_fetch_and_add(&self_stacks_searched, 1);

    // Go back to work.
    return NULL;
}

/**
 * Got a signal from a thread wanting to do cleanup.  The threadscan signal
 * cannot be handled on this stack, though, because the algorithm disallows
 * writes to it.  Switch stacks and continue.
 */
static void signal_handler (int sig)
{
    size_t rsp;
    assert(SIGTHREADSCAN == sig);

    __asm__("movq %%rsp, %0"
            : "=m"(rsp)
            : "r"("%rsp")
            : );

    threadscan_thread_save_stack_ptr(rsp);
    threadscan_thread_cleanup_raise_flag();
    search_self_stack((void*)rsp);
    threadscan_thread_cleanup_lower_flag();
}

/**
 * Like it sounds.
 */
__attribute__((constructor))
static void register_signal_handlers ()
{
    /* We signal threads to get them to stop while we prepare a snapshot
       on the cleanup thread. */
    if (signal(SIGTHREADSCAN, signal_handler) == SIG_ERR) {
        threadscan_fatal("threadscan: Unable to register signal handler.\n");
    }

    max_ptrs = threadscan_ptrs_per_thread * 80; // FIXME: hardcoded thread limit: 80
    // FIXME: Need to do it a better way that doesn't require a hardcoded limit.
    // Probably not hard -- just leftover code from old implementation.

    int scan_map_sz = (2 * max_ptrs * sizeof(size_t) * sizeof(size_t))
        / PAGESIZE;
    if (scan_map_sz % PAGESIZE) {
        scan_map_sz += PAGESIZE;
        scan_map_sz &= ~(PAGESIZE - 1);
    }
    working_buffer_sz = (sizeof(size_t) * 4 + sizeof(int) * 2) * max_ptrs
        + scan_map_sz;

    offset_list[0] = max_ptrs * sizeof(size_t) * 2;
    offset_list[1] = offset_list[0] + (max_ptrs * sizeof(int) * 2);

    storage = NULL;
}
