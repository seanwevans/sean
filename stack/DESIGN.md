# Stack Design

## Objective
Provide a minimal-overhead stack module in C for LIFO workloads with deterministic performance, explicit memory control, and strong debug-time validation.

## Scope
- Generic element stack (fixed element size)
- Pointer stack specialization for lightweight object references
- Fixed-capacity and growable modes
- Optional bounded lock-free variant for MPMC/MPSC scenarios in a later phase

## Core Representation
- **Storage model:** contiguous buffer with a top index
- **Element model:** byte-stride writes for generic mode; direct pointer writes for pointer mode
- **Memory model:** caller-defined allocation/reallocation hooks
- **Bounds model:** strict cap checks with non-blocking failure return codes

## Concurrency Model
- Baseline stack is single-threaded and unsynchronized
- Thread-safe wrappers are explicit opt-in and separate from core hot path
- Lock-free variant, if added, will use tagged indices to mitigate ABA risks

## API Surface (Planned)
- Lifecycle: `dv_stack_init`, `dv_stack_reset`, `dv_stack_destroy`
- Core operations: `dv_stack_push`, `dv_stack_pop`, `dv_stack_peek`
- Capacity management: `dv_stack_reserve`, `dv_stack_capacity`, `dv_stack_size`
- Utility: `dv_stack_clear`, bulk push/pop helpers

## Correctness & Safety Invariants
- `top` always points one past the last valid element
- Pop/peek on empty stack return explicit error status
- No out-of-bounds writes on push under fixed-capacity constraints
- Reset preserves allocation ownership contracts

## Performance Considerations
- O(1) push/pop with contiguous memory locality
- Fast-path inline checks for empty/full conditions
- Bulk operations for amortized reduced branch overhead
- Optional cache-line alignment for hot stacks in tight loops

## Testing & Validation Strategy
- Unit tests for LIFO ordering, boundary transitions, and reset semantics
- Stress tests for large push/pop cycles and grow/shrink behavior
- Sanitizer-backed validation for memory safety
- Benchmarks for single-thread push/pop throughput and bulk operation efficiency

## Implementation Status
Delivered in `dv_stack.h` / `dv_stack.c`, with the opt-in synchronized wrapper in
`dv_stack_mt.h` / `dv_stack_mt.c`.

| Scope item | Status |
| --- | --- |
| Generic element stack (fixed element size) | done |
| Pointer stack specialization | done, as a mode of the same type |
| Fixed-capacity and growable modes | done, with an optional growth ceiling |
| Thread-safe wrapper | done, mutex-guarded, separate translation unit |
| Bounded lock-free variant | not started, still a later phase |

The planned API landed as specified, plus `dv_stack_shrink_to_fit`,
`dv_stack_top` / `dv_stack_at` for zero-copy access, `dv_stack_is_empty` /
`dv_stack_is_full`, and `dv_stack_status_str` for diagnostics.

## Decisions Taken During Implementation

- **In-place initialization.** `dv_stack_init` initializes a caller-owned
  `dv_stack_t` rather than returning a heap pointer, so a stack can be embedded
  in another structure or held in an automatic variable. `dv_stack_destroy`
  releases the buffer and zeroes the control block; it never frees the struct.
- **Status codes over booleans.** Push failures separate `DV_STACK_ERR_FULL`
  from `DV_STACK_ERR_NOMEM` and `DV_STACK_ERR_OVERFLOW`, because a bounded
  stack hitting its cap and an allocator refusing memory call for different
  responses from the caller.
- **`clear` / `reset` / `destroy` are three distinct contracts.** `clear` drops
  the elements and keeps the buffer; `reset` additionally returns growth beyond
  the configured initial capacity to the owning allocator; `destroy` returns
  everything. Only `clear` is guaranteed allocator-free.
- **Bulk pop is successive pops.** `dv_stack_pop_bulk` writes the former top to
  `out[0]`, so a bulk pop is observationally identical to that many single
  pops. The alternative — copying the top run in ascending memory order — is
  faster by one reversed loop but silently reverses LIFO order.
- **Bulk push sizes the buffer once.** A growable bulk push reserves for the
  whole request before copying, falling back to a partial push only when the
  allocation is refused, so a batch costs one resize rather than one per
  element.
- **Growth is 1.5x off a floor of `DV_STACK_MIN_CAPACITY`.** Saturating rather
  than wrapping at the top of the range, and clamped to `max_capacity` when one
  is configured. 1.5x keeps previously freed blocks reusable by the allocator in
  a way doubling does not.
- **Stride must be a multiple of element alignment.** `sizeof` is always a
  multiple of `alignof`, so this rejects only hand-rolled configurations — ones
  where every element after the first would be misaligned. It is checked at
  init and refused with `DV_STACK_ERR_INVALID`.
- **Allocator hooks carry sizes and alignment.** `alloc`, `realloc` and `free`
  all receive the exact byte count and the required alignment, so arena and
  slab allocators need no side table. `realloc` is optional; when absent the
  stack copies only the live prefix into a fresh block.
- **No interior pointers escape the mutex wrapper.** `dv_stack_top` and
  `dv_stack_at` return addresses valid only until the next mutating call, which
  cannot be honored once a lock is released, so the wrapper exposes copying
  accessors and `dv_stack_mt_with` for compound critical sections.

## Test Coverage
`unit_tests.c` covers configuration rejection, boundary transitions, ordering,
capacity management, the bulk contracts, the pointer specialization, allocator
accounting with and without a resize hook, injected allocation failure,
byte-size overflow, cache-line alignment, a randomized differential test against
a reference array, and the wrapper under contention.

`dv_stack_tester.c` is the soak harness: fixed fill/drain cycles, grow/shrink
under live elements with full byte accounting, bulk round-trips at scale,
pointer ownership over heap objects (ASAN turns any mistake into a failure), a
long randomized differential run, and the mutex wrapper under producer/consumer
contention.

`dv_stack_bench.c` measures single-threaded push/pop throughput for narrow and
cache-line-sized elements, the pointer specialization, the bulk path, amortized
growth from empty, and an unpredictable push/pop mix. Every run is checksummed
before its timing is counted.
