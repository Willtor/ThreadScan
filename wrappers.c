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

#define _GNU_SOURCE

#include "alloc.h"
#include <assert.h>
#include <dlfcn.h>
#include "env.h"
#include "proc.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include "thread.h"
#include "util.h"

/****************************************************************************/
/*                   Types of functions that get wrapped.                   */
/****************************************************************************/

typedef int (*pthread_create_t) (pthread_t *,
                                 const pthread_attr_t *,
                                 void *(*)(void *),
                                 void *);

typedef void (*pthread_exit_t) (void *);

typedef int (*pthread_join_t) (pthread_t, void **);

typedef int (*__libc_start_main_t)(int (*) (int, char **, char **),
                                   int, char **,
                                   void (*) (void),
                                   void (*) (void),
                                   void (*) (void),
                                   void (*));

typedef int (*main_t) (int, char **, char **);

/****************************************************************************/
/*                                 Globals.                                 */
/****************************************************************************/

static int g_thread_count = 1;

/****************************************************************************/
/*                            Wrapped functions.                            */
/****************************************************************************/

static pthread_create_t orig_pthread_create;
static pthread_exit_t orig_pthread_exit;
static pthread_join_t orig_pthread_join;
static __libc_start_main_t orig_libc_start_main;
static main_t orig_main;

/****************************************************************************/
/*                    Wrapping function implementations.                    */
/****************************************************************************/

__attribute__((visibility("default")))
int pthread_create (pthread_t *thread,
                    const pthread_attr_t *attr,
                    void *(*start_routine) (void *),
                    void *arg)
{
    thread_data_t *td;
    int ret;
    pthread_attr_t real_attr;
    void *stack;
    size_t stacksize;

    assert(orig_pthread_create);

    if (MAX_THREAD_COUNT < __sync_fetch_and_add(&g_thread_count, 1)) {
        // Don't overflow buffers.
        threadscan_fatal("Exceeded maximum thread count (%d).\n",
                         MAX_THREAD_COUNT);
    }

    // Wrap the user data.
    td = threadscan_util_thread_data_new();
    if (NULL == td) {
        threadscan_fatal("threadscan: Out of memory.\n");
    }
    td->user_routine = start_routine;
    td->user_arg = arg;
    td->is_active = 0;

    // If the user hasn't specified a stack, we'll use one of our own.
    // Otherwise, we get the bounds of the user's stack and use it as
    // our own.
    if (NULL == attr) {
        ret = pthread_attr_init(&real_attr);
        if (0 != ret) {
            threadscan_fatal("threadscan: could not create thread.\n");
        }
    } else {
        real_attr = *attr;
    }

    ret = pthread_attr_getstack(&real_attr, &stack, &stacksize);
    if (0 != ret) {
        threadscan_fatal("threadscan: unable to get stack attributes.\n");
    }

    if (NULL == stack) {
        stacksize = 2 * 1024 * 1024; // 2 MB.
        assert(stacksize % PAGESIZE == 0);
        stack = threadscan_alloc_mmap(stacksize);
        ret = pthread_attr_setstack(&real_attr, stack, stacksize);
        if (0 != ret) {
            threadscan_fatal("threadscan: unable to set stack attributes.\n");
        }
        td->stack_is_ours = 1;
    }

    td->user_stack_low = (char*)stack;
    td->user_stack_high = (char*)stack + stacksize;

    // Insert the metadata into the global structure.
    threadscan_proc_add_thread_data(td);

    // Try to create the thread.
    ret = orig_pthread_create(thread, &real_attr,
                              threadscan_thread_base, (void*)td);

    if (0 != ret) {
        // Ruh, roh!  Failed to create a thread.  That isn't really our
        // problem, though.  Just clean up the memory we allocated for
        // the thread.  The end.
        if (td->stack_is_ours) {
            threadscan_alloc_munmap(stack);
        }
        threadscan_util_thread_data_free(td);
    }

    return ret;
}

__attribute__((noreturn))
static void exit_wrapper (void *retval)
{
    assert(orig_pthread_exit);

    threadscan_thread_cleanup();
    __sync_fetch_and_sub(&g_thread_count, 1);
    orig_pthread_exit(retval);

    abort(); // Should never get past orig_pthread_exit();
}

__attribute__((visibility("default"), noreturn))
void pthread_exit (void *retval)
{
    exit_wrapper(retval);
}

void threadscan_pthread_exit (void *retval)
{
    exit_wrapper(retval);
}

__attribute__((visibility("default")))
int pthread_join (pthread_t thread, void **retval)
{
    assert(orig_pthread_join);
    int ret = orig_pthread_join(thread, retval);
    threadscan_util_thread_data_cleanup(thread);
    return ret;
}

typedef struct main_args_t main_args_t;

struct main_args_t {
    int argc;
    char **argv;
    char **env;
};

static void *main_thunk (void *arg)
{
    main_args_t *main_args = (main_args_t*)arg;
    exit(orig_main(main_args->argc, main_args->argv, main_args->env));
}

static int main_replacement (int argc, char **argv, char **env)
{
    thread_data_t *td = threadscan_util_thread_data_new();
    mem_range_t stack_data;
    main_args_t main_args = { argc, argv, env };
    if (NULL == td) {
        threadscan_fatal("threadscan: Out of memory.\n");
    }
    td->user_routine = main_thunk;
    td->user_arg = &main_args;
    threadscan_proc_stack_from_addr(&stack_data, (size_t)&stack_data);
    td->user_stack_low = (char*)stack_data.low;
    threadscan_thread_base(td);
    assert(0); // Should not return.  It should die in main_thunk().
    return 0;
}

__attribute__((visibility("default")))
int __libc_start_main(int (*main) (int, char **, char **),
                      int argc, char **ubp_av,
                      void (*init) (void),
                      void (*fini) (void),
                      void (*rtld_fini) (void),
                      void (*stack_end))
{
    extern void *threadscan_gc_thread (void *); // Defined in threadscan.c.
    pthread_t tid;
    int ret = orig_pthread_create(&tid, NULL, threadscan_gc_thread, NULL);
    if (0 != ret) {
        threadscan_fatal("Unable to start garbage collector.\n");
        // Does not return.
    }

    orig_main = main;
    return orig_libc_start_main(main_replacement, argc, ubp_av,
                                init, fini, rtld_fini, stack_end);
}

/****************************************************************************/
/*                           Replacement routine.                           */
/****************************************************************************/

/**
 * Find the functions that are being wrapped and keep pointers to them so
 * they can be called by their respective wrappers.  This function gets
 * called automatically as soon as the module is loaded.
 */
__attribute__((constructor))
static void do_wrapper_replacement ()
{
    orig_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    orig_pthread_exit = dlsym(RTLD_NEXT, "pthread_exit");
    orig_pthread_join = dlsym(RTLD_NEXT, "pthread_join");
    orig_libc_start_main = dlsym(RTLD_NEXT, "__libc_start_main");
}
