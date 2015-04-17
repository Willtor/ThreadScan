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

#ifndef _PROC_H_
#define _PROC_H_

#include <stddef.h>
#include "util.h"

/**
 * Return the list of thread metadata objects for all the threads known to
 * threadscan.
 */
thread_list_t *threadscan_proc_get_thread_list ();

/**
 * Given an address, find the bounds of the stack on which it lives.  The
 * mem_range is populated based on the results.
 */
void threadscan_proc_stack_from_addr (mem_range_t *mem_range, size_t addr);

/****************************************************************************/
/*                             Per-thread data                              */
/****************************************************************************/

/**
 * Threads call this to register themselves with threadscan when they start.
 */
void threadscan_proc_add_thread_data (thread_data_t *td);

/**
 * Threads call this when they are going away.  It unregisters them with the
 * threadscan.
 */
void threadscan_proc_remove_thread_data (thread_data_t *td);

/**
 * Send a signal to all threads in the process (except the calling thread)
 * using pthread_kill().
 */
int threadscan_proc_signal_all_except (int sig, thread_data_t *except);

/**
 * Wait for all threads that are trying to help out to discover the
 * current timestamp.
 */
void threadscan_proc_wait_for_timestamp (size_t curr);

#endif // !defined _PROC_H_
