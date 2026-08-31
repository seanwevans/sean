# Graph Design

## Objective
Provide a foundational, high-performance graph module in C with deterministic memory behavior, explicit ownership semantics, and no external dependencies.

## Scope
- Directed and undirected graph modes
- Weighted and unweighted edges
- Stable vertex/edge handles for long-lived systems code
- Optional fixed-capacity mode for hard real-time workloads

## Core Representation
- **Storage model:** adjacency-list-first layout
  - Vertex table stores metadata and adjacency head/tail offsets
  - Edge pool stores destination, weight (optional), flags, and next index
- **Memory model:** caller-supplied allocator hooks with a zero-allocation steady-state path
- **Handle model:** generation-tagged indices to prevent stale-handle use-after-free

## Concurrency Model
- Baseline implementation is single-writer/multi-reader via external coordination
- Optional lock-free read snapshots can be enabled with epoch-based reclamation boundaries
- Mutations are explicit API phases (`begin_update` / `commit_update`) to keep invariants simple

## API Surface (Planned)
- Lifecycle: `dv_graph_init`, `dv_graph_reset`, `dv_graph_destroy`
- Topology operations: `dv_graph_add_vertex`, `dv_graph_remove_vertex`, `dv_graph_add_edge`, `dv_graph_remove_edge`
- Traversal utilities: iterator-style adjacency walkers, BFS/DFS utility entry points
- Query helpers: degree, reachability checks, edge/vertex existence

## Correctness & Safety Invariants
- No hidden allocations in fixed-capacity mode
- Vertex and edge counts remain internally consistent after every mutation boundary
- Adjacency links are acyclic within per-vertex edge chains
- Stale handles are rejected through generation checks

## Performance Considerations
- Cache-aware contiguous pools for vertices and edges
- Branch-light adjacency iteration for hot traversal loops
- Optional prefetch hints during bulk edge ingestion
- Bulk insertion/removal APIs for graph construction phases

## Testing & Validation Strategy
- Deterministic unit tests for topology mutations and iterator semantics
- Property-based tests for invariants under randomized mutation sequences
- Sanitizer runs (ASAN/UBSAN/TSAN where applicable)
- Benchmarks for construction throughput and traversal latency on sparse and dense shapes

## Implementation Status
Delivered in `dv_graph.h` / `dv_graph.c`, with the opt-in synchronized wrapper
in `dv_graph_mt.h` / `dv_graph_mt.c`.

| Scope item | Status |
| --- | --- |
| Directed and undirected graph modes | done |
| Weighted and unweighted edges | done, weights in a parallel array |
| Stable vertex/edge handles | done, generation-tagged indices |
| Fixed-capacity mode | done, and it reaches for the allocator exactly zero times after init |
| Adjacency-list-first layout | done, one edge record threaded on both endpoints |
| Caller-supplied allocator hooks | done, with an optional resize hook |
| `begin_update` / `commit_update` phases | done, but optional rather than required |
| BFS/DFS entry points and adjacency iterators | done |
| Thread-safe wrapper | done, mutex-guarded, separate translation unit |
| Lock-free read snapshots with epoch reclamation | not started, still a later phase |
| Prefetch hints during bulk ingestion | not started |

The planned API landed as specified, plus `dv_graph_find_edge` /
`dv_graph_has_edge` / `dv_graph_remove_edge_between`, the weight accessors,
`dv_graph_add_vertices` / `dv_graph_add_edges` for bulk construction,
`dv_graph_reserve_*` and `dv_graph_shrink_to_fit`, `dv_graph_clear` alongside
`dv_graph_reset`, `dv_graph_reachable`, iterators over out-edges, in-edges,
incident edges and live vertices, and `dv_graph_is_valid` for asserting the
structural invariants in tests.

## Decisions Taken During Implementation

- **In-place initialization.** `dv_graph_init` initializes a caller-owned
  `dv_graph_t` rather than returning a heap pointer, so a graph can be embedded
  in another structure or held in an automatic variable. `dv_graph_destroy`
  releases the pools and zeroes the control block; it never frees the struct.
- **Status codes over booleans, and a stale handle is its own answer.**
  `DV_GRAPH_ERR_STALE` is distinct from `DV_GRAPH_ERR_INVALID` and from
  `DV_GRAPH_ERR_NOT_FOUND`, because "you are holding a handle to something that
  was removed" and "there is no edge between these two vertices" call for
  different responses from the caller.
- **One edge record, on two doubly linked lists.** An edge is a node on the
  source's out-list and on the destination's in-list at once, in both graph
  modes, so an undirected edge is stored once rather than as two half-edges.
  The `prev` links are what make `dv_graph_remove_edge` O(1) instead of
  O(degree), and removing a vertex O(degree) instead of O(E): each incident
  edge is unlinked from both of its lists directly. The cost is four link
  fields per edge instead of two, which keeps the record at 32 bytes.
- **Generation 0 is reserved.** A live slot always carries a non-zero
  generation, so a zero-initialized handle -- the one a caller gets from a plain
  struct declaration -- is rejected rather than addressing slot 0.
- **`clear` and `reset` are different promises.** `clear` bumps the generation
  of every live slot, so handles held across it are rejected; that is what makes
  it O(V + E) rather than a counter reset. `reset` additionally drops the
  high-water marks and returns the pools to their initial capacities, which
  restarts slot generations: handles must not cross a reset, the same way they
  must not cross a destroy.
- **Weights live in a parallel array.** A traversal that never reads a weight --
  which is most of them -- pulls only topology into cache, and an unweighted
  graph allocates no weight array at all. The weight accessors refuse an
  unweighted graph rather than inventing a value for it. The array is resized
  before the edge pool grows and after it shrinks, and carries its own capacity,
  so a refused allocation in either half leaves the array covering the pool that
  indexes it -- the other order would hand out an edge slot with no weight
  behind it.
- **The update phase is a batching scope, not a lock.** Requiring
  `begin_update` around a single `add_edge` is ceremony, so mutations are legal
  at any time. What the phase buys is the version counter: every mutation steps
  it, and a phase defers that to one step at the commit. Iterators capture the
  version and refuse to advance when it moves, which turns "iterating while
  mutating" from undefined behaviour into a walk that ends.
- **Editing a weight does not step the version.** It is not topology, so
  iterators over it stay valid.
- **The pools are a high-water mark plus a free list.** Every slot below the
  mark is either live or on the free list, which means growing a pool costs a
  resize and nothing else -- no free-list rebuild, and no initialization for
  slots that have not been handed out yet. It also gives the bulk paths an
  exact room calculation: what is left in a pool is `capacity - live count`.
- **Shrinking stops at the high-water mark.** Compacting the pools down to the
  live count would move slots and invalidate every handle, which is the one
  thing the handle model promises not to do.
- **Traversal scratch is owned by the graph and reused.** A breadth- or
  depth-first walk needs a frontier and a visited set; allocating them per
  traversal would put the allocator in the middle of the hot path. They are
  sized once and kept, and a fixed-capacity graph sizes them during `init`,
  because the whole point of fixed capacity is that nothing later allocates.
- **Depth-first carries a resumed cursor per stack level.** Pushing every
  neighbour and marking on pop would need O(E) scratch and would not produce
  depth-first order; marking on push bounds the stack at O(V) but reorders the
  walk. Keeping one edge cursor per level gives the order a recursive walk would
  produce with a stack that cannot exceed one entry per vertex.
- **Capacity is capped at 2^31 - 1.** That leaves the top bit of an index free,
  which is where the depth-first cursor carries its list selector instead of a
  parallel array of flags. At 32 bytes per record the ceiling is 64 GiB of pool.
- **Degrees follow the storage, and a self-loop counts twice.** `out_degree`
  and `in_degree` are the two lists; `degree` is their sum. On an undirected
  graph that makes a self-loop count two, the usual convention, and it is the
  same reason an incident walk yields a self-loop twice.
- **Rejecting parallel edges and self-loops is opt-in.** Rejecting a duplicate
  costs a scan of the shorter incidence list on every insertion, which a caller
  who already knows its edges are distinct should not pay.
- **Lookup by endpoints scans the shorter list.** `find_edge` picks between the
  source's out-list and the destination's in-list by recorded degree, so the
  cost is O(min(out_degree, in_degree)) rather than O(out_degree).
- **Edge flags were dropped.** Nothing in the module reads them, and a caller
  that needs per-edge tags can key them by edge index against an array of its
  own -- which costs nothing in the adjacency loop, unlike a byte inside the
  record.
- **The wrapper guards with a mutex, not a reader/writer lock.**
  `pthread_rwlock_t` is not visible to a strictly conforming translation unit
  without a feature-test macro, and a public header is the wrong place to set
  one. Concurrent readers therefore remain what this document says they are:
  the caller's own coordination. A traversal would not have been a reader in
  any case -- it reads the topology but writes the scratch the graph owns.
- **`is_valid` verifies the links, not just the counters.** It walks every
  chain against its recorded degree, which is what rules out a cycle inside a
  per-vertex edge chain, and checks every edge's four links against its
  neighbours and both list heads and tails, which catches an edge that fell off
  a list without being retired. It allocates nothing, so it is usable from
  inside an allocation-failure test.

## Test Coverage
`unit_tests.c` covers configuration rejection, empty-graph accessors, vertex
and edge lifecycle including handle recycling and stale rejection, directed and
undirected edges, self-loops under both conventions, parallel-edge rejection in
both orientations, vertex removal taking its incident edges with it, weights on
weighted and unweighted graphs, insertion-ordered iteration, iterator
invalidation and the update phase, the in/out/incident iterators, breadth-first
order and depth, depth-first order, undirected traversal across both lists,
reachability, fixed-capacity boundaries, growth against a ceiling, reserve /
shrink / clear / reset, the bulk contracts including partial batches, allocator
accounting with and without a resize hook, injected allocation failure, the
zero-allocation promise of fixed capacity, cache-line alignment, randomized
differential runs against a reference model in both graph modes, a weighted
growth whose two halves are refused in turn, and the wrapper under contention.

`dv_graph_tester.c` is the soak harness: a build and teardown at both caps
repeated over cycles, traversal over shapes whose depths and components are
known in advance, a long churn of mixed mutations checked against a running
model with per-vertex degree accounting, grow/shrink cycles with every byte
accounted for and a per-cycle baseline that a leak would raise, bulk ingestion
checked edge for edge against the one-at-a-time path, and the mutex wrapper
under producer/consumer contention.

`dv_graph_bench.c` measures edge insertion one at a time against the batch
path, adjacency scanning through the inline iterator, breadth-first against
depth-first traversal, lookup by endpoints, removal by handle against removal
by endpoints, and vertex removal at degree. Every run is checksummed before its
timing is counted, and backends that measure work over an existing graph get
one built outside the timed region.
