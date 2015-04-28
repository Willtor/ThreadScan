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

#ifndef _THREADSCAN_H_
#define _THREADSCAN_H_

/**
 * Submit a pointer for memory reclamation.  threadscan_collect() will call
 * free() on the pointer, itself, when there are no more outstanding
 * references on the stack or in blocks of memory specified using
 * threadscan_register_local_block().
 */
extern void threadscan_collect (void *ptr);

/**
 * Specify a block of memory, local to the thread that called the function,
 * that ThreadScan will search during the reclamation phase.  Without this
 * call ThreadScan will search only the thread stacks.
 */
extern void threadscan_register_local_block (void *addr, size_t size);

#endif // !defined _THREADSCAN_H_
