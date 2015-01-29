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

#ifndef _PROC_H_
#define _PROC_H_

#include <stddef.h>
#include "util.h"

thread_list_t *threadscan_proc_get_thread_list ();
void threadscan_proc_stack_from_addr (mem_range_t *mem_range, size_t addr);

/****************************************************************************/
/*                             Per-thread data                              */
/****************************************************************************/

void threadscan_proc_add_thread_data (thread_data_t *td);
void threadscan_proc_remove_thread_data (thread_data_t *td);
int threadscan_proc_signal_all_except (int sig, thread_data_t *except);
void threadscan_proc_wait_for_timestamp (size_t curr);

#endif // !defined _PROC_H_
