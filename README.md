# The ThreadScan Memory Reclamation System

## Introduction

ThreadScan is a library for performing automated memory reclamation on concurrent data structures in C and C++.

When one thread removes a node from a data structure, it isn't safe to call ***free*** if another thread may be accessing that node at the same time.  In place of ***free***, ThreadScan provides the ***threadscan_collect*** function which reclaims the memory on the node when no remaining threads hold references to it.

## Compilation

At this time, the ThreadScan is only supported on Linux.  Use ***make*** to build the library.

```
% make
```

The library will appear as ***libthreadscan.so*** in the same directory as the source code.  If you want to install it on your system, use:

```
% sudo make install
```

The library will be installed at ***/usr/local/lib/libthreadscan.so*** and the header file will be installed at ***/usr/local/include/threadscan.h***.

## Usage

ThreadScan can be used in another code base by calling the collection function from that code, and building the other package with the library on the command line.  To access the library routines from your code, include the threadscan header.

```
#include <threadscan.h>
```

The main collection routine is:

```
void threadscan_collect (void *);
```

***threadscan_collect*** can be used to automate memory reclamation on any pointer for which 1. the only remaining references exist on thread stacks or in active registers, and 2. ***threadscan_collect*** is not called multiple times on the same node.

For example, a lock-free linked list swings the previous node's next pointer, and the node is no longer reachable from the root.  Provided the pointer to the node has not been stored somewhere else, the node now meets the first criterion.  If the call to ***threadscan_collect*** only occurs after the CAS that removes the node, the second criterion is met, too.

To include the library in your build, install it as above and add the library to the link line given to GCC.

```
-lthreadscan
```

ThreadScan may also be used in semi-automated mode.  If a thread uses a buffer that is not on the stack, but is still functionally local to that one thread, ThreadScan can be configured to search that space, too.

```
void threadscan_register_local_block (void *addr, size_t size);
```

Call this function with a pointer to the buffer and its size when the thread starts.  The identified region will be scanned along with the stack when reclamation occurs.

## Recommendations

+ Use TC-Malloc or Hoard, which are known to be fast allocators in multi-threaded code.  TC-Malloc is available at https://code.google.com/p/gperftools/
+ If you don't install ***libthreadscan.so***, or if you install it somewhere that requires you to use LD_PRELOAD, specify TC-Malloc, first, so that ThreadScan will find its ***free*** before that of the default Linux malloc (PT-Malloc).  Mixing ***malloc*** and ***free*** calls from different libraries can cause the program to crash.

## Bugs/Questions/Contributions

You can contact the maintainer, William M. Leiserson, at willtor@mit.edu.

We appreciate contributions of bug fixes, features, etc.  If you would like to contribute, please read the MIT License (LICENSE) carefully to be sure you agree to the terms under which this library is released.  If your name doesn't appear in the AUTHORS file, you can append your name to the list of authors along with your changes.
