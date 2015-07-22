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
#include "child.h"
#include "env.h"
#include <fcntl.h>
#include <malloc.h>
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

#define PIPE_READ  0
#define PIPE_WRITE 1

#define SIGTHREADSCAN SIGUSR1

#define SET_LOW_BIT(p) do {                      \
        size_t v = (size_t)*(p);                 \
        if ((v & 1) == 0) { *(p) = (v + 1); }    \
    } while (0)

/****************************************************************************/
/*                           Typedefs and structs                           */
/****************************************************************************/

typedef struct threadscan_data_t threadscan_data_t;

struct threadscan_data_t {
    int max_ptrs; // Max pointer count that can be tracked during reclamation.

    // Size of the BIG buffer used to store pointers for a collection run.
    size_t working_buffer_sz;
};

/****************************************************************************/
/*                                 Globals                                  */
/****************************************************************************/

__attribute__((visibility("default")))
void threadscan_collect (void *ptr);

static threadscan_data_t g_tsdata;

// For signaling the garbage collector with work to do.
static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gc_cond = PTHREAD_COND_INITIALIZER;
static int g_gc_waiting;
static volatile int g_received_signal;
static volatile size_t g_cleanup_counter;

static gc_data_t *g_gc_data, *g_uncollected_data;

/****************************************************************************/
/*                            Pointer tracking.                             */
/****************************************************************************/

static void generate_working_pointers_list (gc_data_t *gc_data)
{
    int n = 0;
    thread_list_t *thread_list = threadscan_proc_get_thread_list();
    thread_data_t *td;

    // Add the pointers from each of the individual thread buffers.
    FOREACH_IN_THREAD_LIST(td, thread_list)
        assert(td);
        n += threadscan_queue_pop_bulk(&gc_data->addrs[n],
                                       g_tsdata.max_ptrs - n,
                                       &td->ptr_list);
    ENDFOREACH_IN_THREAD_LIST(td, thread_list);

    gc_data->n_addrs = n;
}

/****************************************************************************/
/*                             Cleanup thread.                              */
/****************************************************************************/

static void threadscan_reclaim ()
{
    char *working_memory;   // Block of memory to free.
    gc_data_t *gc_data;

    // Get memory to store the list of pointers:
    //   0 - 4095: Reserved page for the gc_data_t struct.
    //   4096 -  : Address list.
    working_memory = threadscan_alloc_mmap(g_tsdata.working_buffer_sz);
    gc_data = (gc_data_t*)working_memory;
    gc_data->addrs = (size_t*)&working_memory[PAGE_SIZE];
    gc_data->n_addrs = 0;

    // Copy the pointers into the list.
    generate_working_pointers_list(gc_data);

    // Give the list to the gc thread, signaling it if it's asleep.
    pthread_mutex_lock(&g_gc_mutex);
    gc_data->next = g_gc_data;
    g_gc_data = gc_data;
    if (g_gc_waiting != 0) {
        pthread_cond_signal(&g_gc_cond);
    }
    pthread_mutex_unlock(&g_gc_mutex);
    threadscan_thread_cleanup_release();
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
        // While this thread's local queue of pointers is full, try to initiate
        // reclamation.

        threadscan_thread_cleanup_try_acquire()
            ? threadscan_reclaim() // reclaim() will release the cleanup lock.
            : pthread_yield();
    }
}

static int read_from_child (int fd, void *buffer)
{
    int ret = read(fd, buffer, sizeof(size_t));
    if (! (ret == 0 || ret == sizeof(size_t))) {
        threadscan_fatal("Error reading from file descriptor.\n");
    }
    return ret;
}

static void garbage_collect (gc_data_t *gc_data)
{
    int pipefd[2];
    int sig_count;
    pid_t pid;

    if (0 != pipe2(pipefd, 0)) {
        threadscan_fatal("Collection failed (pipe2).\n");
    }

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    g_received_signal = 0;
    sig_count = threadscan_proc_signal(SIGTHREADSCAN);
    while (g_received_signal < sig_count) pthread_yield();
    pid = fork();

    if (pid == -1) {
        threadscan_fatal("Collection failed (fork).\n");
    } else if (pid == 0) {
        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        close(pipefd[PIPE_READ]);

        // Include the addrs from the last collection iteration.
        if (g_uncollected_data) {
            g_uncollected_data->next = gc_data;
            gc_data = g_uncollected_data;
        }
        threadscan_child(gc_data, pipefd[PIPE_WRITE]);

        close(pipefd[PIPE_WRITE]);
        exit(0);
    }

    // Parent: Listen to the child process.  It will report pointers to free
    // followed by pointers that could not be collected.
    size_t addr;
    enum { FREEING, STORING } mode = FREEING;

    ++g_cleanup_counter;
    close(pipefd[PIPE_WRITE]);

    gc_data->n_addrs = 0;
    if (g_uncollected_data) {
        threadscan_alloc_munmap(g_uncollected_data); // FIXME: munmap is slow!
    }

    while (read_from_child(pipefd[PIPE_READ], (void*)&addr)) {
        if (0 == addr) {
            // Switch from free'ing pointers to saving them for the next round.
            mode = STORING;
        } else {
            if (mode == FREEING) {
                void *p = (void*)addr;
                memset(p, 0, malloc_usable_size(p));
                free(p);
            } else gc_data->addrs[gc_data->n_addrs++] = addr;
        }
    }

    close(pipefd[PIPE_READ]);

    // Save the old addresses.
    g_uncollected_data = gc_data;
    //threadscan_diagnostic("%d remaining.\n", gc_data->n_addrs);
}

/**
 * Garbage-collector thread.
 */
void *threadscan_gc_thread (void *ignored)
{
    gc_data_t *gc_data;

    while ((1)) {
        pthread_mutex_lock(&g_gc_mutex);
        if (NULL == g_gc_data) {
            // Wait for somebody to come up with a set of addresses for us to
            // collect.
            g_gc_waiting = 1;
            pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
            g_gc_waiting = 0;
        }

        assert(g_gc_data);
        gc_data = g_gc_data;
        g_gc_data = NULL;
        pthread_mutex_unlock(&g_gc_mutex);

        garbage_collect(gc_data);
    }

    return NULL;
}

/****************************************************************************/
/*                            Bystander threads.                            */
/****************************************************************************/

/**
 * Got a signal from a thread wanting to do cleanup.
 */
static void signal_handler (int sig)
{
    size_t old_counter;
    assert(SIGTHREADSCAN == sig);

    // Acknowledge the signal and wait for the snapshot to complete.
    old_counter = g_cleanup_counter;
    __sync_fetch_and_add(&g_received_signal, 1);
    while (old_counter == g_cleanup_counter) pthread_yield();
}

/**
 * Like it sounds.
 */
__attribute__((constructor))
static void register_signal_handlers ()
{
    /* We signal threads to get them to stop while we prepare a snapshot
       on the cleanup thread. */
    if (signal(SIGTHREADSCAN, signal_handler) == SIG_ERR) {
        threadscan_fatal("threadscan: Unable to register signal handler.\n");
    }

    g_tsdata.max_ptrs = g_threadscan_ptrs_per_thread * MAX_THREAD_COUNT;

    // Calculate reserved space for stored addresses.
    g_tsdata.working_buffer_sz = g_tsdata.max_ptrs * sizeof(size_t)
        + PAGE_SIZE;
}
