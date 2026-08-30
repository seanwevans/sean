// dv_graph.h - adjacency-list graph with stable handles and explicit memory
// control.
//
// Vertices and edges live in two contiguous pools. A vertex record holds the
// head and tail of its outgoing and incoming edge lists; an edge record is a
// node on both of them at once -- on the source's out-list and on the
// destination's in-list -- so one record represents one edge no matter how
// many lists it belongs to. Both lists are doubly linked, which is what makes
// removing an edge, or a vertex with all its edges, O(1) per edge rather than
// O(degree) per unlink.
//
// Handles are generation-tagged indices. Removing a vertex or an edge bumps
// the generation of its slot, so a handle kept across the removal is rejected
// with DV_GRAPH_ERR_STALE instead of silently addressing whatever was
// recycled into the slot. A zeroed handle is never valid: a live slot always
// carries a non-zero generation.
//
// Weights live in a separate array parallel to the edge pool, so a traversal
// that never reads a weight -- which is most of them -- pulls only topology
// into cache, and an unweighted graph allocates no weights at all.
//
// The core is single-threaded and unsynchronized by design. A reader/writer
// wrapper lives in dv_graph_mt.h and is deliberately kept out of this header
// so the hot path carries no synchronization cost.

#ifndef DV_GRAPH_H
#define DV_GRAPH_H

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef DV_CACHE_LINE_SIZE
#if defined(__aarch64__)
#define DV_CACHE_LINE_SIZE 128u
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
#define DV_CACHE_LINE_SIZE 64u
#else
#define DV_CACHE_LINE_SIZE 32u
#endif
#endif

// Smallest capacity a growable pool jumps to on its first growth. Growth is
// 1.5x thereafter, which keeps freed blocks reusable by the allocator.
#ifndef DV_GRAPH_MIN_CAPACITY
#define DV_GRAPH_MIN_CAPACITY 8u
#endif

// Terminates every intrusive list and marks an empty free list. It is also the
// one index value a pool can never hold, which is why capacity is refused at
// UINT32_MAX.
#define DV_GRAPH_INVALID_INDEX 0xFFFFFFFFu

// Capacity ceiling for both pools. The top bit of an index is free below it,
// which is what lets a depth-first walk carry its list selector in the top bit
// of an edge cursor instead of a parallel array of flags. At 32 bytes per
// record the ceiling is 64 GiB of pool, so nothing gives it up in practice.
#define DV_GRAPH_MAX_CAPACITY 0x7FFFFFFFu

// Debug-time contract checks. Compiled out by NDEBUG like plain assert, and
// suppressible independently for callers that want checked release builds of
// everything else.
#ifdef DV_GRAPH_NO_ASSERT
#define DV_GRAPH_ASSERT(expr) ((void)0)
#else
#define DV_GRAPH_ASSERT(expr) assert(expr)
#endif

typedef enum {
  DV_GRAPH_OK = 0,
  DV_GRAPH_ERR_INVALID,   // NULL or contract-violating arguments
  DV_GRAPH_ERR_STALE,     // handle names a slot that was recycled or freed
  DV_GRAPH_ERR_NOT_FOUND, // no such edge between the given vertices
  DV_GRAPH_ERR_EXISTS,    // duplicate edge on a graph that forbids them
  DV_GRAPH_ERR_FULL,      // past a fixed or maximum pool capacity
  DV_GRAPH_ERR_NOMEM,     // allocator hook returned NULL
  DV_GRAPH_ERR_OVERFLOW,  // byte size of the request is not representable
} dv_graph_status_t;

// --- Handles -----------------------------------------------------------
//
// `index` addresses the pool slot; `generation` is the tag the slot carried
// when the handle was issued. Generation 0 is reserved for "no handle", so a
// zero-initialized handle is always rejected.

typedef struct {
  uint32_t index;
  uint32_t generation;
} dv_vertex_t;

typedef struct {
  uint32_t index;
  uint32_t generation;
} dv_edge_t;

static inline dv_vertex_t dv_vertex_none(void) {
  const dv_vertex_t v = {DV_GRAPH_INVALID_INDEX, 0u};
  return v;
}

static inline dv_edge_t dv_edge_none(void) {
  const dv_edge_t e = {DV_GRAPH_INVALID_INDEX, 0u};
  return e;
}

static inline bool dv_vertex_is_none(dv_vertex_t v) {
  return v.generation == 0u;
}

static inline bool dv_edge_is_none(dv_edge_t e) { return e.generation == 0u; }

static inline bool dv_vertex_eq(dv_vertex_t a, dv_vertex_t b) {
  return a.index == b.index && a.generation == b.generation;
}

static inline bool dv_edge_eq(dv_edge_t a, dv_edge_t b) {
  return a.index == b.index && a.generation == b.generation;
}

// --- Allocator ---------------------------------------------------------
//
// See dv_stack.h for the same contract: alloc and free are mandatory, realloc
// is optional, and every hook receives the exact byte count and the required
// alignment so arena and slab allocators need no side table.
typedef struct {
  void *(*alloc)(size_t size, size_t align, void *user_data);
  void *(*realloc)(void *ptr, size_t old_size, size_t new_size, size_t align,
                   void *user_data);
  void (*free)(void *ptr, size_t size, size_t align, void *user_data);
  void *user_data;
} dv_graph_allocator_t;

// --- Configuration -----------------------------------------------------

typedef struct {
  bool directed;  // false stores each edge once and walks both endpoints
  bool weighted;  // false allocates no weight array at all
  bool growable;  // false pins both pools at their initial capacities
  bool cache_aligned; // align the pools to DV_CACHE_LINE_SIZE

  // Rejecting a duplicate costs a scan of the shorter of the two incidence
  // lists on every insertion, so it is opt-in: leave false when the caller
  // already knows its edges are distinct.
  bool reject_parallel_edges;
  bool reject_self_loops;

  uint32_t initial_vertex_capacity;
  uint32_t initial_edge_capacity;
  uint32_t max_vertex_capacity; // growth ceiling; 0 means unbounded
  uint32_t max_edge_capacity;
} dv_graph_config_t;

// --- Pool records ------------------------------------------------------
//
// Exposed so adjacency iteration can be inlined. The layout is internal and
// may change; walk it through the iterator rather than by hand.

typedef struct {
  uint32_t out_head; // doubles as the free-list link while inactive
  uint32_t out_tail;
  uint32_t in_head;
  uint32_t in_tail;
  uint32_t out_degree;
  uint32_t in_degree;
  uint32_t generation;
  bool active;
} dv_graph_vertex_rec_t;

typedef struct {
  uint32_t src;
  uint32_t dst;
  uint32_t next_out; // doubles as the free-list link while inactive
  uint32_t prev_out;
  uint32_t next_in;
  uint32_t prev_in;
  uint32_t generation;
  bool active;
} dv_graph_edge_rec_t;

// Field order is deliberate: the pools, the weights and the version counter
// are what adjacency iteration reads on every step, and they share the first
// cache line.
typedef struct {
  dv_graph_vertex_rec_t *vertices;
  dv_graph_edge_rec_t *edges;
  double *weights; // NULL on an unweighted graph
  uint64_t version;

  uint32_t vertex_capacity;
  uint32_t vertex_count;
  uint32_t vertex_free;      // head of the recycled-slot list
  uint32_t vertex_watermark; // slots below this have been used at least once

  uint32_t edge_capacity;
  uint32_t weight_capacity; // slots the weights array holds; never below edge_capacity
  uint32_t edge_count;
  uint32_t edge_free;
  uint32_t edge_watermark;

  // Traversal scratch, owned by the graph and reused: `work` holds a frontier
  // of vertex indices and, for depth-first walks, a parallel edge cursor.
  uint32_t *work;
  uint64_t *visited;
  uint32_t work_capacity; // vertices the scratch is sized for
  size_t visited_words;

  size_t align;
  uint32_t initial_vertex_capacity;
  uint32_t initial_edge_capacity;
  uint32_t max_vertex_capacity;
  uint32_t max_edge_capacity;
  dv_graph_allocator_t allocator;

  bool directed;
  bool weighted;
  bool growable;
  bool cache_aligned;
  bool reject_parallel_edges;
  bool reject_self_loops;
  bool in_update;
  bool update_dirty; // a mutation happened inside the open update phase
} dv_graph_t;

// --- Lifecycle ---------------------------------------------------------

// Initializes `g` in place. On failure `*g` is left zeroed, which is a valid
// argument to dv_graph_destroy. `allocator` may be NULL to use malloc-backed
// default hooks; when supplied, its alloc and free members must be non-NULL.
//
// A fixed-capacity graph also reserves its traversal scratch here, because the
// point of fixed capacity is that nothing later reaches for the allocator.
dv_graph_status_t dv_graph_init(dv_graph_t *g, const dv_graph_config_t *config,
                                const dv_graph_allocator_t *allocator);

// Drops every vertex and edge and keeps the pools. Handles issued before the
// call are rejected afterwards: every live slot's generation is bumped.
void dv_graph_clear(dv_graph_t *g);

// Clears, then returns both pools to their configured initial capacities. The
// graph stays usable with the same config, and is cleared even if the resize
// itself fails.
dv_graph_status_t dv_graph_reset(dv_graph_t *g);

// Releases the pools through the owning allocator and zeroes the control
// block. Idempotent, and safe on a zeroed or already-destroyed graph.
void dv_graph_destroy(dv_graph_t *g);

// --- Update phase ------------------------------------------------------
//
// Optional batching scope. Every topology mutation bumps `version`, which is
// what invalidates iterators; inside a phase the bump is deferred to the
// commit, so a bulk edit costs one step instead of one per mutation. Mutating
// outside a phase is allowed and is the common case -- an explicit phase for a
// single edge insertion would be ceremony.

dv_graph_status_t dv_graph_begin_update(dv_graph_t *g);
dv_graph_status_t dv_graph_commit_update(dv_graph_t *g);

// --- Vertices ----------------------------------------------------------

// `out` may be NULL when the handle is not needed.
dv_graph_status_t dv_graph_add_vertex(dv_graph_t *g, dv_vertex_t *out);

// Removes the vertex and every edge incident to it, in O(degree).
dv_graph_status_t dv_graph_remove_vertex(dv_graph_t *g, dv_vertex_t vertex);

bool dv_graph_vertex_valid(const dv_graph_t *g, dv_vertex_t vertex);

// Handle for a live slot index, or the none handle. Useful for recovering a
// handle from an index stored in a caller-side array.
dv_vertex_t dv_graph_vertex_at(const dv_graph_t *g, uint32_t index);

// --- Edges -------------------------------------------------------------

// Appends to the tail of both incidence lists, so iteration yields edges in
// insertion order. `weight` is ignored on an unweighted graph. `out` may be
// NULL.
dv_graph_status_t dv_graph_add_edge(dv_graph_t *g, dv_vertex_t src,
                                    dv_vertex_t dst, double weight,
                                    dv_edge_t *out);

dv_graph_status_t dv_graph_remove_edge(dv_graph_t *g, dv_edge_t edge);

// Removes one edge between the two vertices, the first in the source's
// out-list. On an undirected graph the pair is unordered.
dv_graph_status_t dv_graph_remove_edge_between(dv_graph_t *g, dv_vertex_t src,
                                               dv_vertex_t dst);

bool dv_graph_edge_valid(const dv_graph_t *g, dv_edge_t edge);
dv_edge_t dv_graph_edge_at(const dv_graph_t *g, uint32_t index);

// Scans the shorter of the two incidence lists, so the cost is
// O(min(out_degree(src), in_degree(dst))). `out` may be NULL.
dv_graph_status_t dv_graph_find_edge(const dv_graph_t *g, dv_vertex_t src,
                                     dv_vertex_t dst, dv_edge_t *out);

bool dv_graph_has_edge(const dv_graph_t *g, dv_vertex_t src, dv_vertex_t dst);

dv_graph_status_t dv_graph_edge_endpoints(const dv_graph_t *g, dv_edge_t edge,
                                          dv_vertex_t *src, dv_vertex_t *dst);

// Both refuse an unweighted graph with DV_GRAPH_ERR_INVALID rather than
// inventing a weight it does not store.
dv_graph_status_t dv_graph_edge_weight(const dv_graph_t *g, dv_edge_t edge,
                                       double *out);
dv_graph_status_t dv_graph_set_edge_weight(dv_graph_t *g, dv_edge_t edge,
                                           double weight);

// --- Degrees -----------------------------------------------------------
//
// An edge is stored once, on the out-list of `src` and the in-list of `dst`,
// in both graph modes. On an undirected graph that split is an implementation
// detail and the incident degree is the sum, under which a self-loop counts
// twice -- the usual convention.

uint32_t dv_graph_out_degree(const dv_graph_t *g, dv_vertex_t vertex);
uint32_t dv_graph_in_degree(const dv_graph_t *g, dv_vertex_t vertex);
uint32_t dv_graph_degree(const dv_graph_t *g, dv_vertex_t vertex);

// --- Capacity management -----------------------------------------------

dv_graph_status_t dv_graph_reserve_vertices(dv_graph_t *g, uint32_t capacity);
dv_graph_status_t dv_graph_reserve_edges(dv_graph_t *g, uint32_t capacity);

// Sizes the traversal scratch for the current vertex capacity, so a later BFS
// or DFS on a growable graph runs without touching the allocator.
dv_graph_status_t dv_graph_reserve_traversal(dv_graph_t *g);

// Releases pool capacity above the live high-water mark, and the traversal
// scratch with it. A fixed graph has nothing to give back and succeeds without
// change.
dv_graph_status_t dv_graph_shrink_to_fit(dv_graph_t *g);

// --- Bulk operations ---------------------------------------------------

typedef struct {
  dv_vertex_t src;
  dv_vertex_t dst;
  double weight;
} dv_graph_edge_spec_t;

// Both reserve for the whole request once and then insert, so a batch costs
// one resize rather than one per element, and both return the number added --
// short of `count` only when a pool cannot grow far enough or a spec is
// rejected. `out` may be NULL. The version bumps once for the batch.
uint32_t dv_graph_add_vertices(dv_graph_t *g, uint32_t count,
                               dv_vertex_t *out);
uint32_t dv_graph_add_edges(dv_graph_t *g, const dv_graph_edge_spec_t *specs,
                            uint32_t count, dv_edge_t *out);

// --- Traversal ---------------------------------------------------------

// Returning false stops the walk early; the traversal still reports
// DV_GRAPH_OK. `depth` is edges from the start vertex, which is 0 for the
// start itself and, for a breadth-first walk, the shortest hop count.
typedef bool (*dv_graph_visit_fn)(dv_vertex_t vertex, uint32_t depth,
                                  void *user_data);

// Both follow out-edges on a directed graph and incident edges on an
// undirected one, and both take a mutable graph because they use the scratch
// the graph owns. Neither allocates once the scratch is sized.
dv_graph_status_t dv_graph_bfs(dv_graph_t *g, dv_vertex_t start,
                               dv_graph_visit_fn visit, void *user_data);
dv_graph_status_t dv_graph_dfs(dv_graph_t *g, dv_vertex_t start,
                               dv_graph_visit_fn visit, void *user_data);

dv_graph_status_t dv_graph_reachable(dv_graph_t *g, dv_vertex_t from,
                                     dv_vertex_t to, bool *out);

// --- Diagnostics -------------------------------------------------------

const char *dv_graph_status_str(dv_graph_status_t status);

// Walks every list in both directions and checks it against the recorded
// degrees and counts: link symmetry, chain length, endpoint agreement, and
// free-list disjointness. O(V + E), for tests and debug assertions.
bool dv_graph_is_valid(const dv_graph_t *g);

// --- Inline accessors --------------------------------------------------

static inline uint32_t dv_graph_vertex_count(const dv_graph_t *g) {
  return g ? g->vertex_count : 0u;
}

static inline uint32_t dv_graph_edge_count(const dv_graph_t *g) {
  return g ? g->edge_count : 0u;
}

static inline uint32_t dv_graph_vertex_capacity(const dv_graph_t *g) {
  return g ? g->vertex_capacity : 0u;
}

static inline uint32_t dv_graph_edge_capacity(const dv_graph_t *g) {
  return g ? g->edge_capacity : 0u;
}

static inline bool dv_graph_is_directed(const dv_graph_t *g) {
  return g && g->directed;
}

static inline bool dv_graph_is_weighted(const dv_graph_t *g) {
  return g && g->weighted;
}

// Steps on every topology mutation, or once per committed update phase. An
// iterator that captured a different value refuses to advance.
static inline uint64_t dv_graph_version(const dv_graph_t *g) {
  return g ? g->version : 0u;
}

// --- Iteration ---------------------------------------------------------

typedef struct {
  const dv_graph_t *graph;
  uint64_t version;
  uint32_t vertex; // the vertex whose lists are being walked
  uint32_t edge;   // next edge to yield
  bool incoming;   // currently on the in-list
  bool both;       // walk the out-list, then the in-list
} dv_graph_edge_iter_t;

typedef struct {
  dv_edge_t edge;
  dv_vertex_t src;      // as stored
  dv_vertex_t dst;      // as stored
  dv_vertex_t neighbor; // the endpoint opposite the vertex being walked
  double weight;        // 0.0 on an unweighted graph
} dv_graph_edge_ref_t;

typedef struct {
  const dv_graph_t *graph;
  uint64_t version;
  uint32_t index;
} dv_graph_vertex_iter_t;

// Internal: an exhausted iterator, which is what every rejected begin yields
// so a caller that ignores the status simply iterates nothing.
static inline void dv_graph_edge_iter_end(dv_graph_edge_iter_t *it) {
  it->graph = NULL;
  it->version = 0u;
  it->vertex = DV_GRAPH_INVALID_INDEX;
  it->edge = DV_GRAPH_INVALID_INDEX;
  it->incoming = false;
  it->both = false;
}

dv_graph_status_t dv_graph_out_edges_begin(const dv_graph_t *g,
                                           dv_vertex_t vertex,
                                           dv_graph_edge_iter_t *it);
dv_graph_status_t dv_graph_in_edges_begin(const dv_graph_t *g,
                                          dv_vertex_t vertex,
                                          dv_graph_edge_iter_t *it);

// Every edge touching the vertex, out-list first. On an undirected graph this
// is the neighbour walk; on a directed one it is both directions. A self-loop
// sits on both lists and is therefore yielded twice, the same way it counts
// twice toward the degree.
dv_graph_status_t dv_graph_edges_begin(const dv_graph_t *g, dv_vertex_t vertex,
                                       dv_graph_edge_iter_t *it);

// Yields the next edge, or false when the walk is done or the graph has been
// mutated since the iterator was created. `out` may be NULL to count.
static inline bool dv_graph_edge_iter_next(dv_graph_edge_iter_t *it,
                                           dv_graph_edge_ref_t *out) {
  if (!it || !it->graph)
    return false;

  const dv_graph_t *g = it->graph;
  if (it->version != g->version) {
    dv_graph_edge_iter_end(it);
    return false;
  }

  while (it->edge == DV_GRAPH_INVALID_INDEX) {
    if (!it->both || it->incoming) {
      dv_graph_edge_iter_end(it);
      return false;
    }
    it->incoming = true;
    it->edge = g->vertices[it->vertex].in_head;
  }

  const uint32_t index = it->edge;
  const dv_graph_edge_rec_t *e = &g->edges[index];

  if (out) {
    const dv_edge_t handle = {index, e->generation};
    const dv_vertex_t src = {e->src, g->vertices[e->src].generation};
    const dv_vertex_t dst = {e->dst, g->vertices[e->dst].generation};

    out->edge = handle;
    out->src = src;
    out->dst = dst;
    out->neighbor = (e->src == it->vertex && !it->incoming) ? dst : src;
    out->weight = g->weights ? g->weights[index] : 0.0;
  }

  it->edge = it->incoming ? e->next_in : e->next_out;
  return true;
}

static inline void dv_graph_vertices_begin(const dv_graph_t *g,
                                           dv_graph_vertex_iter_t *it) {
  if (!it)
    return;

  it->graph = g;
  it->version = g ? g->version : 0u;
  it->index = 0u;
}

// Yields live vertices in slot order. `out` may be NULL.
static inline bool dv_graph_vertex_iter_next(dv_graph_vertex_iter_t *it,
                                             dv_vertex_t *out) {
  if (!it || !it->graph)
    return false;

  const dv_graph_t *g = it->graph;
  if (it->version != g->version) {
    it->graph = NULL;
    return false;
  }

  while (it->index < g->vertex_watermark) {
    const dv_graph_vertex_rec_t *v = &g->vertices[it->index];
    const uint32_t index = it->index++;

    if (v->active) {
      if (out) {
        const dv_vertex_t handle = {index, v->generation};
        *out = handle;
      }
      return true;
    }
  }

  return false;
}

#endif // DV_GRAPH_H
