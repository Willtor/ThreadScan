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

thread_data_t *threadscan_thread_get_td ();
void *threadscan_thread_base (void *arg);
void threadscan_thread_cleanup ();
int threadscan_thread_signal_all_but_me (int sig);
void *threadscan_thread_call_on_safe_stack (void *(*f) (void *), void *arg);
mem_range_t threadscan_thread_user_stack ();
void threadscan_thread_save_stack_ptr(size_t sp);

void threadscan_thread_cleanup_raise_flag ();
void threadscan_thread_cleanup_lower_flag ();
int threadscan_thread_cleanup_try_acquire ();
void threadscan_thread_cleanup_release ();

#endif // !defined _THREAD_H_
