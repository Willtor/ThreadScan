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

#include "alloc.h"
#include <alloca.h>
#include <assert.h>
#include "proc.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "thread.h"
#include "util.h"

/**
 * Thread-local reference to threadscan's per-thread data.
 */
__thread thread_data_t *threadscan_local_td;

/**
 * Return the local metadata for this thread.
 */
thread_data_t *threadscan_thread_get_td () { return threadscan_local_td; }

/**
 * Base routine of all threads that are created in the process.  The wrapper
 * for pthread_create() will call this function instead of the one the user
 * requested.  This allows us to hook each thread into the threadscan system
 * as it is made.  Then we call the user routine.
 */
void *threadscan_thread_base (void *arg)
{
    thread_data_t *td = (thread_data_t*)arg;
    size_t sp, buffer_size;
    void *unused_buffer;

    // Get the stack bounds for disabling writes, later.  The stack is
    // preserved up to this point and does not have any user data on it, but
    // it _does_ have pthread stuff that libpthread will want to write and
    // be very unhappy about trying to write if we disable writes.
    __asm__("movq %%rsp, %0"
            : "=m"(sp)
            : "r"("%rsp")
            : );
    buffer_size = (sp & ((PAGESIZE - 1)));
    unused_buffer = alloca(buffer_size);
    if (unused_buffer > 0) {
        memset(unused_buffer, 0xDEADBEEF, buffer_size);
    }

    td->user_stack_high = (char*)(sp - buffer_size);

    // Put the thread metadata into TLS.
    threadscan_local_td = td;

    // Counter for getting consensus during cleanup.
    td->local_timestamp = 0;

    // Save info about this thread so that it can be signalled for cleanup.
    td->self = pthread_self();
    td->is_active = 1;

    // Call the user thread.  Exit with the return code when complete.
    pthread_exit(td->user_routine(td->user_arg));

    assert(0); // Should never get past pthread_exit().
    return 0;
}

/**
 * Do metadata cleanup for the thread before it exits.
 */
void threadscan_thread_cleanup ()
{
    thread_data_t *td = threadscan_local_td;
    assert(td);
    td->is_active = 0;
    threadscan_proc_remove_thread_data(td);
    threadscan_util_thread_data_decr_ref(td);
}

/**
 * Send the given signal to all threads in the process and return the number
 * of signals sent.
 */
int threadscan_thread_signal_all_but_me (int sig)
{
    thread_data_t *me;

    me = threadscan_local_td;
    assert(me);

    return threadscan_proc_signal_all_except(sig, me);
}

/**
 * Return the address range of the stack where the user has (or might have)
 * data.
 */
mem_range_t threadscan_thread_user_stack ()
{
    mem_range_t ret;
    thread_data_t *td = threadscan_local_td;

    ret.low = (size_t)td->user_stack_low;
    ret.high = (size_t)td->user_stack_high;

    return ret;
}

static volatile size_t global_timestamp = 1;

/**
 * Raise the "helping" flag for this thread.
 */
void threadscan_thread_cleanup_raise_flag ()
{
    assert(threadscan_local_td != NULL);
    thread_data_t *td = threadscan_local_td;
    size_t old_timestamp = td->local_timestamp;
    int updated;

    // Nothing needs to be atomic.  Only one thread ever writes to the
    // local timestamp.
    td->local_timestamp = TIMESTAMP_RAISE_FLAG(old_timestamp);
    __sync_synchronize(); // mfence.
    size_t curr = global_timestamp;
    td->local_timestamp = TIMESTAMP_RAISE_FLAG(curr);

    updated = TIMESTAMP(curr) != old_timestamp;

    // We use the times_without_update counter for distinguishing errant
    // writes from accesses to memory that we have protected for the
    // purposes of creating a snapshot.  The idea is, if we've seen the
    // same timestamp twice, during a period of inactivity, it's a bad
    // write.
    if (updated) {
        td->times_without_update = 0;
    } else if (!TIMESTAMP_IS_ACTIVE(curr)) {
        if (td->times_without_update < 2) {
            ++td->times_without_update;
        }
    }
}

/**
 * Lower the "helping" flag for this thread.
 */
void threadscan_thread_cleanup_lower_flag ()
{
    thread_data_t *td = threadscan_local_td;
    // Nothing needs to be atomic.  Only one thread ever writes to this.
    td->local_timestamp = TIMESTAMP(td->local_timestamp);
}

/**
 * Try to become the reclaimer.  Return true if successful, false otherwise.
 */
int threadscan_thread_cleanup_try_acquire ()
{
    size_t old_timestamp = global_timestamp;
    if (TIMESTAMP_IS_ACTIVE(old_timestamp)) return 0;

    size_t attempt = TIMESTAMP_SET_ACTIVE(old_timestamp + 1);
    if (!BCAS(&global_timestamp, old_timestamp, attempt)) {
        // Failed to set the value -- someone else beat us to the punch.
        return 0;
    }

    // We have the critical section and are the new cleanup thread.  Wait
    // for all threads that are trying to "help out" to acknowledge this.
    threadscan_proc_wait_for_timestamp(TIMESTAMP(attempt));
    return 1;
}

/**
 * Give up reclaimer lock.
 */
void threadscan_thread_cleanup_release ()
{
    global_timestamp = TIMESTAMP(global_timestamp);
}
