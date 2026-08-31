# seanlib

Foundational, high-performance, concurrent data structures written in C designed for systems that require deterministic memory management, extreme mechanical sympathy, and no external dependencies.

## Queue
The initial component of this library is a rigorously bounded, lock-free MPMC queue implemented with C11 atomics. 
To eliminate performance degradation from false sharing, both the queue structure and its individual buffer cells are strictly padded and aligned to the hardware cache line. 
Concurrency is achieved through sequence-gated state transitions rather than mutexes, allowing producers and consumers to advance independently without blocking. 
The implementation includes support for bulk enqueue and dequeue operations to maximize batch processing throughput. 
It is heavily validated through aggressive multi-threaded smoke tests, TSAN/ASAN sanitizers, and a C++ benchmark matrix that evaluates its operations-per-second directly against standard implementations like `rigtorp` and `atomic_queue`.

## Graph
An adjacency-list graph for topology that keeps changing while it is being read.
One edge record is threaded on two doubly linked lists at once -- the source's outgoing list and the destination's incoming list -- so an undirected edge is stored once rather than as two half-edges, removing an edge is O(1) instead of a walk of the list it sits on, and removing a vertex costs one unlink per incident edge instead of a scan of the edge pool.
On this machine that shows up as removal through a handle running about three times the rate of removal by endpoints, which is the same unlink reached by a search.
Vertices and edges are generation-tagged indices into two pools, so a handle kept across the removal of what it named is rejected rather than addressing whatever was recycled into the slot.
Both pools are a high-water mark over a free list, which makes growth a resize with no free-list rebuild and gives fixed capacity a guarantee worth having: a fixed graph, its traversal scratch included, reaches for the allocator exactly zero times after `init`.
Directed and undirected modes; optional weights in an array parallel to the edge pool, so a traversal that reads none of them pulls only topology into cache; opt-in rejection of parallel edges and self-loops; breadth- and depth-first walks over scratch the graph owns and reuses; and adjacency iterators that stop at a mutation instead of walking a torn list.
Validation covers randomized differential runs against a reference model in both graph modes, a soak harness that churns mutations against a running degree model and checks traversals over shapes whose answers are known in advance, an accounting allocator that fails a run on a single unreturned byte, TSAN/UBSAN/ASAN matrices, and a benchmark that measures the design decisions -- batched edge insertion against one at a time, and removal by handle against removal by endpoints.

Design document: [`graph/DESIGN.md`](graph/DESIGN.md)

## Heap
An array-backed binary heap for priority scheduling and top-k workloads.
Push and pop are iterative sift loops that carry one element through a travelling hole, so a sift of depth `d` costs `d + 1` element moves rather than the `3d` a swap loop would; the scratch slot the carried element occupies is allocated as part of the buffer, so no operation allocates once the heap has room.
Ordering is configured rather than hard-coded: built-in key types read a scalar at a fixed offset -- optionally through the pointer stored in the slot, for heaps of pointers to objects -- and are compared inline, with a comparator callback available for orderings the built-ins cannot express.
That split exists because comparison is the hot path, and on this machine the inline key beats the equivalent callback by roughly a quarter on the same data.
Min and max order, fixed and growable capacity, Floyd's O(n) bottom-up construction, `replace_top` for bounded top-k, and `update_at` / `remove_at` for reprioritizing and cancelling work already queued.
Keyed updates need an element's current index, which the optional `on_move` hook supplies by reporting every element that comes to rest -- an index map with no allocation and no internal table.
Validation covers a randomized differential test that mirrors every push, pop, update and removal onto a reference model and re-checks the heap-order property after each one, a scheduling soak with several hundred thousand operations, an accounting allocator that fails a run on a single unreturned byte, TSAN/UBSAN/ASAN matrices, and a benchmark that measures the design decisions -- bottom-up build against repeated pushes, and the inline key against the callback.

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
