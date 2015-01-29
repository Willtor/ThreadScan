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

#include "env.h"
#include <stdlib.h>

// The number of pointers per thread should be a power of 2 because we use
// this number to do masking (to avoid the costly modulo operation).
int threadscan_ptrs_per_thread = 2 * 1024;

/** Parse an integer from a string.  0 if val is NULL.
 */
static int get_int (const char *val, int default_val)
{
    if (NULL == val) return default_val;
    return atoi(val);
}

__attribute__((constructor))
static void env_init ()
{
    // Pointers per thread -- how many pointers a thread can track before a
    // collection run occurs.
    {
        int ptrs_per_thread;
        ptrs_per_thread = get_int(getenv("THREADSCAN_PTRS_PER_THREAD"), 16);
        if (ptrs_per_thread < 1) {
            ptrs_per_thread = 1;
        } else if (ptrs_per_thread > 32) {
            ptrs_per_thread = 32;
        }
        threadscan_ptrs_per_thread = ptrs_per_thread * 1024;
    }
}
