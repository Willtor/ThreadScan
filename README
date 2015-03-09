# The ThreadScan Memory Reclamation System

## Introduction

ThreadScan is a library for performing automated memory reclamation on concurrent data structures in C and C++.

When one thread removes a node from a data structure, it isn't safe to call ***free*** if another thread may be accessing that node at the same time.  In place of ***free***, ThreadScan provides the ***threadscan_collect*** function which reclaims the memory on the node when no remaining threads hold references to it.

## Compilation

At this time, the ThreadScan is only supported on Linux.  Use ***make*** to build the library.

```
% make
```

The library will appear as ***libthreadscan.so*** in the same directory as the source code.

## Usage

ThreadScan can be used in another code base by calling the collection function from that code, and building the other package with the library on the command line.  To use the collection function in a file, include the line:

```
extern void threadscan_collect (void *);
```

***threadscan_collect*** can be used on any pointer for which 1. the only remaining references exist on thread stacks or in active registers, and 2. ***threadscan_collect*** is not called multiple times on the same node.

For example, a lock-free linked list swings the previous node's next pointer, and the node is no longer reachable from the root.  Provided the pointer to the node has not been stored somewhere else, the node now meets the first criterion.  If the call to ***threadscan_collect*** only occurs after the CAS that removes the node, the second criterion is met, too.

To include the library in your build, place the library in some directory, /example/path, and add the argument to GCC:

```
-L/example/path -lthreadscan
```

## Recommendations

+ Use TC-Malloc or Hoard, which are known to be fast allocators in multi-threaded code.  TC-Malloc is available at https://code.google.com/p/gperftools/
+ If you install libthreadscan.so somewhere that requires you to use LD_PRELOAD, specify TC-Malloc, first, so that ThreadScan will find its ***free*** before that of the default Linux malloc (PT-Malloc).  Mixing ***malloc*** and ***free*** calls from different libraries can cause the program to crash.

## Bugs/Questions

You can contact the maintainer, William M. Leiserson, at willtor@mit.edu.
