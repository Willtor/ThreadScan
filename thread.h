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

#ifndef _THREAD_H_
#define _THREAD_H_

#include "util.h"

/**
 * Return the local metadata for this thread.
 */
thread_data_t *threadscan_thread_get_td ();

/**
 * Base routine of all threads that are created in the process.  The wrapper
 * for pthread_create() will call this function instead of the one the user
 * requested.  This allows us to hook each thread into the threadscan system
 * as it is made.  Then we call the user routine.
 */
void *threadscan_thread_base (void *arg);

/**
 * Do metadata cleanup for the thread before it exits.
 */
void threadscan_thread_cleanup ();

/**
 * Send the given signal to all threads in the process and return the number
 * of signals sent.
 */
int threadscan_thread_signal_all_but_me (int sig);

/**
 * Return the address range of the stack where the user has (or might have)
 * data.
 */
mem_range_t threadscan_thread_user_stack ();

/**
 * Raise the "helping" flag for this thread.
 */
void threadscan_thread_cleanup_raise_flag ();

/**
 * Lower the "helping" flag for this thread.
 */
void threadscan_thread_cleanup_lower_flag ();

/**
 * Try to become the reclaimer.  Return true if successful, false otherwise.
 */
int threadscan_thread_cleanup_try_acquire ();

/**
 * Give up reclaimer lock.
 */
void threadscan_thread_cleanup_release ();

#endif // !defined _THREAD_H_
