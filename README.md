# seanlib

Foundational, high-performance, concurrent data structures written in C designed for systems that require deterministic memory management, extreme mechanical sympathy, and no external dependencies.

## Queue
The initial component of this library is a rigorously bounded, lock-free MPMC queue implemented with C11 atomics. 
To eliminate performance degradation from false sharing, both the queue structure and its individual buffer cells are strictly padded and aligned to the hardware cache line. 
Concurrency is achieved through sequence-gated state transitions rather than mutexes, allowing producers and consumers to advance independently without blocking. 
The implementation includes support for bulk enqueue and dequeue operations to maximize batch processing throughput. 
It is heavily validated through aggressive multi-threaded smoke tests, TSAN/ASAN sanitizers, and a C++ benchmark matrix that evaluates its operations-per-second directly against standard implementations like `rigtorp` and `atomic_queue`.

## Graph
Design document: [`graph/DESIGN.md`](graph/DESIGN.md)

## Heap
Design document: [`heap/DESIGN.md`](heap/DESIGN.md)

## Stack
A contiguous LIFO stack for workloads that want the allocator out of the hot path.
Elements live in one contiguous buffer behind a single `top` index, so push and pop are branch-light O(1) writes to the hot end of the array and never touch the allocator while the stack has room.
Two element models share one representation: a generic byte-stride mode for fixed-size elements, and a pointer specialization that writes `void *` slots directly with no `memcpy` on the hot path.
Capacity is either strictly fixed or growable to an optional ceiling, and every allocation, resize and release goes through caller-supplied hooks that receive the exact byte counts and alignment involved, so arena and slab allocators need no bookkeeping of their own.
Failures are explicit status codes rather than aborts: full, empty, out-of-memory and non-representable byte sizes are all distinguishable and leave the stack unchanged.
The core is deliberately unsynchronized; an opt-in mutex wrapper lives in its own translation unit so the single-threaded path never links a lock.
Validation covers a differential test against a reference array over millions of randomized operations, an accounting allocator that fails a run on a single unreturned byte, injected allocation failures, TSAN/UBSAN/ASAN matrices, and a benchmark harness for push/pop, bulk and growth throughput.

Design document: [`stack/DESIGN.md`](stack/DESIGN.md)
