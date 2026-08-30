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

## Implementation Status
Delivered in `dv_heap.h` / `dv_heap.c`, with the opt-in synchronized wrapper in
`dv_heap_mt.h` / `dv_heap_mt.c`.

| Scope item | Status |
| --- | --- |
| Min-heap and max-heap modes | done |
| Optional custom comparator callback | done, alongside seven built-in key types |
| Fixed-capacity and growable configurations | done, with an optional growth ceiling |
| Keyed updates through index maps | done, via the `on_move` hook |
| Thin synchronized wrapper | done, mutex-guarded, separate translation unit |

The planned API landed as specified, plus `dv_heap_build` for bulk loading,
`dv_heap_replace_top` for bounded top-k, `dv_heap_remove_at` for cancellation,
`dv_heap_push_bulk` / `dv_heap_pop_bulk`, and `dv_heap_is_valid` for asserting
the heap-order property in tests.

## Decisions Taken During Implementation

- **The comparison is the hot path, so ordering is configured, not called.** A
  binary heap performs O(log n) comparisons per push and twice that per pop; an
  indirect call in that loop dominates everything else. Built-in key types
  (`u64`, `i64`, `u32`, `i32`, `f64`, `f32`, `ptr`) load a scalar at a fixed
  offset and compare it inline, and `DV_HEAP_KEY_CUSTOM` falls back to a
  callback for orderings they cannot express. The benchmark measures the gap
  between the two on identical data.
- **Keys can be read through the slot.** With `key_indirect`, the slot holds a
  pointer and the offset applies inside the pointed-to object, which is the
  common scheduling shape -- a heap of pointers to work items ordered by a
  field of the item -- without a callback. Key offsets are bounds-checked
  against `elem_size` at init in the direct case; the indirect case cannot be
  checked and says so.
- **Sifts move a hole, not a swap.** Both loops carry one element and shift
  others into the hole, costing `d + 1` moves for a sift of depth `d` instead
  of `3d`.
- **The scratch slot lives in the buffer.** The allocation always carries one
  slot beyond the capacity to hold the carried element, so pop and remove need
  no scratch allocation, no second buffer, and no per-operation allocator
  traffic. It also means the accounting in the tests expects
  `(capacity + 1) * elem_size` bytes, not `capacity * elem_size`.
- **The capacity ceiling keeps child indices inside `size_t`.** Capacity is
  refused above `(SIZE_MAX - 2) / 2`, so `2i + 2` cannot overflow for any
  reachable index and the sift loops need no per-step overflow check.
- **The index map is a hook, not a table.** `on_move` reports every element
  that comes to rest at a new index, letting an owner record it inside its own
  object. No allocation, no hash map, and no cost beyond one well-predicted
  branch when the hook is unset. `heapify` suppresses per-move reporting for
  the duration of the bottom-up pass and issues one sweep at the end, which
  leaves the map consistent with far fewer calls.
- **`update_at` re-sifts exactly once.** One comparison against the parent
  decides the direction, so `decrease_key` and `increase_key` are the same
  entry point and neither pays for the other.
- **Comparator contract violations are checked in debug builds.** Every
  comparison asks the reverse question as well and asserts that both are not
  true at once, which catches a comparator written as `a <= b` -- a mistake
  that otherwise produces a silently wrong heap rather than a crash. The check
  doubles comparisons, so it compiles out with `NDEBUG`. Every debug-build test
  run exercises it.
- **NaN keys are unordered.** The floating-point key types compare with `<`,
  under which NaN is neither less nor greater. A heap containing NaN keys makes
  no ordering promise about them; the debug check does not fire, because
  answering "no" both ways is consistent, not contradictory.

## Test Coverage
`unit_tests.c` covers configuration rejection, empty and boundary states, min
and max order, duplicate keys, all seven built-in key types including the
negative half of the signed ones, keys at an offset inside a struct, pointer
payloads with indirect keys, custom comparators under both orders, capacity
management, `build` and `heapify`, `replace_top` for top-k, `update_at` in both
directions, `remove_at` at the root and at interior and last positions, the
bulk contracts, allocator accounting with and without a resize hook, injected
allocation failure, byte-size overflow, cache-line alignment, a randomized
differential test against a reference model, and the wrapper under contention.

`dv_heap_tester.c` is the soak harness: fill and drain at capacity, bottom-up
build checked against repeated pushes for identical output, a full scheduling
mix of submit/dispatch/reprioritize/cancel verified against a linear scan of
the live set, bounded top-k checked against an independent selection,
grow/shrink under live elements with byte accounting, and the mutex wrapper
under producer/consumer contention.

`dv_heap_bench.c` measures push/pop, construction one element at a time against
Floyd's bottom-up build, the built-in key against an equivalent callback,
bounded top-k via `replace_top`, and a stream of keyed updates against a live
index map. Every run is checksummed before its timing is counted.
