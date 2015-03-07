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

/* Module Description:
   Allocate/deallocate and track memory used by threadscan in a centralized
   location.
 */

#ifndef _ALLOC_H_
#define _ALLOC_H_

#include <stddef.h>
#include "util.h"

/**
 * mmap() for the threadscan system.  This call never fails.  But you should
 * only ever ask for big chunks in multiples of the page size.
 * @return The allocated memory.
 */
void *threadscan_alloc_mmap (size_t size);

/**
 * munmap() for the threadscan system.
 */
void threadscan_alloc_munmap (void *ptr);

#endif // !defined _ALLOC_H_
