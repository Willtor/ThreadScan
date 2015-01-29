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

#include <alloca.h>
#include <pthread.h>
#include <stdio.h>

typedef enum { MAIN_CLEANUP, THREAD_CLEANUP } who_cleans_up_t;

extern void threadscan_collect (void *ptr);

void *run_me (void *arg)
{
    who_cleans_up_t who = (who_cleans_up_t)arg;
    printf("  Running (%s)\n", who == THREAD_CLEANUP ? "yes" : "no");

    if (THREAD_CLEANUP == who) {
        threadscan_collect(NULL);
    } else {
        sleep(2);
    }
    return 0;
}

void run (int thread_count, int who)
{
    int other_threads = thread_count - 1;
    pthread_t *threads = alloca(sizeof(pthread_t) * other_threads);
    int i;

    printf("Testing %d threads (who = %d).\n", thread_count, who);

    for (i = 0; i < other_threads; ++i) {
        pthread_create(&threads[i], NULL, run_me,
                       who == i + 1 ?
                       (void*)THREAD_CLEANUP :
                       (void*)MAIN_CLEANUP);
    }

    if (who == 0) {
        // I do the cleanup.
        threadscan_collect(NULL);
    }

    for (i = 0; i < other_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    printf("Completed test.\n");
}

int main ()
{
    run(2, 0);
    run(2, 1);
    run(4, 0);
    run(4, 1);
    run(4, 2);
    run(4, 3);
    run(8, 0);
    run(8, 1);
    run(8, 7);

    return 0;
}
