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

#include "env.h"
#include <stdlib.h>
#include "util.h"

#define MAX_PTRS_PER_THREAD (32 * 1024)
#define MIN_PTRS_PER_THREAD 1024

static const char env_ptrs_per_thread[] = "THREADSCAN_PTRS_PER_THREAD";

// # of ptrs a thread can "save up" before initiating a collection run.
// The number of pointers per thread should be a power of 2 because we use
// this number to do masking (to avoid the costly modulo operation).
int g_threadscan_ptrs_per_thread;

/** Parse an integer from a string.  0 if val is NULL.
 */
static int get_int (const char *val, int default_val)
{
    if (NULL == val) return default_val;
    return atoi(val);
}

__attribute__((constructor (101)))
static void env_init ()
{
    // Pointers per thread -- how many pointers a thread can track before a
    // collection run occurs.  This should be a power of 2.  To avoid
    // complicated numbers the environment variable,
    // THREADSCAN_PTRS_PER_THREAD, is multiplied by 1024 so that a user can
    // think in terms of small powers of 2.
    {
        int ptrs_per_thread;
        // Default is ~16000 pointers per thread, derived from trial data.
        ptrs_per_thread = get_int(getenv(env_ptrs_per_thread), 4);
        ptrs_per_thread *= 1024;

        // Round up to power of 2 bit trick:
        --ptrs_per_thread;
        ptrs_per_thread |= ptrs_per_thread >> 1;
        ptrs_per_thread |= ptrs_per_thread >> 2;
        ptrs_per_thread |= ptrs_per_thread >> 4;
        ptrs_per_thread |= ptrs_per_thread >> 8;
        ptrs_per_thread |= ptrs_per_thread >> 16;
        ++ptrs_per_thread;

        // Bounds-checking.
        if (ptrs_per_thread < MIN_PTRS_PER_THREAD) {
            threadscan_diagnostic("warning: %s = %s\n"
                                  "  But min value is %d\n",
                                  env_ptrs_per_thread,
                                  getenv(env_ptrs_per_thread),
                                  MIN_PTRS_PER_THREAD / 1024);
            ptrs_per_thread = MIN_PTRS_PER_THREAD;
        } else if (ptrs_per_thread > MAX_PTRS_PER_THREAD) {
            threadscan_diagnostic("warning: %s = %s\n"
                                  "  But max value is %d\n",
                                  env_ptrs_per_thread,
                                  getenv(env_ptrs_per_thread),
                                  MAX_PTRS_PER_THREAD / 1024);
            ptrs_per_thread = MAX_PTRS_PER_THREAD;
        }

        g_threadscan_ptrs_per_thread = ptrs_per_thread;
    }
}
