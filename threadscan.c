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

/****************************************************************************/
/*                                  Macros                                  */
/****************************************************************************/

#define SIGTHREADSCAN SIGUSR1

#define SCAN_MAP_OFFSET 0

#define PTR_MASK(v) ((v) & ~3) // Mask off the low two bits.

#define SET_LOW_BIT(p) do {                      \
        size_t v = (size_t)*(p);                 \
        if ((v & 1) == 0) { *(p) = (v + 1); }    \
    } while (0)

#ifndef NDEBUG
static void assert_monotonicity (size_t *a, int n)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            threadscan_fatal("The list is not monotonic at position %d "
                             "out of %d (%llu, last: %llu)\n",
                             i, n, a[i], last);
        }
        last = a[i];
    }
}
#else
#define assert_monotonicity(a, b) /* nothing. */
#endif

#define BINARY_THRESHOLD 32

#define GET_STACK_POINTER(qword)                \
    __asm__("movq %%rsp, %0"                    \
            : "=m"(qword)                       \
            : "r"("%rsp")                       \
            : )

/****************************************************************************/
/*                           Typedefs and structs                           */
/****************************************************************************/

typedef struct threadscan_data_t threadscan_data_t;

typedef struct addr_storage_t addr_storage_t;

typedef struct do_reclaim_arg_t do_reclaim_arg_t;

struct threadscan_data_t {
    int max_ptrs; // Max pointer count that can be tracked during reclamation.

    // Addresses being tracked for reclamation.
    int n_addrs;
    size_t *buf_addrs;

    // Scan map is a mini-map of buf_addrs.  One element in the scan map
    // corresponds to a page of addresses in buf_addrs.
    int n_scan_map;
    size_t *buf_scan_map;

    // Instead of allocating each of the above buffers, we allocate one _big_
    // buffer and then split it up.  The working_buffer_sz is the size of
    // that buffer, and the offset_list is used for assigning the pointers to
    // the individual sub-buffers.
    size_t working_buffer_sz;
    size_t offset_list[2];

    // Some pointers may not have been free'd.  We have to keep them around
    // for the next iteration.  storage is a buffer of un-free'd pointers.
    addr_storage_t *storage;
};

struct addr_storage_t {
    addr_storage_t *next;
    size_t length;
    size_t addrs[];
};

struct do_reclaim_arg_t {
    // Return values:
    size_t *addrs;
    int count;
};

/****************************************************************************/
/*                                 Globals                                  */
/****************************************************************************/

__attribute__((visibility("default")))
void threadscan_collect (void *ptr);

__attribute__((visibility("default")))
void threadscan_register_local_block (void *addr, size_t size);

static threadscan_data_t g_tsdata;

static volatile int self_stacks_searched = 1;

/****************************************************************************/
/*                            Pointer tracking.                             */
/****************************************************************************/

static void assign_working_space (char *buf)
{
    g_tsdata.buf_addrs = (size_t*)buf;
    g_tsdata.buf_scan_map =
        (size_t*)(buf + g_tsdata.offset_list[SCAN_MAP_OFFSET]);
}

/**
 * The remaining n pointers were unable to be free'd because there were
 * outstanding references.  Store them away until the next run.
 */
static void store_remaining_addrs (size_t *addrs, int n)
{
    addr_storage_t *tmp;

    if (n == 0) {
        // Nothing remaining, nothing to store.
        threadscan_alloc_munmap(addrs);
        return;
    }

    if (n + 2 > g_tsdata.max_ptrs * 2) {
        threadscan_fatal("threadscan internal error: "
                         "Ran out of storage space.\n");
    }

    // Convert the array of addresses to an addr_storage_t struct.  That
    // struct ends with an array of addresses, but it starts with a pointer
    // and a length field.  Move the first two addresses to the end and
    // reuse the space they used to occupy as the pointer and length fields.
    addrs[n] = addrs[0];
    addrs[n + 1] = addrs[1];
    tmp = (addr_storage_t*)addrs;
    tmp->length = n;

    do {
        tmp->next = g_tsdata.storage;
    } while (!BCAS(&g_tsdata.storage, tmp->next, tmp));
}

/**
 * Move addresses over to the current working list of addrs.  Add the
 * number of elements copied over into *n.
 */
static void add_to_buf_addrs (int *n, size_t *buf, int max)
{
    if (*n + max > g_tsdata.max_ptrs * 2) {
        threadscan_diagnostic("*n = %d, max = %d\n", *n, max);
        threadscan_fatal("threadscan internal error: "
                         "overflowed address list.\n");
    }

    memcpy(&g_tsdata.buf_addrs[*n], buf, max * sizeof(size_t));
    *n += max;
}

static int generate_working_pointers_list ()
{
    int n = 0;
    thread_list_t *thread_list = threadscan_proc_get_thread_list();
    thread_data_t *td;

    // Add leftover pointers.
    addr_storage_t *leftovers =
        __sync_lock_test_and_set(&g_tsdata.storage, NULL);

    while (leftovers) {
        add_to_buf_addrs(&n, leftovers->addrs, leftovers->length);
        addr_storage_t *tmp = leftovers;
        leftovers = leftovers->next;
        threadscan_alloc_munmap(tmp);
    }

    // Add the pointers from each of the individual thread buffers.
    FOREACH_IN_THREAD_LIST(td, thread_list)
        assert(td);
        n += threadscan_queue_pop_bulk(&g_tsdata.buf_addrs[n],
                                       g_tsdata.max_ptrs * 2 - n,
                                       &td->ptr_list);
    ENDFOREACH_IN_THREAD_LIST(td, thread_list);

    return n;
}

static void generate_scan_map ()
{
    int i;
    g_tsdata.n_scan_map = 0;
    for (i = 0; i < g_tsdata.n_addrs;
         i += (PAGESIZE / sizeof(size_t))) {
        g_tsdata.buf_scan_map[g_tsdata.n_scan_map] =
            g_tsdata.buf_addrs[i];
        ++g_tsdata.n_scan_map;
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

static int binary_search (size_t val, size_t *a, int min, int max)
{
    while (max - min >= BINARY_THRESHOLD) {
        int mid = (max + min) / 2;
        size_t cmp = a[mid];
        if (cmp == val) return mid;

        if (cmp > val) max = mid;
        else min = mid;
    }

    return iterative_search(val, a, min, max);
}

/****************************************************************************/
/*                            Search utilities.                             */
/****************************************************************************/

static void do_search (size_t *mem, size_t range_size)
{
    size_t i;
    size_t min_ptr, max_ptr;

    min_ptr = g_tsdata.buf_addrs[0];
    max_ptr = g_tsdata.buf_addrs[g_tsdata.n_addrs - 1];

    assert(min_ptr <= max_ptr);

    for (i = 0; i < range_size; ++i) {
        size_t cmp = PTR_MASK(mem[i]);

        // PTR_MASK catches pointers that have been hidden through overloading
        // the two low-order bits.

        if (cmp < min_ptr || cmp > max_ptr) continue;
        if (min_ptr == cmp) {
            SET_LOW_BIT(&g_tsdata.buf_addrs[0]);
            continue;
        } else if (max_ptr == cmp) {
            SET_LOW_BIT(&g_tsdata.buf_addrs[g_tsdata.n_addrs - 1]);
            continue;
        }

        // Level 1 search: Find the page the address would be on.
        int v = binary_search(cmp, g_tsdata.buf_scan_map, 0,
                              g_tsdata.n_scan_map);
        // Level 2 search: Find the address within the page.
        int loc = binary_search(cmp, g_tsdata.buf_addrs,
                                v * (PAGESIZE / sizeof(size_t)),
                                v == g_tsdata.n_scan_map - 1
                                ? g_tsdata.n_addrs
                                : (v + 1) * (PAGESIZE / sizeof(size_t)));
        if (g_tsdata.buf_addrs[loc] == cmp) {
            SET_LOW_BIT(&g_tsdata.buf_addrs[loc]);
        }
#ifndef NDEBUG
        else {
            int loc2 = binary_search(cmp, g_tsdata.buf_addrs,
                                     0, g_tsdata.n_addrs);
            assert(g_tsdata.buf_addrs[loc2] != cmp);
        }
#endif
    }
}

static void search_range (mem_range_t *mem_range)
{
    size_t *mem;

    assert(mem_range);

    mem = (size_t*)mem_range->low;
    assert_monotonicity(g_tsdata.buf_addrs, g_tsdata.n_addrs);
    do_search(mem, (mem_range->high - mem_range->low) / sizeof(size_t));
    return;
}

/****************************************************************************/
/*                           Post-search analysis                           */
/****************************************************************************/

static int handle_unreferenced_ptrs (size_t *addrs, int count)
{
    int write_position;
    int i;

    write_position = 0;
    for (i = 0; i < count; ++i) {
        if (addrs[i] & 1) {              // Outstanding reference.
            addrs[write_position] = PTR_MASK(addrs[i]);
            addrs[i] = 0;
            ++write_position;
        } else {                         // No remaining references.
            free((void*)addrs[i]);
            addrs[i] = 0;
        }
    }

    return write_position;
}

/****************************************************************************/
/*                             Cleanup thread.                              */
/****************************************************************************/

static void do_reclaim (size_t rsp, do_reclaim_arg_t *do_reclaim_arg)
{
    int sig_count;
    mem_range_t user_stack = threadscan_thread_user_stack();
    mem_range_t stack_search_range = { rsp, user_stack.high };
    mem_range_t *local_block = &threadscan_thread_get_td()->local_block;

    // Signal all of the threads that a scan is about to happen.
    self_stacks_searched = 0;
    sig_count = threadscan_thread_signal_all_but_me(SIGTHREADSCAN);

    // Check my stack for references.
    search_range(&stack_search_range);

    // Search the local region, if it's been set.
    if (local_block->low > 0) {
        search_range(local_block);
    }

    while (self_stacks_searched < sig_count) {
        __sync_synchronize(); // mfence.
    }

    do_reclaim_arg->addrs = g_tsdata.buf_addrs;
    do_reclaim_arg->count = g_tsdata.n_addrs;
}

static void threadscan_reclaim ()
{
    size_t rsp;
    do_reclaim_arg_t do_reclaim_arg;
    void *working_memory;

    GET_STACK_POINTER(rsp);

    working_memory = threadscan_alloc_mmap(g_tsdata.working_buffer_sz);
    assign_working_space(working_memory);
    g_tsdata.n_addrs = generate_working_pointers_list();

    // Sort the pointers and remove duplicates.
    threadscan_util_sort(g_tsdata.buf_addrs, g_tsdata.n_addrs);

    // Populate the scan_map: a minimap for searching for addresses.  This map
    // takes the first address on each page of memory and is used as a level 1
    // search that indicates where an address would be in the buf_addrs list,
    // if it's there at all.
    generate_scan_map();

    do_reclaim(rsp, &do_reclaim_arg);
    threadscan_thread_cleanup_release();

    // Check for pointers to free.  w00t!
    assert_monotonicity(do_reclaim_arg.addrs, do_reclaim_arg.count);
    int remaining =
        handle_unreferenced_ptrs(do_reclaim_arg.addrs,
                                 do_reclaim_arg.count);

    // There may be some remaining pointers that could not be free'd.  They
    // should be stored for the next round, and will be searched again until
    // there are no outstanding references to them.
    threadscan_util_randomize(do_reclaim_arg.addrs, remaining);
    store_remaining_addrs(do_reclaim_arg.addrs, remaining);
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
    threadscan_queue_push(&td->ptr_list, (size_t)ptr); // Add the pointer.
    while (threadscan_queue_is_full(&td->ptr_list)) {
        // While this thread's local queue of pointers is full, try to cleanup
        // or help with cleanup.  If someone else has already started cleanup,
        // this thread will break out of this loop soon enough.

        threadscan_thread_cleanup_try_acquire()
            ? threadscan_reclaim() // reclaim() will release the cleanup lock.
            : pthread_yield();
    }
}

__attribute__((visibility("default")))
void threadscan_register_local_block (void *addr, size_t size)
{
    mem_range_t *local_block = &threadscan_thread_get_td()->local_block;
    local_block->high = (size_t)addr + size;

    // Set "low" last.  This is for safety -- if a thread is setting this value
    // when a reclamation happens, it will check the "low" value, and if it
    // hasn't been set, there's no chance of funky reads.
    local_block->low = (size_t)addr;
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
    mem_range_t *local_block = &threadscan_thread_get_td()->local_block;

    assert(arg);

    // Search the stack for incriminating references.
    search_range(&stack_search_range);

    // Search the local region, if it's been set.
    if (local_block->low > 0) {
        search_range(local_block);
    }

    // Mark this thread done.
    __sync_fetch_and_add(&self_stacks_searched, 1);

    // Go back to work.
    return NULL;
}

/**
 * Got a signal from a thread wanting to do cleanup.
 */
static void signal_handler (int sig)
{
    size_t rsp;
    assert(SIGTHREADSCAN == sig);

    GET_STACK_POINTER(rsp);

    threadscan_thread_cleanup_raise_flag(); // FIXME: Do we need timestamps?
    search_self_stack((void*)rsp);
    threadscan_thread_cleanup_lower_flag();
}

/**
 * Like it sounds.
 */
__attribute__((constructor (201)))
static void register_signal_handlers ()
{
    /* We signal threads to get them to stop while we prepare a snapshot
       on the cleanup thread. */
    if (signal(SIGTHREADSCAN, signal_handler) == SIG_ERR) {
        threadscan_fatal("threadscan: Unable to register signal handler.\n");
    }

    g_tsdata.max_ptrs = g_threadscan_ptrs_per_thread
        * MAX_THREAD_COUNT;

    // Figure out how big the scan map needs to be.  It should be large
    // enough to store one pointer for every page in the main buffer of
    // addresses, and we round up to the nearest page to avoid sharing
    // with the buf_addrs.
    int scan_map_sz = (2 * g_tsdata.max_ptrs * sizeof(size_t) * sizeof(size_t))
        / PAGESIZE;
    if (scan_map_sz % PAGESIZE) {
        scan_map_sz += PAGESIZE;
        scan_map_sz &= ~(PAGESIZE - 1);
    }

    // Since we allocate all the buffers in a single allocation, do all the
    // necessary math to get the size of that alloc.  Also, calculate the
    // offsets into that big buffer for all of the sub-buffers.

    // Reserve space for buf_addrs.
    g_tsdata.working_buffer_sz = g_tsdata.max_ptrs * sizeof(size_t) * 2;
    g_tsdata.offset_list[SCAN_MAP_OFFSET] = g_tsdata.working_buffer_sz;

    // Reserve space for the scan map.
    g_tsdata.working_buffer_sz += scan_map_sz;

    g_tsdata.storage = NULL;
}
