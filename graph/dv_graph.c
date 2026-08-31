// dv_graph.c - adjacency-list graph over two pooled, generation-tagged slabs.

#include "dv_graph.h"

#include <stdlib.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

// Selector carried in the top bit of a depth-first edge cursor: set means the
// cursor is walking the in-list. Indices stay below DV_GRAPH_MAX_CAPACITY, so
// a flagged index can never collide with DV_GRAPH_INVALID_INDEX.
#define DV_GRAPH_CURSOR_IN 0x80000000u
#define DV_GRAPH_CURSOR_MASK 0x7FFFFFFFu

// --- Size and alignment arithmetic -------------------------------------

static bool dv_graph_is_pow2(size_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static bool dv_graph_align_up(size_t size, size_t align, size_t *out) {
  if (!out || !dv_graph_is_pow2(align))
    return false;

  if (size > SIZE_MAX - (align - 1u))
    return false;

  *out = (size + align - 1u) & ~(align - 1u);
  return true;
}

static dv_graph_status_t dv_graph_bytes_for(uint32_t count, size_t elem_size,
                                            size_t *out) {
  const size_t n = (size_t)count;

  if (n != 0u && elem_size > SIZE_MAX / n)
    return DV_GRAPH_ERR_OVERFLOW;

  *out = n * elem_size;
  return DV_GRAPH_OK;
}

// --- Default allocator hooks -------------------------------------------

#define DV_GRAPH_MALLOC_ALIGN (alignof(max_align_t))

static void *dv_graph_over_aligned_alloc(size_t size, size_t align) {
#if defined(_MSC_VER)
  return _aligned_malloc(size, align);
#else
  size_t padded = 0u;
  if (!dv_graph_align_up(size, align, &padded))
    return NULL;
  return aligned_alloc(align, padded);
#endif
}

static void dv_graph_over_aligned_free(void *ptr) {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static void *dv_graph_default_alloc(size_t size, size_t align,
                                    void *user_data) {
  (void)user_data;

  if (align <= DV_GRAPH_MALLOC_ALIGN)
    return malloc(size);
  return dv_graph_over_aligned_alloc(size, align);
}

static void *dv_graph_default_realloc(void *ptr, size_t old_size,
                                      size_t new_size, size_t align,
                                      void *user_data) {
  (void)user_data;

  if (align <= DV_GRAPH_MALLOC_ALIGN)
    return realloc(ptr, new_size);

  void *mem = dv_graph_over_aligned_alloc(new_size, align);
  if (!mem)
    return NULL;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);

  dv_graph_over_aligned_free(ptr);
  return mem;
}

static void dv_graph_default_free(void *ptr, size_t size, size_t align,
                                  void *user_data) {
  (void)size;
  (void)user_data;

  if (align <= DV_GRAPH_MALLOC_ALIGN) {
    free(ptr);
    return;
  }
  dv_graph_over_aligned_free(ptr);
}

static dv_graph_allocator_t dv_graph_default_allocator(void) {
  const dv_graph_allocator_t hooks = {
      .alloc = dv_graph_default_alloc,
      .realloc = dv_graph_default_realloc,
      .free = dv_graph_default_free,
      .user_data = NULL,
  };
  return hooks;
}

// --- Block resizing ----------------------------------------------------
//
// One helper for all four blocks: the two pools, the weights parallel to the
// edge pool, and the traversal scratch. `live_bytes` is what must survive a
// move when the allocator cannot resize in place.

static dv_graph_status_t dv_graph_resize_block(dv_graph_t *g, void **block,
                                               size_t old_bytes,
                                               size_t new_bytes,
                                               size_t live_bytes) {
  if (new_bytes == old_bytes)
    return DV_GRAPH_OK;

  if (new_bytes == 0u) {
    if (*block)
      g->allocator.free(*block, old_bytes, g->align, g->allocator.user_data);
    *block = NULL;
    return DV_GRAPH_OK;
  }

  void *mem = NULL;
  if (!*block) {
    mem = g->allocator.alloc(new_bytes, g->align, g->allocator.user_data);
  } else if (g->allocator.realloc) {
    mem = g->allocator.realloc(*block, old_bytes, new_bytes, g->align,
                               g->allocator.user_data);
  } else {
    mem = g->allocator.alloc(new_bytes, g->align, g->allocator.user_data);
    if (mem) {
      const size_t copy = live_bytes < new_bytes ? live_bytes : new_bytes;
      if (copy > 0u)
        memcpy(mem, *block, copy);
      g->allocator.free(*block, old_bytes, g->align, g->allocator.user_data);
    }
  }

  if (!mem)
    return DV_GRAPH_ERR_NOMEM;

  *block = mem;
  return DV_GRAPH_OK;
}

static uint32_t dv_graph_next_capacity(uint32_t capacity) {
  if (capacity < DV_GRAPH_MIN_CAPACITY)
    return DV_GRAPH_MIN_CAPACITY;

  const uint32_t half = capacity / 2u;
  if (capacity > DV_GRAPH_MAX_CAPACITY - half)
    return DV_GRAPH_MAX_CAPACITY;

  return capacity + half;
}

static uint32_t dv_graph_clamp_capacity(uint32_t capacity, uint32_t ceiling) {
  if (ceiling != 0u && capacity > ceiling)
    return ceiling;
  return capacity;
}

static dv_graph_status_t dv_graph_set_vertex_capacity(dv_graph_t *g,
                                                      uint32_t capacity) {
  if (capacity == g->vertex_capacity)
    return DV_GRAPH_OK;
  if (capacity > DV_GRAPH_MAX_CAPACITY)
    return DV_GRAPH_ERR_OVERFLOW;
  if (capacity < g->vertex_watermark)
    return DV_GRAPH_ERR_INVALID;

  size_t old_bytes = 0u;
  size_t new_bytes = 0u;
  dv_graph_status_t status = dv_graph_bytes_for(
      g->vertex_capacity, sizeof(dv_graph_vertex_rec_t), &old_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  status =
      dv_graph_bytes_for(capacity, sizeof(dv_graph_vertex_rec_t), &new_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  const size_t live =
      (size_t)g->vertex_watermark * sizeof(dv_graph_vertex_rec_t);

  void *block = g->vertices;
  status = dv_graph_resize_block(g, &block, old_bytes, new_bytes, live);
  if (status != DV_GRAPH_OK)
    return status;

  g->vertices = (dv_graph_vertex_rec_t *)block;
  g->vertex_capacity = capacity;
  return DV_GRAPH_OK;
}

static dv_graph_status_t dv_graph_resize_weights(dv_graph_t *g,
                                                 uint32_t capacity) {
  if (!g->weighted || capacity == g->weight_capacity)
    return DV_GRAPH_OK;

  size_t old_bytes = 0u;
  size_t new_bytes = 0u;
  dv_graph_status_t status =
      dv_graph_bytes_for(g->weight_capacity, sizeof(double), &old_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  status = dv_graph_bytes_for(capacity, sizeof(double), &new_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  const size_t live = (size_t)g->edge_watermark * sizeof(double);

  void *block = g->weights;
  status = dv_graph_resize_block(g, &block, old_bytes, new_bytes, live);
  if (status != DV_GRAPH_OK)
    return status;

  g->weights = (double *)block;
  g->weight_capacity = capacity;
  return DV_GRAPH_OK;
}

// The weights are resized before the pool grows and after it shrinks, so the
// array is never shorter than the pool that indexes it. A failure in either
// half leaves the other consistent -- an oversized weights array wastes bytes
// and nothing else, while a short one would be an out-of-bounds write.
static dv_graph_status_t dv_graph_set_edge_capacity(dv_graph_t *g,
                                                    uint32_t capacity) {
  if (capacity == g->edge_capacity)
    return dv_graph_resize_weights(g, capacity);
  if (capacity > DV_GRAPH_MAX_CAPACITY)
    return DV_GRAPH_ERR_OVERFLOW;
  if (capacity < g->edge_watermark)
    return DV_GRAPH_ERR_INVALID;

  const bool growing = capacity > g->edge_capacity;
  if (growing) {
    const dv_graph_status_t status = dv_graph_resize_weights(g, capacity);
    if (status != DV_GRAPH_OK)
      return status;
  }

  size_t old_bytes = 0u;
  size_t new_bytes = 0u;
  dv_graph_status_t status = dv_graph_bytes_for(
      g->edge_capacity, sizeof(dv_graph_edge_rec_t), &old_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  status = dv_graph_bytes_for(capacity, sizeof(dv_graph_edge_rec_t),
                              &new_bytes);
  if (status != DV_GRAPH_OK)
    return status;

  const size_t live = (size_t)g->edge_watermark * sizeof(dv_graph_edge_rec_t);

  void *block = g->edges;
  status = dv_graph_resize_block(g, &block, old_bytes, new_bytes, live);
  if (status != DV_GRAPH_OK)
    return status;

  g->edges = (dv_graph_edge_rec_t *)block;
  g->edge_capacity = capacity;

  if (!growing)
    return dv_graph_resize_weights(g, capacity);

  return DV_GRAPH_OK;
}

// --- Traversal scratch -------------------------------------------------

static void dv_graph_free_scratch(dv_graph_t *g) {
  if (g->work) {
    const size_t bytes = (size_t)g->work_capacity * 2u * sizeof(uint32_t);
    g->allocator.free(g->work, bytes, g->align, g->allocator.user_data);
    g->work = NULL;
  }

  if (g->visited) {
    const size_t bytes = g->visited_words * sizeof(uint64_t);
    g->allocator.free(g->visited, bytes, g->align, g->allocator.user_data);
    g->visited = NULL;
  }

  g->work_capacity = 0u;
  g->visited_words = 0u;
}

// Sizes the scratch for the current vertex capacity. Idempotent, and a no-op
// once it is large enough, which is what keeps repeated traversals free of
// allocator traffic.
static dv_graph_status_t dv_graph_ensure_scratch(dv_graph_t *g) {
  const uint32_t capacity = g->vertex_capacity;
  if (capacity == 0u)
    return DV_GRAPH_OK;

  const size_t words = ((size_t)capacity + 63u) / 64u;
  if (g->work_capacity >= capacity && g->visited_words >= words)
    return DV_GRAPH_OK;

  size_t work_bytes = 0u;
  if (dv_graph_bytes_for(capacity, 2u * sizeof(uint32_t), &work_bytes) !=
      DV_GRAPH_OK)
    return DV_GRAPH_ERR_OVERFLOW;

  // Nothing in the scratch outlives a traversal, so both blocks resize with a
  // live size of zero. Each counter is updated only after its own block moves,
  // which keeps a partial failure describing exactly what was allocated.
  if (g->work_capacity < capacity) {
    void *work = g->work;
    const size_t old_bytes =
        (size_t)g->work_capacity * 2u * sizeof(uint32_t);

    const dv_graph_status_t status =
        dv_graph_resize_block(g, &work, old_bytes, work_bytes, 0u);
    if (status != DV_GRAPH_OK)
      return status;

    g->work = (uint32_t *)work;
    g->work_capacity = capacity;
  }

  if (g->visited_words < words) {
    void *visited = g->visited;
    const size_t old_bytes = g->visited_words * sizeof(uint64_t);

    const dv_graph_status_t status = dv_graph_resize_block(
        g, &visited, old_bytes, words * sizeof(uint64_t), 0u);
    if (status != DV_GRAPH_OK)
      return status;

    g->visited = (uint64_t *)visited;
    g->visited_words = words;
  }

  return DV_GRAPH_OK;
}

static inline bool dv_graph_visited_test(const dv_graph_t *g, uint32_t index) {
  return (g->visited[index >> 6u] & (1ull << (index & 63u))) != 0ull;
}

static inline void dv_graph_visited_set(dv_graph_t *g, uint32_t index) {
  g->visited[index >> 6u] |= 1ull << (index & 63u);
}

// --- Version and update phase ------------------------------------------

static inline void dv_graph_touch(dv_graph_t *g) {
  if (g->in_update)
    g->update_dirty = true;
  else
    ++g->version;
}

// Opens an implicit phase around a batch so the version steps once. Returns
// whether a phase was already open, which the matching end must not close.
static inline bool dv_graph_batch_begin(dv_graph_t *g) {
  const bool outer = g->in_update;
  g->in_update = true;
  return outer;
}

static inline void dv_graph_batch_end(dv_graph_t *g, bool outer) {
  if (outer)
    return;

  g->in_update = false;
  if (g->update_dirty) {
    g->update_dirty = false;
    ++g->version;
  }
}

// --- Slot handling -----------------------------------------------------
//
// Each pool is a high-water mark plus a free list of recycled slots. Growing
// therefore costs a resize and nothing else: there is no free list to rebuild,
// and a slot beyond the mark needs no initialization until it is taken.

static inline uint32_t dv_graph_bump(uint32_t generation) {
  const uint32_t next = generation + 1u;
  return next == 0u ? 1u : next; // 0 is reserved for "no handle"
}

static dv_graph_status_t dv_graph_grow_vertices(dv_graph_t *g) {
  if (!g->growable)
    return DV_GRAPH_ERR_FULL;

  const uint32_t next = dv_graph_clamp_capacity(
      dv_graph_next_capacity(g->vertex_capacity), g->max_vertex_capacity);
  if (next <= g->vertex_capacity)
    return DV_GRAPH_ERR_FULL;

  return dv_graph_set_vertex_capacity(g, next);
}

static dv_graph_status_t dv_graph_grow_edges(dv_graph_t *g) {
  if (!g->growable)
    return DV_GRAPH_ERR_FULL;

  const uint32_t next = dv_graph_clamp_capacity(
      dv_graph_next_capacity(g->edge_capacity), g->max_edge_capacity);
  if (next <= g->edge_capacity)
    return DV_GRAPH_ERR_FULL;

  return dv_graph_set_edge_capacity(g, next);
}

static dv_graph_status_t dv_graph_take_vertex(dv_graph_t *g, uint32_t *out) {
  if (g->vertex_free != DV_GRAPH_INVALID_INDEX) {
    const uint32_t index = g->vertex_free;
    g->vertex_free = g->vertices[index].out_head;
    *out = index;
    return DV_GRAPH_OK;
  }

  if (g->vertex_watermark == g->vertex_capacity) {
    const dv_graph_status_t status = dv_graph_grow_vertices(g);
    if (status != DV_GRAPH_OK)
      return status;
  }

  const uint32_t index = g->vertex_watermark++;
  g->vertices[index].generation = 1u; // first use; never recycled before
  *out = index;
  return DV_GRAPH_OK;
}

static dv_graph_status_t dv_graph_take_edge(dv_graph_t *g, uint32_t *out) {
  if (g->edge_free != DV_GRAPH_INVALID_INDEX) {
    const uint32_t index = g->edge_free;
    g->edge_free = g->edges[index].next_out;
    *out = index;
    return DV_GRAPH_OK;
  }

  if (g->edge_watermark == g->edge_capacity) {
    const dv_graph_status_t status = dv_graph_grow_edges(g);
    if (status != DV_GRAPH_OK)
      return status;
  }

  const uint32_t index = g->edge_watermark++;
  g->edges[index].generation = 1u;
  *out = index;
  return DV_GRAPH_OK;
}

// --- Handle validation -------------------------------------------------

static inline bool dv_graph_vertex_live(const dv_graph_t *g, dv_vertex_t v) {
  if (v.generation == 0u || v.index >= g->vertex_watermark)
    return false;

  const dv_graph_vertex_rec_t *rec = &g->vertices[v.index];
  return rec->active && rec->generation == v.generation;
}

static inline bool dv_graph_edge_live(const dv_graph_t *g, dv_edge_t e) {
  if (e.generation == 0u || e.index >= g->edge_watermark)
    return false;

  const dv_graph_edge_rec_t *rec = &g->edges[e.index];
  return rec->active && rec->generation == e.generation;
}

// --- Intrusive list maintenance ----------------------------------------

static void dv_graph_link_edge(dv_graph_t *g, uint32_t index, uint32_t src,
                               uint32_t dst) {
  dv_graph_edge_rec_t *e = &g->edges[index];
  dv_graph_vertex_rec_t *s = &g->vertices[src];
  dv_graph_vertex_rec_t *d = &g->vertices[dst];

  e->src = src;
  e->dst = dst;
  e->active = true;

  // Append to the source's out-list, so iteration yields insertion order.
  e->next_out = DV_GRAPH_INVALID_INDEX;
  e->prev_out = s->out_tail;
  if (s->out_tail == DV_GRAPH_INVALID_INDEX)
    s->out_head = index;
  else
    g->edges[s->out_tail].next_out = index;
  s->out_tail = index;
  ++s->out_degree;

  // And to the destination's in-list. A self-loop lands on both lists of the
  // same vertex, which is why the tail is re-read from the record rather than
  // from the local `s` copy of it.
  e->next_in = DV_GRAPH_INVALID_INDEX;
  e->prev_in = d->in_tail;
  if (d->in_tail == DV_GRAPH_INVALID_INDEX)
    d->in_head = index;
  else
    g->edges[d->in_tail].next_in = index;
  d->in_tail = index;
  ++d->in_degree;
}

static void dv_graph_unlink_edge(dv_graph_t *g, uint32_t index) {
  dv_graph_edge_rec_t *e = &g->edges[index];
  dv_graph_vertex_rec_t *s = &g->vertices[e->src];
  dv_graph_vertex_rec_t *d = &g->vertices[e->dst];

  if (e->prev_out == DV_GRAPH_INVALID_INDEX)
    s->out_head = e->next_out;
  else
    g->edges[e->prev_out].next_out = e->next_out;

  if (e->next_out == DV_GRAPH_INVALID_INDEX)
    s->out_tail = e->prev_out;
  else
    g->edges[e->next_out].prev_out = e->prev_out;

  --s->out_degree;

  if (e->prev_in == DV_GRAPH_INVALID_INDEX)
    d->in_head = e->next_in;
  else
    g->edges[e->prev_in].next_in = e->next_in;

  if (e->next_in == DV_GRAPH_INVALID_INDEX)
    d->in_tail = e->prev_in;
  else
    g->edges[e->next_in].prev_in = e->prev_in;

  --d->in_degree;
}

// Unlinks, retires the slot and pushes it on the free list. The generation
// bump is what turns every outstanding handle to this edge stale.
static void dv_graph_release_edge(dv_graph_t *g, uint32_t index) {
  dv_graph_unlink_edge(g, index);

  dv_graph_edge_rec_t *e = &g->edges[index];
  e->active = false;
  e->generation = dv_graph_bump(e->generation);
  e->next_out = g->edge_free;
  g->edge_free = index;

  --g->edge_count;
}

// --- Lifecycle ---------------------------------------------------------

dv_graph_status_t dv_graph_init(dv_graph_t *g, const dv_graph_config_t *config,
                                const dv_graph_allocator_t *allocator) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;

  memset(g, 0, sizeof(*g));

  if (!config)
    return DV_GRAPH_ERR_INVALID;

  if (config->initial_vertex_capacity > DV_GRAPH_MAX_CAPACITY ||
      config->initial_edge_capacity > DV_GRAPH_MAX_CAPACITY ||
      config->max_vertex_capacity > DV_GRAPH_MAX_CAPACITY ||
      config->max_edge_capacity > DV_GRAPH_MAX_CAPACITY)
    return DV_GRAPH_ERR_OVERFLOW;

  uint32_t max_vertices = config->max_vertex_capacity;
  uint32_t max_edges = config->max_edge_capacity;
  if (!config->growable) {
    max_vertices = config->initial_vertex_capacity;
    max_edges = config->initial_edge_capacity;
  } else {
    if (max_vertices != 0u && max_vertices < config->initial_vertex_capacity)
      return DV_GRAPH_ERR_INVALID;
    if (max_edges != 0u && max_edges < config->initial_edge_capacity)
      return DV_GRAPH_ERR_INVALID;
  }

  dv_graph_allocator_t hooks;
  if (allocator) {
    if (!allocator->alloc || !allocator->free)
      return DV_GRAPH_ERR_INVALID;
    hooks = *allocator;
  } else {
    hooks = dv_graph_default_allocator();
  }

  size_t align = alignof(max_align_t);
  if (config->cache_aligned && align < DV_CACHE_LINE_SIZE)
    align = DV_CACHE_LINE_SIZE;

  g->version = 1u; // 0 is what a zeroed iterator carries
  g->vertex_free = DV_GRAPH_INVALID_INDEX;
  g->edge_free = DV_GRAPH_INVALID_INDEX;
  g->align = align;
  g->initial_vertex_capacity = config->initial_vertex_capacity;
  g->initial_edge_capacity = config->initial_edge_capacity;
  g->max_vertex_capacity = max_vertices;
  g->max_edge_capacity = max_edges;
  g->allocator = hooks;
  g->directed = config->directed;
  g->weighted = config->weighted;
  g->growable = config->growable;
  g->cache_aligned = config->cache_aligned;
  g->reject_parallel_edges = config->reject_parallel_edges;
  g->reject_self_loops = config->reject_self_loops;

  dv_graph_status_t status =
      dv_graph_set_vertex_capacity(g, config->initial_vertex_capacity);
  if (status == DV_GRAPH_OK)
    status = dv_graph_set_edge_capacity(g, config->initial_edge_capacity);

  // Fixed capacity means no allocator traffic after init, and a traversal that
  // sized its scratch lazily would break that promise.
  if (status == DV_GRAPH_OK && !g->growable)
    status = dv_graph_ensure_scratch(g);

  if (status != DV_GRAPH_OK) {
    dv_graph_destroy(g);
    return status;
  }

  return DV_GRAPH_OK;
}

void dv_graph_clear(dv_graph_t *g) {
  if (!g)
    return;

  // Every live slot's generation is bumped, so handles held across a clear are
  // rejected rather than silently re-bound. That is what makes this O(V + E)
  // rather than a counter reset.
  for (uint32_t i = 0u; i < g->edge_watermark; ++i) {
    dv_graph_edge_rec_t *e = &g->edges[i];
    if (e->active)
      e->generation = dv_graph_bump(e->generation);
    e->active = false;
    e->next_out = (i + 1u == g->edge_watermark) ? DV_GRAPH_INVALID_INDEX
                                                : (i + 1u);
  }
  g->edge_free =
      g->edge_watermark == 0u ? DV_GRAPH_INVALID_INDEX : 0u;
  g->edge_count = 0u;

  for (uint32_t i = 0u; i < g->vertex_watermark; ++i) {
    dv_graph_vertex_rec_t *v = &g->vertices[i];
    if (v->active)
      v->generation = dv_graph_bump(v->generation);
    v->active = false;
    v->out_tail = DV_GRAPH_INVALID_INDEX;
    v->in_head = DV_GRAPH_INVALID_INDEX;
    v->in_tail = DV_GRAPH_INVALID_INDEX;
    v->out_degree = 0u;
    v->in_degree = 0u;
    v->out_head = (i + 1u == g->vertex_watermark) ? DV_GRAPH_INVALID_INDEX
                                                  : (i + 1u);
  }
  g->vertex_free =
      g->vertex_watermark == 0u ? DV_GRAPH_INVALID_INDEX : 0u;
  g->vertex_count = 0u;

  dv_graph_touch(g);
}

dv_graph_status_t dv_graph_reset(dv_graph_t *g) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;

  // Unlike clear, reset drops the high-water marks, so slot generations start
  // over: handles issued before a reset must not be used after it.
  g->vertex_watermark = 0u;
  g->edge_watermark = 0u;
  g->vertex_free = DV_GRAPH_INVALID_INDEX;
  g->edge_free = DV_GRAPH_INVALID_INDEX;
  g->vertex_count = 0u;
  g->edge_count = 0u;
  g->in_update = false;
  g->update_dirty = false;
  ++g->version;

  dv_graph_status_t status =
      dv_graph_set_vertex_capacity(g, g->initial_vertex_capacity);
  const dv_graph_status_t edge_status =
      dv_graph_set_edge_capacity(g, g->initial_edge_capacity);
  if (status == DV_GRAPH_OK)
    status = edge_status;

  return status;
}

void dv_graph_destroy(dv_graph_t *g) {
  if (!g)
    return;

  if (g->allocator.free) {
    size_t bytes = 0u;

    if (g->vertices &&
        dv_graph_bytes_for(g->vertex_capacity, sizeof(dv_graph_vertex_rec_t),
                           &bytes) == DV_GRAPH_OK)
      g->allocator.free(g->vertices, bytes, g->align, g->allocator.user_data);

    if (g->edges &&
        dv_graph_bytes_for(g->edge_capacity, sizeof(dv_graph_edge_rec_t),
                           &bytes) == DV_GRAPH_OK)
      g->allocator.free(g->edges, bytes, g->align, g->allocator.user_data);

    if (g->weights && dv_graph_bytes_for(g->weight_capacity, sizeof(double),
                                         &bytes) == DV_GRAPH_OK)
      g->allocator.free(g->weights, bytes, g->align, g->allocator.user_data);

    dv_graph_free_scratch(g);
  }

  memset(g, 0, sizeof(*g));
}

// --- Update phase ------------------------------------------------------

dv_graph_status_t dv_graph_begin_update(dv_graph_t *g) {
  if (!g || g->in_update)
    return DV_GRAPH_ERR_INVALID;

  g->in_update = true;
  g->update_dirty = false;
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_commit_update(dv_graph_t *g) {
  if (!g || !g->in_update)
    return DV_GRAPH_ERR_INVALID;

  g->in_update = false;
  if (g->update_dirty) {
    g->update_dirty = false;
    ++g->version;
  }
  return DV_GRAPH_OK;
}

// --- Vertices ----------------------------------------------------------

dv_graph_status_t dv_graph_add_vertex(dv_graph_t *g, dv_vertex_t *out) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;

  uint32_t index = 0u;
  const dv_graph_status_t status = dv_graph_take_vertex(g, &index);
  if (status != DV_GRAPH_OK)
    return status;

  dv_graph_vertex_rec_t *v = &g->vertices[index];
  v->out_head = DV_GRAPH_INVALID_INDEX;
  v->out_tail = DV_GRAPH_INVALID_INDEX;
  v->in_head = DV_GRAPH_INVALID_INDEX;
  v->in_tail = DV_GRAPH_INVALID_INDEX;
  v->out_degree = 0u;
  v->in_degree = 0u;
  v->active = true;

  ++g->vertex_count;
  dv_graph_touch(g);

  if (out) {
    const dv_vertex_t handle = {index, v->generation};
    *out = handle;
  }
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_remove_vertex(dv_graph_t *g, dv_vertex_t vertex) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, vertex))
    return DV_GRAPH_ERR_STALE;

  const uint32_t index = vertex.index;
  dv_graph_vertex_rec_t *v = &g->vertices[index];

  // Every incident edge is unlinked from both of its lists, so a self-loop
  // freed in the first pass is already gone from the in-list the second walks.
  uint32_t e = v->out_head;
  while (e != DV_GRAPH_INVALID_INDEX) {
    const uint32_t next = g->edges[e].next_out;
    dv_graph_release_edge(g, e);
    e = next;
  }

  e = v->in_head;
  while (e != DV_GRAPH_INVALID_INDEX) {
    const uint32_t next = g->edges[e].next_in;
    dv_graph_release_edge(g, e);
    e = next;
  }

  v->active = false;
  v->generation = dv_graph_bump(v->generation);
  v->out_head = g->vertex_free;
  g->vertex_free = index;

  --g->vertex_count;
  dv_graph_touch(g);
  return DV_GRAPH_OK;
}

bool dv_graph_vertex_valid(const dv_graph_t *g, dv_vertex_t vertex) {
  return g && dv_graph_vertex_live(g, vertex);
}

dv_vertex_t dv_graph_vertex_at(const dv_graph_t *g, uint32_t index) {
  if (!g || index >= g->vertex_watermark || !g->vertices[index].active)
    return dv_vertex_none();

  const dv_vertex_t handle = {index, g->vertices[index].generation};
  return handle;
}

// --- Edges -------------------------------------------------------------

// Scans the shorter of the two candidate lists. On an undirected graph the
// pair is unordered, so the walk is over incident edges and matches either
// orientation.
static uint32_t dv_graph_locate_edge(const dv_graph_t *g, uint32_t a,
                                     uint32_t b) {
  const dv_graph_vertex_rec_t *va = &g->vertices[a];
  const dv_graph_vertex_rec_t *vb = &g->vertices[b];

  if (g->directed) {
    if (va->out_degree <= vb->in_degree) {
      for (uint32_t e = va->out_head; e != DV_GRAPH_INVALID_INDEX;
           e = g->edges[e].next_out) {
        if (g->edges[e].dst == b)
          return e;
      }
      return DV_GRAPH_INVALID_INDEX;
    }

    for (uint32_t e = vb->in_head; e != DV_GRAPH_INVALID_INDEX;
         e = g->edges[e].next_in) {
      if (g->edges[e].src == a)
        return e;
    }
    return DV_GRAPH_INVALID_INDEX;
  }

  const uint32_t deg_a = va->out_degree + va->in_degree;
  const uint32_t deg_b = vb->out_degree + vb->in_degree;
  const uint32_t from = deg_a <= deg_b ? a : b;
  const uint32_t other = deg_a <= deg_b ? b : a;
  const dv_graph_vertex_rec_t *v = &g->vertices[from];

  for (uint32_t e = v->out_head; e != DV_GRAPH_INVALID_INDEX;
       e = g->edges[e].next_out) {
    if (g->edges[e].dst == other)
      return e;
  }

  for (uint32_t e = v->in_head; e != DV_GRAPH_INVALID_INDEX;
       e = g->edges[e].next_in) {
    if (g->edges[e].src == other)
      return e;
  }

  return DV_GRAPH_INVALID_INDEX;
}

dv_graph_status_t dv_graph_add_edge(dv_graph_t *g, dv_vertex_t src,
                                    dv_vertex_t dst, double weight,
                                    dv_edge_t *out) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, src) || !dv_graph_vertex_live(g, dst))
    return DV_GRAPH_ERR_STALE;
  if (g->reject_self_loops && src.index == dst.index)
    return DV_GRAPH_ERR_INVALID;
  if (g->reject_parallel_edges &&
      dv_graph_locate_edge(g, src.index, dst.index) != DV_GRAPH_INVALID_INDEX)
    return DV_GRAPH_ERR_EXISTS;

  uint32_t index = 0u;
  const dv_graph_status_t status = dv_graph_take_edge(g, &index);
  if (status != DV_GRAPH_OK)
    return status;

  dv_graph_link_edge(g, index, src.index, dst.index);
  if (g->weights)
    g->weights[index] = weight;

  ++g->edge_count;
  dv_graph_touch(g);

  if (out) {
    const dv_edge_t handle = {index, g->edges[index].generation};
    *out = handle;
  }
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_remove_edge(dv_graph_t *g, dv_edge_t edge) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_edge_live(g, edge))
    return DV_GRAPH_ERR_STALE;

  dv_graph_release_edge(g, edge.index);
  dv_graph_touch(g);
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_remove_edge_between(dv_graph_t *g, dv_vertex_t src,
                                               dv_vertex_t dst) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, src) || !dv_graph_vertex_live(g, dst))
    return DV_GRAPH_ERR_STALE;

  const uint32_t index = dv_graph_locate_edge(g, src.index, dst.index);
  if (index == DV_GRAPH_INVALID_INDEX)
    return DV_GRAPH_ERR_NOT_FOUND;

  dv_graph_release_edge(g, index);
  dv_graph_touch(g);
  return DV_GRAPH_OK;
}

bool dv_graph_edge_valid(const dv_graph_t *g, dv_edge_t edge) {
  return g && dv_graph_edge_live(g, edge);
}

dv_edge_t dv_graph_edge_at(const dv_graph_t *g, uint32_t index) {
  if (!g || index >= g->edge_watermark || !g->edges[index].active)
    return dv_edge_none();

  const dv_edge_t handle = {index, g->edges[index].generation};
  return handle;
}

dv_graph_status_t dv_graph_find_edge(const dv_graph_t *g, dv_vertex_t src,
                                     dv_vertex_t dst, dv_edge_t *out) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, src) || !dv_graph_vertex_live(g, dst))
    return DV_GRAPH_ERR_STALE;

  const uint32_t index = dv_graph_locate_edge(g, src.index, dst.index);
  if (index == DV_GRAPH_INVALID_INDEX)
    return DV_GRAPH_ERR_NOT_FOUND;

  if (out) {
    const dv_edge_t handle = {index, g->edges[index].generation};
    *out = handle;
  }
  return DV_GRAPH_OK;
}

bool dv_graph_has_edge(const dv_graph_t *g, dv_vertex_t src, dv_vertex_t dst) {
  return dv_graph_find_edge(g, src, dst, NULL) == DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_edge_endpoints(const dv_graph_t *g, dv_edge_t edge,
                                          dv_vertex_t *src, dv_vertex_t *dst) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_edge_live(g, edge))
    return DV_GRAPH_ERR_STALE;

  const dv_graph_edge_rec_t *e = &g->edges[edge.index];
  if (src) {
    const dv_vertex_t handle = {e->src, g->vertices[e->src].generation};
    *src = handle;
  }
  if (dst) {
    const dv_vertex_t handle = {e->dst, g->vertices[e->dst].generation};
    *dst = handle;
  }
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_edge_weight(const dv_graph_t *g, dv_edge_t edge,
                                       double *out) {
  if (!g || !out || !g->weights)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_edge_live(g, edge))
    return DV_GRAPH_ERR_STALE;

  *out = g->weights[edge.index];
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_set_edge_weight(dv_graph_t *g, dv_edge_t edge,
                                           double weight) {
  if (!g || !g->weights)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_edge_live(g, edge))
    return DV_GRAPH_ERR_STALE;

  // A weight is not topology: iterators stay valid, so the version holds.
  g->weights[edge.index] = weight;
  return DV_GRAPH_OK;
}

// --- Degrees -----------------------------------------------------------

uint32_t dv_graph_out_degree(const dv_graph_t *g, dv_vertex_t vertex) {
  if (!g || !dv_graph_vertex_live(g, vertex))
    return 0u;
  return g->vertices[vertex.index].out_degree;
}

uint32_t dv_graph_in_degree(const dv_graph_t *g, dv_vertex_t vertex) {
  if (!g || !dv_graph_vertex_live(g, vertex))
    return 0u;
  return g->vertices[vertex.index].in_degree;
}

uint32_t dv_graph_degree(const dv_graph_t *g, dv_vertex_t vertex) {
  if (!g || !dv_graph_vertex_live(g, vertex))
    return 0u;

  const dv_graph_vertex_rec_t *v = &g->vertices[vertex.index];
  return v->out_degree + v->in_degree;
}

// --- Capacity management -----------------------------------------------

dv_graph_status_t dv_graph_reserve_vertices(dv_graph_t *g, uint32_t capacity) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (capacity <= g->vertex_capacity)
    return DV_GRAPH_OK;
  if (!g->growable)
    return DV_GRAPH_ERR_FULL;
  if (g->max_vertex_capacity != 0u && capacity > g->max_vertex_capacity)
    return DV_GRAPH_ERR_FULL;
  if (capacity > DV_GRAPH_MAX_CAPACITY)
    return DV_GRAPH_ERR_OVERFLOW;

  return dv_graph_set_vertex_capacity(g, capacity);
}

dv_graph_status_t dv_graph_reserve_edges(dv_graph_t *g, uint32_t capacity) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (capacity <= g->edge_capacity)
    return DV_GRAPH_OK;
  if (!g->growable)
    return DV_GRAPH_ERR_FULL;
  if (g->max_edge_capacity != 0u && capacity > g->max_edge_capacity)
    return DV_GRAPH_ERR_FULL;
  if (capacity > DV_GRAPH_MAX_CAPACITY)
    return DV_GRAPH_ERR_OVERFLOW;

  return dv_graph_set_edge_capacity(g, capacity);
}

dv_graph_status_t dv_graph_reserve_traversal(dv_graph_t *g) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;

  return dv_graph_ensure_scratch(g);
}

dv_graph_status_t dv_graph_shrink_to_fit(dv_graph_t *g) {
  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!g->growable)
    return DV_GRAPH_OK;

  // Capacity comes back to the high-water mark, not to the live count:
  // compacting the pools would move slots and invalidate every handle, which
  // is the one thing the handle model promises not to do.
  dv_graph_status_t status =
      dv_graph_set_vertex_capacity(g, g->vertex_watermark);
  const dv_graph_status_t edge_status =
      dv_graph_set_edge_capacity(g, g->edge_watermark);
  if (status == DV_GRAPH_OK)
    status = edge_status;

  dv_graph_free_scratch(g);
  return status;
}

// --- Bulk operations ---------------------------------------------------

// Every slot below the high-water mark is either live or on the free list, so
// the room left in a pool is exactly capacity - live count.
static void dv_graph_reserve_for_bulk(dv_graph_t *g, uint32_t live,
                                      uint32_t capacity, uint32_t count,
                                      bool vertices) {
  if (count > DV_GRAPH_MAX_CAPACITY - live)
    return;

  const uint32_t needed = live + count;
  if (needed <= capacity)
    return;

  const uint32_t curve = dv_graph_next_capacity(capacity);
  const uint32_t target = needed > curve ? needed : curve;
  const uint32_t ceiling =
      vertices ? g->max_vertex_capacity : g->max_edge_capacity;
  const uint32_t clamped = dv_graph_clamp_capacity(target, ceiling);

  const dv_graph_status_t status =
      vertices ? dv_graph_reserve_vertices(g, clamped)
               : dv_graph_reserve_edges(g, clamped);
  if (status == DV_GRAPH_OK)
    return;

  // The growth curve overshot what the allocator would give; fall back to the
  // exact request before giving up on the batch.
  const uint32_t exact = dv_graph_clamp_capacity(needed, ceiling);
  if (exact < clamped) {
    if (vertices)
      (void)dv_graph_reserve_vertices(g, exact);
    else
      (void)dv_graph_reserve_edges(g, exact);
  }
}

uint32_t dv_graph_add_vertices(dv_graph_t *g, uint32_t count,
                               dv_vertex_t *out) {
  if (!g || count == 0u)
    return 0u;

  if (g->growable)
    dv_graph_reserve_for_bulk(g, g->vertex_count, g->vertex_capacity, count,
                              true);

  const bool outer = dv_graph_batch_begin(g);

  uint32_t added = 0u;
  for (; added < count; ++added) {
    if (dv_graph_add_vertex(g, out ? &out[added] : NULL) != DV_GRAPH_OK)
      break;
  }

  dv_graph_batch_end(g, outer);
  return added;
}

uint32_t dv_graph_add_edges(dv_graph_t *g, const dv_graph_edge_spec_t *specs,
                            uint32_t count, dv_edge_t *out) {
  if (!g || !specs || count == 0u)
    return 0u;

  if (g->growable)
    dv_graph_reserve_for_bulk(g, g->edge_count, g->edge_capacity, count,
                              false);

  const bool outer = dv_graph_batch_begin(g);

  uint32_t added = 0u;
  for (; added < count; ++added) {
    const dv_graph_edge_spec_t *spec = &specs[added];
    if (dv_graph_add_edge(g, spec->src, spec->dst, spec->weight,
                          out ? &out[added] : NULL) != DV_GRAPH_OK)
      break;
  }

  dv_graph_batch_end(g, outer);
  return added;
}

// --- Adjacency iteration -----------------------------------------------

static dv_graph_status_t dv_graph_iter_begin(const dv_graph_t *g,
                                             dv_vertex_t vertex,
                                             dv_graph_edge_iter_t *it,
                                             bool incoming, bool both) {
  if (!it)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_edge_iter_end(it);

  if (!g)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, vertex))
    return DV_GRAPH_ERR_STALE;

  const dv_graph_vertex_rec_t *v = &g->vertices[vertex.index];

  it->graph = g;
  it->version = g->version;
  it->vertex = vertex.index;
  it->edge = incoming ? v->in_head : v->out_head;
  it->incoming = incoming;
  it->both = both;
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_out_edges_begin(const dv_graph_t *g,
                                           dv_vertex_t vertex,
                                           dv_graph_edge_iter_t *it) {
  return dv_graph_iter_begin(g, vertex, it, false, false);
}

dv_graph_status_t dv_graph_in_edges_begin(const dv_graph_t *g,
                                          dv_vertex_t vertex,
                                          dv_graph_edge_iter_t *it) {
  return dv_graph_iter_begin(g, vertex, it, true, false);
}

dv_graph_status_t dv_graph_edges_begin(const dv_graph_t *g, dv_vertex_t vertex,
                                       dv_graph_edge_iter_t *it) {
  return dv_graph_iter_begin(g, vertex, it, false, true);
}

// --- Traversal ---------------------------------------------------------
//
// Both walks run off the scratch the graph owns: `work` holds a frontier of
// vertex indices in its first half and, in its second, the per-entry depth for
// a breadth-first walk or the per-entry edge cursor for a depth-first one. A
// vertex is marked before it is pushed, so neither structure can exceed one
// entry per vertex and the scratch is exactly the right size by construction.

static uint32_t dv_graph_first_cursor(const dv_graph_t *g, uint32_t vertex,
                                      bool incident) {
  const dv_graph_vertex_rec_t *v = &g->vertices[vertex];
  if (v->out_head != DV_GRAPH_INVALID_INDEX)
    return v->out_head;
  if (!incident || v->in_head == DV_GRAPH_INVALID_INDEX)
    return DV_GRAPH_INVALID_INDEX;

  return v->in_head | DV_GRAPH_CURSOR_IN;
}

static uint32_t dv_graph_next_cursor(const dv_graph_t *g, uint32_t vertex,
                                     uint32_t cursor, bool incident) {
  const uint32_t index = cursor & DV_GRAPH_CURSOR_MASK;

  if ((cursor & DV_GRAPH_CURSOR_IN) != 0u) {
    const uint32_t next = g->edges[index].next_in;
    return next == DV_GRAPH_INVALID_INDEX ? DV_GRAPH_INVALID_INDEX
                                          : (next | DV_GRAPH_CURSOR_IN);
  }

  const uint32_t next = g->edges[index].next_out;
  if (next != DV_GRAPH_INVALID_INDEX)
    return next;

  const uint32_t head = g->vertices[vertex].in_head;
  if (!incident || head == DV_GRAPH_INVALID_INDEX)
    return DV_GRAPH_INVALID_INDEX;

  return head | DV_GRAPH_CURSOR_IN;
}

static dv_graph_status_t dv_graph_traverse_setup(dv_graph_t *g,
                                                 dv_vertex_t start,
                                                 dv_graph_visit_fn visit) {
  if (!g || !visit)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, start))
    return DV_GRAPH_ERR_STALE;

  const dv_graph_status_t status = dv_graph_ensure_scratch(g);
  if (status != DV_GRAPH_OK)
    return status;

  if (g->visited_words > 0u)
    memset(g->visited, 0, g->visited_words * sizeof(uint64_t));

  return DV_GRAPH_OK;
}

static inline dv_vertex_t dv_graph_handle(const dv_graph_t *g,
                                          uint32_t index) {
  const dv_vertex_t handle = {index, g->vertices[index].generation};
  return handle;
}

dv_graph_status_t dv_graph_bfs(dv_graph_t *g, dv_vertex_t start,
                               dv_graph_visit_fn visit, void *user_data) {
  const dv_graph_status_t status = dv_graph_traverse_setup(g, start, visit);
  if (status != DV_GRAPH_OK)
    return status;

  const bool incident = !g->directed;
  uint32_t *queue = g->work;
  uint32_t *depth = g->work + g->work_capacity;

  size_t head = 0u;
  size_t tail = 0u;

  dv_graph_visited_set(g, start.index);
  queue[tail] = start.index;
  depth[tail] = 0u;
  ++tail;

  while (head < tail) {
    const uint32_t index = queue[head];
    const uint32_t d = depth[head];
    ++head;

    if (!visit(dv_graph_handle(g, index), d, user_data))
      return DV_GRAPH_OK;

    for (uint32_t e = g->vertices[index].out_head;
         e != DV_GRAPH_INVALID_INDEX; e = g->edges[e].next_out) {
      const uint32_t next = g->edges[e].dst;
      if (dv_graph_visited_test(g, next))
        continue;

      dv_graph_visited_set(g, next);
      queue[tail] = next;
      depth[tail] = d + 1u;
      ++tail;
    }

    if (!incident)
      continue;

    for (uint32_t e = g->vertices[index].in_head; e != DV_GRAPH_INVALID_INDEX;
         e = g->edges[e].next_in) {
      const uint32_t next = g->edges[e].src;
      if (dv_graph_visited_test(g, next))
        continue;

      dv_graph_visited_set(g, next);
      queue[tail] = next;
      depth[tail] = d + 1u;
      ++tail;
    }
  }

  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_dfs(dv_graph_t *g, dv_vertex_t start,
                               dv_graph_visit_fn visit, void *user_data) {
  const dv_graph_status_t status = dv_graph_traverse_setup(g, start, visit);
  if (status != DV_GRAPH_OK)
    return status;

  const bool incident = !g->directed;
  uint32_t *stack = g->work;
  uint32_t *cursor = g->work + g->work_capacity;

  dv_graph_visited_set(g, start.index);
  if (!visit(dv_graph_handle(g, start.index), 0u, user_data))
    return DV_GRAPH_OK;

  size_t depth = 0u;
  stack[depth] = start.index;
  cursor[depth] = dv_graph_first_cursor(g, start.index, incident);
  ++depth;

  // The cursor is resumed rather than restarted, so this is the same order a
  // recursive walk would produce, with the stack depth as the visit depth.
  while (depth > 0u) {
    const size_t top = depth - 1u;
    const uint32_t c = cursor[top];

    if (c == DV_GRAPH_INVALID_INDEX) {
      --depth;
      continue;
    }

    cursor[top] = dv_graph_next_cursor(g, stack[top], c, incident);

    const uint32_t index = c & DV_GRAPH_CURSOR_MASK;
    const dv_graph_edge_rec_t *e = &g->edges[index];
    const uint32_t next = (c & DV_GRAPH_CURSOR_IN) != 0u ? e->src : e->dst;

    if (dv_graph_visited_test(g, next))
      continue;

    dv_graph_visited_set(g, next);
    if (!visit(dv_graph_handle(g, next), (uint32_t)depth, user_data))
      return DV_GRAPH_OK;

    stack[depth] = next;
    cursor[depth] = dv_graph_first_cursor(g, next, incident);
    ++depth;
  }

  return DV_GRAPH_OK;
}

typedef struct {
  uint32_t target;
  bool found;
} dv_graph_reach_state_t;

static bool dv_graph_reach_visit(dv_vertex_t vertex, uint32_t depth,
                                 void *user_data) {
  dv_graph_reach_state_t *state = (dv_graph_reach_state_t *)user_data;
  (void)depth;

  if (vertex.index != state->target)
    return true;

  state->found = true;
  return false; // stop the walk at the first sighting
}

dv_graph_status_t dv_graph_reachable(dv_graph_t *g, dv_vertex_t from,
                                     dv_vertex_t to, bool *out) {
  if (!g || !out)
    return DV_GRAPH_ERR_INVALID;
  if (!dv_graph_vertex_live(g, from) || !dv_graph_vertex_live(g, to))
    return DV_GRAPH_ERR_STALE;

  dv_graph_reach_state_t state = {to.index, false};
  const dv_graph_status_t status =
      dv_graph_bfs(g, from, dv_graph_reach_visit, &state);
  if (status != DV_GRAPH_OK)
    return status;

  *out = state.found;
  return DV_GRAPH_OK;
}

// --- Diagnostics -------------------------------------------------------

const char *dv_graph_status_str(dv_graph_status_t status) {
  switch (status) {
  case DV_GRAPH_OK:
    return "DV_GRAPH_OK";
  case DV_GRAPH_ERR_INVALID:
    return "DV_GRAPH_ERR_INVALID";
  case DV_GRAPH_ERR_STALE:
    return "DV_GRAPH_ERR_STALE";
  case DV_GRAPH_ERR_NOT_FOUND:
    return "DV_GRAPH_ERR_NOT_FOUND";
  case DV_GRAPH_ERR_EXISTS:
    return "DV_GRAPH_ERR_EXISTS";
  case DV_GRAPH_ERR_FULL:
    return "DV_GRAPH_ERR_FULL";
  case DV_GRAPH_ERR_NOMEM:
    return "DV_GRAPH_ERR_NOMEM";
  case DV_GRAPH_ERR_OVERFLOW:
    return "DV_GRAPH_ERR_OVERFLOW";
  }

  return "DV_GRAPH_ERR_UNKNOWN";
}

// Counts a free list without trusting it: a corrupt link either leaves the
// pool or revisits a slot, and the iteration bound catches the cycle.
static bool dv_graph_free_list_len(const dv_graph_t *g, bool vertices,
                                   uint32_t *out) {
  const uint32_t watermark =
      vertices ? g->vertex_watermark : g->edge_watermark;

  uint32_t index = vertices ? g->vertex_free : g->edge_free;
  uint32_t seen = 0u;

  while (index != DV_GRAPH_INVALID_INDEX) {
    if (index >= watermark || seen > watermark)
      return false;

    if (vertices) {
      if (g->vertices[index].active)
        return false;
      index = g->vertices[index].out_head;
    } else {
      if (g->edges[index].active)
        return false;
      index = g->edges[index].next_out;
    }

    ++seen;
  }

  *out = seen;
  return true;
}

bool dv_graph_is_valid(const dv_graph_t *g) {
  if (!g)
    return false;
  if (g->vertex_watermark > g->vertex_capacity ||
      g->edge_watermark > g->edge_capacity)
    return false;
  if (g->vertex_count > g->vertex_watermark ||
      g->edge_count > g->edge_watermark)
    return false;
  if (g->vertex_watermark > 0u && !g->vertices)
    return false;
  if (g->edge_watermark > 0u && !g->edges)
    return false;
  if (g->weighted && g->edge_capacity > 0u &&
      (!g->weights || g->weight_capacity < g->edge_capacity))
    return false;

  uint32_t active_vertices = 0u;
  uint32_t out_total = 0u;
  uint32_t in_total = 0u;

  for (uint32_t i = 0u; i < g->vertex_watermark; ++i) {
    const dv_graph_vertex_rec_t *v = &g->vertices[i];
    if (!v->active)
      continue;
    if (v->generation == 0u)
      return false;

    ++active_vertices;
    out_total += v->out_degree;
    in_total += v->in_degree;

    // Chain length has to match the recorded degree, which is also what rules
    // out a cycle inside a per-vertex edge chain.
    uint32_t walked = 0u;
    for (uint32_t e = v->out_head; e != DV_GRAPH_INVALID_INDEX;
         e = g->edges[e].next_out) {
      if (e >= g->edge_watermark || !g->edges[e].active)
        return false;
      if (g->edges[e].src != i)
        return false;
      if (++walked > v->out_degree)
        return false;
    }
    if (walked != v->out_degree)
      return false;

    walked = 0u;
    for (uint32_t e = v->in_head; e != DV_GRAPH_INVALID_INDEX;
         e = g->edges[e].next_in) {
      if (e >= g->edge_watermark || !g->edges[e].active)
        return false;
      if (g->edges[e].dst != i)
        return false;
      if (++walked > v->in_degree)
        return false;
    }
    if (walked != v->in_degree)
      return false;

    if ((v->out_head == DV_GRAPH_INVALID_INDEX) !=
        (v->out_tail == DV_GRAPH_INVALID_INDEX))
      return false;
    if ((v->in_head == DV_GRAPH_INVALID_INDEX) !=
        (v->in_tail == DV_GRAPH_INVALID_INDEX))
      return false;
  }

  if (active_vertices != g->vertex_count)
    return false;
  if (out_total != g->edge_count || in_total != g->edge_count)
    return false;

  uint32_t active_edges = 0u;
  for (uint32_t i = 0u; i < g->edge_watermark; ++i) {
    const dv_graph_edge_rec_t *e = &g->edges[i];
    if (!e->active)
      continue;
    if (e->generation == 0u)
      return false;

    ++active_edges;

    if (e->src >= g->vertex_watermark || e->dst >= g->vertex_watermark)
      return false;
    if (!g->vertices[e->src].active || !g->vertices[e->dst].active)
      return false;

    // Link symmetry in both directions. An edge that fell off a list without
    // being retired shows up here: its neighbours no longer point back at it.
    if (e->prev_out == DV_GRAPH_INVALID_INDEX) {
      if (g->vertices[e->src].out_head != i)
        return false;
    } else if (g->edges[e->prev_out].next_out != i) {
      return false;
    }

    if (e->next_out == DV_GRAPH_INVALID_INDEX) {
      if (g->vertices[e->src].out_tail != i)
        return false;
    } else if (g->edges[e->next_out].prev_out != i) {
      return false;
    }

    if (e->prev_in == DV_GRAPH_INVALID_INDEX) {
      if (g->vertices[e->dst].in_head != i)
        return false;
    } else if (g->edges[e->prev_in].next_in != i) {
      return false;
    }

    if (e->next_in == DV_GRAPH_INVALID_INDEX) {
      if (g->vertices[e->dst].in_tail != i)
        return false;
    } else if (g->edges[e->next_in].prev_in != i) {
      return false;
    }
  }

  if (active_edges != g->edge_count)
    return false;

  // Every slot below a high-water mark is either live or on that pool's free
  // list, and nothing is on both.
  uint32_t free_vertices = 0u;
  uint32_t free_edges = 0u;
  if (!dv_graph_free_list_len(g, true, &free_vertices))
    return false;
  if (!dv_graph_free_list_len(g, false, &free_edges))
    return false;

  if (g->vertex_count + free_vertices != g->vertex_watermark)
    return false;
  if (g->edge_count + free_edges != g->edge_watermark)
    return false;

  return true;
}
