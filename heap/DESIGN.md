# Heap Design

## Objective
Deliver a high-performance heap module in C for predictable priority scheduling and top-k workflows with strict control over memory and latency.

## Scope
- Min-heap and max-heap modes
- Optional custom comparator callback
- Fixed-capacity and dynamically growable configurations
- Support for keyed updates (`decrease_key` / `increase_key`) through optional index maps

## Core Representation
- **Storage model:** contiguous array-backed binary heap
- **Indexing:** 0-based with `parent=(i-1)/2`, `left=2i+1`, `right=2i+2`
- **Payload model:** opaque item bytes or pointer payload mode selected at initialization
- **Memory model:** caller-provided allocator hooks; no allocation during push/pop in fixed mode

## Concurrency Model
- Non-thread-safe by default for maximal performance
- Thin synchronized wrapper planned for external locking scenarios
- Bulk-build path (`heapify`) intended for single-threaded setup phases

## API Surface (Planned)
- Lifecycle: `dv_heap_init`, `dv_heap_reset`, `dv_heap_destroy`
- Core operations: `dv_heap_push`, `dv_heap_pop`, `dv_heap_peek`
- Maintenance: `dv_heap_heapify`, `dv_heap_reserve`, `dv_heap_shrink_to_fit`
- Priority updates: `dv_heap_update_at`, optional key-index API

## Correctness & Safety Invariants
- Heap-order property is preserved after every mutating operation
- Capacity and size counters never diverge
- Comparator contract violations are surfaced via debug checks
- Update-at operations re-sift in the correct direction exactly once

## Performance Considerations
- Iterative sift-up/sift-down to avoid recursion overhead
- Branch-minimized compare/swap loops in hot paths
- Bulk heap construction via Floyd heapify for O(n) build
- Optional inline-friendly static configuration for small fixed heaps

## Testing & Validation Strategy
- Unit tests for push/pop ordering, duplicates, and edge capacities
- Randomized differential tests against a reference implementation
- Sanitizer instrumentation for bounds and lifetime checks
- Microbenchmarks for push/pop throughput and heapify performance
