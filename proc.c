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
#include <errno.h>
#include "proc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

/****************************************************************************/
/*                                 Structs                                  */
/****************************************************************************/

/**
 * A mapline_t corresponds to a line of the maps file: /proc/<pid>/maps.  It
 * contains data the OS keeps around about mmap()'d ranges of memory.
 */

#define MAPLINE_LOCATION_SIZE 256

typedef struct mapline_t mapline_t;

struct mapline_t {
    unsigned long long range_begin, range_end;
    char bits[5];
    unsigned long long f1;
    unsigned int f2, f3;
    unsigned int f4;
    char location[MAPLINE_LOCATION_SIZE];
};

/****************************************************************************/
/*                             Static utilities                             */
/****************************************************************************/

/**
 * List of threads the system needs to know about for signalling and
 * stalling.  This list gets updated whenever a new thread is created or
 * whenever an old thread exits.
 */
static thread_list_t thread_list = { (thread_data_t*)0x1,
                                     PTHREAD_MUTEX_INITIALIZER };

thread_list_t *threadscan_proc_get_thread_list () { return &thread_list; }

/**
 * Path to the maps file for this process: /proc/<pid>/maps
 */
static const char procmap[] = "/proc/self/maps";

/**
 * Read a mapline_t from the proc map (in the OS pseudofile,
 * /proc/<pid>/maps).
 *
 * @return Nonzero, if a line was read, zero otherwise.
 */
static int read_mapline (FILE *fp, mapline_t *m)
{
    int n;

    if (feof(fp)) return 0;

    n = fscanf(fp, "%llx-%llx %s %llx %d:%d %d",
               &m->range_begin, &m->range_end, m->bits, &m->f1,
               &m->f2, &m->f3, &m->f4);

    m->location[0] = '\0';

    if (n != 7) {
        threadscan_fatal("threadscan internal error: "
                         "fscanf returned %d\n", n);
    } else {
        char c;
        while (' ' == (c = fgetc(fp)));
        if (c != '\n' && c != EOF) {
            m->location[0] = c;
            n = fscanf(fp, "%s\n", &m->location[1]); // FIXME: not safe.
            if (n != 1) {
                threadscan_fatal("threadscan internal error: "
                                 "fscanf returned %d\n",
                                 n);
            }
        }
    }

    return 1; // populated a mapline.
}

/****************************************************************************/
/****************************************************************************/

/**
 * Given an address, find the bounds of the stack on which it lives.  The
 * mem_range is populated based on the results.
 */
void threadscan_proc_stack_from_addr (mem_range_t *mem_range, size_t addr)
{
    FILE *fp;
    mapline_t mapline;

    assert(mem_range);
    assert(addr > 0);

    mem_range->low = mem_range->high = 0;

    if (NULL == (fp = fopen(procmap, "r"))) {
        threadscan_fatal("threadscan: unable to open memory map file.\n");
    }

    while (read_mapline(fp, &mapline)) {
        if (addr > mapline.range_begin &&
            addr < mapline.range_end) {
            mem_range->low = mapline.range_begin;
            mem_range->high = mapline.range_end;
            break;
        }
    }

    fclose(fp);
}

/****************************************************************************/
/*                             Per-thread data                              */
/****************************************************************************/

void threadscan_proc_add_thread_data (thread_data_t *td)
{
    // The list initialization will only actually happen if this is the
    // first time.
    threadscan_util_thread_list_init(&thread_list);
    threadscan_util_thread_list_add(&thread_list, td);
}

void threadscan_proc_remove_thread_data (thread_data_t *td)
{
    threadscan_util_thread_list_remove(&thread_list, td);
}

int threadscan_proc_signal_all_except (int sig, thread_data_t *except)
{
    int signal_count = 0;
    thread_data_t *td;

    // Yay!  C doesn't have lambdas!  So this is way uglier and more fragile
    // than it needs to be!  Thanks, C.
    FOREACH_IN_THREAD_LIST(td, &thread_list)
        assert(td);
        if (td != except && td->is_active) {
            int ret = pthread_kill(td->self, sig);
            if (EINVAL == ret) {
                threadscan_fatal("threadscan: "
                                 "pthread_kill() returned EINVAL.\n");
            } else if (ESRCH == ret) {
                threadscan_diagnostic("threadscan: "
                                      "pthread_kill() returned ESRCH.\n");
            } else {
                ++signal_count;
            }
        }
    ENDFOREACH_IN_THREAD_LIST(td, &thread_list);

    return signal_count;
}

/**
 * Wait for all threads that are trying to help out to discover the
 * current timestamp.
 */
void threadscan_proc_wait_for_timestamp (size_t curr)
{
    thread_data_t *td;
    FOREACH_IN_THREAD_LIST(td, &thread_list)
        assert(td);
        size_t stamp = td->local_timestamp;
        while (TIMESTAMP_IS_ACTIVE(stamp)
               && TIMESTAMP(stamp) != curr) {
            // This thread is still helping the previous round.
            pthread_yield();
            stamp = td->local_timestamp;
        }
    ENDFOREACH_IN_THREAD_LIST(td, &thread_list);

    // After the loop, all threads either know about the current timestamp
    // and will not try to do anything crazy with the heap, or they do not
    // know about the current timestamp but they will find out about it if
    // they try to help.
}

__attribute__((constructor))
static void proc_init ()
{
    thread_list.head = NULL;
}
