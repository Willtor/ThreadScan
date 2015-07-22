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

#include <assert.h>
#include "alloc.h"
#include "child.h"
#include <malloc.h>
#include "proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

/****************************************************************************/
/*                                  Macros                                  */
/****************************************************************************/

#define PTR_MASK(v) ((v) & ~3) // Mask off the low two bits.

#ifndef NDEBUG
#define assert_monotonicity(a, n)                       \
    __assert_monotonicity(a, n, __FILE__, __LINE__)
static void __assert_monotonicity (size_t *a, int n, const char *f, int line)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            threadscan_diagnostic("Error at %s:%d\n", f, line);
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

/****************************************************************************/
/*                            Search utilities.                             */
/****************************************************************************/

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

static void do_search (size_t *mem, size_t range_size, gc_data_t *gc_data)
{
    size_t i;
    size_t min_ptr, max_ptr;

    min_ptr = gc_data->addrs[0];
    max_ptr = gc_data->addrs[gc_data->n_addrs - 1]
        + gc_data->alloc_sz[gc_data->n_addrs - 1] - sizeof(size_t);

    assert(min_ptr <= max_ptr);

    for (i = 0; i < range_size; ++i) {
        size_t cmp = PTR_MASK(mem[i]);
        // PTR_MASK catches pointers that have been hidden through overloading
        // the two low-order bits.

        if (cmp < min_ptr || cmp > max_ptr) continue; // Out-of-range.

        // Level 1 search: Find the page the address would be on.
        int v = binary_search(cmp, gc_data->minimap, 0,
                              gc_data->n_minimap);
        // Level 2 search: Find the address within the page.
        int loc = binary_search(cmp, gc_data->addrs,
                                v * (PAGESIZE / sizeof(size_t)),
                                v == gc_data->n_minimap - 1
                                ? gc_data->n_addrs
                                : (v + 1) * (PAGESIZE / sizeof(size_t)));
        size_t addr = gc_data->addrs[loc];
        if (cmp >= addr && cmp < addr + gc_data->alloc_sz[loc]) {
            // It's a pointer somewhere into the allocated region of memory.
            ++gc_data->refs[loc];
        }
#ifndef NDEBUG
        else {
            int loc2 = binary_search(cmp, gc_data->addrs,
                                     0, gc_data->n_addrs);
            // FIXME: Assert does not catch all bad cases.
            assert(gc_data->addrs[loc2] != cmp);
        }
#endif
    }
}

static void search_range (mem_range_t *range, gc_data_t *gc_data)
{
    size_t *mem;

    assert(range);

    mem = (size_t*)range->low;
    assert_monotonicity(gc_data->addrs, gc_data->n_addrs);
    do_search(mem, (range->high - range->low) / sizeof(size_t), gc_data);
    return;
}

static gc_data_t *aggregate_gc_data (gc_data_t *data_list)
{
    gc_data_t *ret, *tmp;
    size_t n_addrs = 0;
    int list_count = 0;

    tmp = data_list;
    do {
        n_addrs += tmp->n_addrs;
        ++list_count;
    } while ((tmp = tmp->next));
    //threadscan_diagnostic("List of %d dealies.\n", list_count);

    assert(n_addrs != 0);

    // How many pages of memory are needed to store this many addresses?
    size_t pages_of_addrs = ((n_addrs * sizeof(size_t))
                             + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages of memory are needed to store the minimap?
    size_t pages_of_minimap = ((pages_of_addrs * sizeof(size_t))
                               + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages are needed to store the allocated size and reference
    // count arrays?
    size_t pages_of_count = ((n_addrs * sizeof(int))
                             + PAGE_SIZE - sizeof(int)) / PAGE_SIZE;
    // Total pages needed is the number of pages for the addresses, plus the
    // number of pages needed for the minimap, plus one (for the gc_data_t).
    char *p =
        (char*)threadscan_alloc_mmap((pages_of_addrs     // addr array.
                                      + pages_of_minimap // minimap.
                                      + pages_of_count   // ref count.
                                      + pages_of_count   // alloc size.
                                      + 1)               // struct page.
                                     * PAGE_SIZE);

    // Perform assignments as offsets into the block that was bulk-allocated.
    size_t offset = 0;
    ret = (gc_data_t*)p;
    offset += PAGE_SIZE;

    ret->addrs = (size_t*)(p + offset);
    offset += pages_of_addrs * PAGE_SIZE;

    ret->minimap = (size_t*)(p + offset);
    offset += pages_of_minimap * PAGE_SIZE;

    ret->refs = (int*)(p + offset);
    offset += pages_of_count * PAGE_SIZE;

    ret->alloc_sz = (int*)(p + offset);

    ret->n_addrs = n_addrs;

    // Copy the addresses over.
    char *dest = (char*)ret->addrs;
    tmp = data_list;
    do {
        memcpy(dest, tmp->addrs, tmp->n_addrs * sizeof(size_t));
        dest += tmp->n_addrs * sizeof(size_t);
    } while ((tmp = tmp->next));

    return ret;
}

static void generate_minimap (gc_data_t *gc_data)
{
    size_t i;

    assert(gc_data);
    assert(gc_data->addrs);
    assert(gc_data->minimap);

    gc_data->n_minimap = 0;
    for (i = 0; i < gc_data->n_addrs; i += (PAGESIZE / sizeof(size_t))) {
        gc_data->minimap[gc_data->n_minimap] = gc_data->addrs[i];
        ++gc_data->n_minimap;
    }
}

/**
 * Determine whether a "path" is the location of the given library.
 *
 * @return 1 if it is the library location, zero otherwise.
 */
static int is_lib (const char *library, const char *path)
{
    if ('/' != path[0]) return 0;

    int len = strlen(library);
    ++path;
    while (1) {
        if ('\0' == path[0]) return 0;
        ++path;
        if (0 == strncmp(library, path, len)
            && ('.' == path[len] || '-' == path[len])) {
            return 1;
        }
        while ('\0' != path[0] && '/' != path[0]) ++path;
    }
}

static int scan_memory (void *p,
                        size_t low,
                        size_t high,
                        const char *bits,
                        const char *path)
{
    // Decide whether this is a region we want to look at.

    if (bits[1] == '-') {
        // Memory is not writable.
        return 1;
    }
    if (bits[2] == 'x') {
        // Executable memory.  This is only writable if it is a
        // relocation table, so don't worry about it.
        return 1;
    }
    if (low == high) {
        // The wha?  But, you know... it happens.
        return 1;
    }
    if (low <= (size_t)stderr->_lock && high >= (size_t)stderr->_lock) {
        // FIXME: This is bad!  Bad, bad, bad!  What is a general way
        // to find the stuff statically allocated by libc?  stderr,
        // declared right next to its lock, lives in a different
        // section. (See: libio/stdfiles.c in glibc)
        return 1;
    }
    if (is_lib("libc", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (is_lib("libdl", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (is_lib("libthreadscan", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (bits[3] == 's') {
        threadscan_fatal("threadscan: internal error: %s\n",
                         "Writeable memory range was shared.");
    }
    if (0 == memcmp(path, "[stack:", 7)) {
        // Our stack.  Don't check that.  Note: This is not one of the other
        // threads' stacks because in the child process, there is only one
        // thread.
        return 1;
    }

    /* It looks like we've applied all of the criteria and have found a range
       that we want to scan, right?  Not quite.  What about memory allocated
       by _this_ module?  Unfortunately, we cannot apply a simple comparison
       of this range with any specific memory we've mmap'd.
       The /proc/<pid>/maps file consolidates ranges if it can so we
       (potentially) have a range that needs to be turned into Swiss Cheese of
       sub-ranges that we actually want to look at. */

    gc_data_t *gc_data = (gc_data_t*)p;
    mem_range_t big_range = { low, high };
    while (big_range.low != big_range.high) {
        mem_range_t next = threadscan_alloc_next_subrange(&big_range);
        if (next.low != next.high) {
            // This is a region of memory we want to scan.
            search_range(&next, gc_data);
        }
    }

    return 1;
}

static void report_to_parent (int fd, size_t addr)
{
    if (-1 == write(fd, (const void*)&addr, sizeof(size_t))) {
        // Failed to write to the pipe.  In this case, the rest of the child's
        // work is pointless.  This case happens, for example, if the parent
        // exited (cleanly or no).

        // Just exit quietly.
        exit(0);
    }
}

static int once_over (gc_data_t *gc_data, int fd)
{
    int savings = 0;
    int write_position = 0;
    int i, j;
    size_t min_val, max_val;

    min_val = gc_data->addrs[0];
    max_val = gc_data->addrs[gc_data->n_addrs - 1]
        + gc_data->alloc_sz[gc_data->n_addrs - 1] - sizeof(size_t);

    for (i = 0; i < gc_data->n_addrs; ++i) {
        assert((gc_data->addrs[i] & 0xF) == 0);
        assert(gc_data->addrs[i] != 0);

        assert(gc_data->refs[i] >= 0);
        if (gc_data->refs[i] == 0) {
            // No outstanding refs.  This can get free'd.
            size_t addr = gc_data->addrs[i];
            assert(addr != 0 && PTR_MASK(addr) == addr);
            report_to_parent(fd, addr);
            ++savings;

            // Go into the buffer referenced by addr, and for each 8 bytes see
            // if it's a reference to an address we're tracking.  If so,
            // decrement the ref count for that address.
            for (j = 0; j < gc_data->alloc_sz[i]; j += sizeof(size_t)) {
                size_t p = PTR_MASK(((size_t*)addr)[j / sizeof(size_t)]);
                if (p >= min_val && p <= max_val) {
                    // This could be an address.
                    int loc;
                    if (p < addr) {
                        loc = binary_search(p, gc_data->addrs, 0,
                                            write_position);
                    } else {
                        loc = binary_search(p, gc_data->addrs, i,
                                            gc_data->n_addrs);
                    }

                    if (gc_data->addrs[loc] == p
                        || gc_data->addrs[loc] + gc_data->alloc_sz[loc] > p) {
                        --gc_data->refs[loc];
                    }
                }
            }

        } else {
            // Still outstanding refs.
            if (write_position != i) { // Shift the address back.
                gc_data->addrs[write_position] = gc_data->addrs[i];
                gc_data->refs[write_position] = gc_data->refs[i];
                gc_data->alloc_sz[write_position] = gc_data->alloc_sz[i];
            }
            ++write_position;
        }
    }

    gc_data->n_addrs = write_position;
    return savings;
}

void threadscan_child (gc_data_t *gc_data_list, int fd)
{
    gc_data_t *gc_data;
    int i;

    // Collect all of the addresses into a single array, sort them, and
    // create a minimap (to make searching faster).
    gc_data = aggregate_gc_data(gc_data_list);
    threadscan_util_sort(gc_data->addrs, gc_data->n_addrs);
    assert_monotonicity(gc_data->addrs, gc_data->n_addrs);
    generate_minimap(gc_data);

    // Get the size of each malloc'd block.
    for (i = 0; i < gc_data->n_addrs; ++i) {
        assert(gc_data->alloc_sz[i] == 0);
        gc_data->alloc_sz[i] = malloc_usable_size((void*)gc_data->addrs[i]);
        assert(gc_data->alloc_sz[i] > 0);
    }

#ifndef NDEBUG
    for (i = 0; i < gc_data->n_addrs; ++i) {
        assert(gc_data->refs[i] == 0);
    }
#endif

    // Scan memory for references.
    threadscan_proc_map_iterate(scan_memory, (void*)gc_data);

    // Identify unreferenced memory and report back to the parent.
    int savings;
    do {
        savings = once_over(gc_data, fd);
    } while (savings > 0 && gc_data->n_addrs > 0);

    // Report unreclaimed memory.
    report_to_parent(fd, 0);
    for (i = 0; i < gc_data->n_addrs; ++i) {
        report_to_parent(fd, gc_data->addrs[i]);
    }
}
