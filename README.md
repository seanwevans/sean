# seanlib

Foundational, high-performance, concurrent data structures written in C designed for systems that require deterministic memory management, extreme mechanical sympathy, and no external dependencies.

## Queue
The initial component of this library is a rigorously bounded, lock-free MPMC queue implemented with C11 atomics. 
To eliminate performance degradation from false sharing, both the queue structure and its individual buffer cells are strictly padded and aligned to the hardware cache line. 
Concurrency is achieved through sequence-gated state transitions rather than mutexes, allowing producers and consumers to advance independently without blocking. 
The implementation includes support for bulk enqueue and dequeue operations to maximize batch processing throughput. 
It is heavily validated through aggressive multi-threaded smoke tests, TSAN/ASAN sanitizers, and a C++ benchmark matrix that evaluates its operations-per-second directly against standard implementations like `rigtorp` and `atomic_queue`.

## Coming Soon: Graph

## Coming Soon: Heap

## Coming Soon: Stack
