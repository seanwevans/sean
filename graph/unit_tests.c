// unit_tests.c

#include "dv_graph.h"
#include "dv_graph_mt.h"

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_VERTICES 8u
#define TEST_EDGES 16u
#define DIFF_OPS 20000u
#define DIFF_VERTICES 48u
#define DIFF_EDGES 512u
#define MT_THREADS 4u
#define MT_EDGES_PER_THREAD 2000u

#define ASSERT_TRUE(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "ASSERT_TRUE failed: %s (%s:%d)\n", #expr, __FILE__,     \
              __LINE__);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ_U64(a, b)                                                    \
  do {                                                                         \
    const uint64_t _a = (uint64_t)(a);                                         \
    const uint64_t _b = (uint64_t)(b);                                         \
    if (_a != _b) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_EQ_U64 failed: %s != %s (got=%llu expected=%llu) "       \
              "(%s:%d)\n",                                                     \
              #a, #b, (unsigned long long)_a, (unsigned long long)_b,          \
              __FILE__, __LINE__);                                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_STATUS(expr, expected)                                          \
  do {                                                                         \
    const dv_graph_status_t _s = (expr);                                       \
    const dv_graph_status_t _e = (expected);                                   \
    if (_s != _e) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_STATUS failed: %s -> %s (expected %s) (%s:%d)\n", #expr, \
              dv_graph_status_str(_s), dv_graph_status_str(_e), __FILE__,      \
              __LINE__);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_OK(expr) ASSERT_STATUS((expr), DV_GRAPH_OK)

#define ASSERT_GRAPH_VALID(g)                                                  \
  do {                                                                         \
    if (!dv_graph_is_valid(g)) {                                               \
      fprintf(stderr, "graph invariants violated: %s (%s:%d)\n", #g,           \
              __FILE__, __LINE__);                                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

typedef bool (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} test_case_t;

// --- Test allocator ----------------------------------------------------

typedef struct {
  size_t live_bytes;
  size_t peak_bytes;
  size_t alloc_calls;
  size_t realloc_calls;
  size_t free_calls;
  size_t budget; // SIZE_MAX means unlimited
  size_t last_align;
  bool align_respected;
} tracking_state_t;

static void tracking_init(tracking_state_t *st) {
  memset(st, 0, sizeof(*st));
  st->budget = SIZE_MAX;
  st->align_respected = true;
}

static bool tracking_take_budget(tracking_state_t *st) {
  if (st->budget == SIZE_MAX)
    return true;
  if (st->budget == 0u)
    return false;

  --st->budget;
  return true;
}

static void *tracking_raw_alloc(size_t size, size_t align) {
  const size_t padded = (size + align - 1u) & ~(align - 1u);
  return aligned_alloc(align, padded);
}

static void *tracking_alloc(size_t size, size_t align, void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;

  st->last_align = align;
  ++st->alloc_calls;
  if (!tracking_take_budget(st))
    return NULL;

  void *mem = tracking_raw_alloc(size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    st->align_respected = false;

  st->live_bytes += size;
  if (st->live_bytes > st->peak_bytes)
    st->peak_bytes = st->live_bytes;

  return mem;
}

static void tracking_free(void *ptr, size_t size, size_t align,
                          void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;
  (void)align;

  if (!ptr)
    return;

  ++st->free_calls;
  st->live_bytes -= size;
  free(ptr);
}

static void *tracking_realloc(void *ptr, size_t old_size, size_t new_size,
                              size_t align, void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;

  st->last_align = align;
  ++st->realloc_calls;
  if (!tracking_take_budget(st))
    return NULL;

  void *mem = tracking_raw_alloc(new_size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    st->align_respected = false;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);
  free(ptr);

  st->live_bytes -= old_size;
  st->live_bytes += new_size;
  if (st->live_bytes > st->peak_bytes)
    st->peak_bytes = st->live_bytes;

  return mem;
}

static dv_graph_allocator_t tracking_hooks(tracking_state_t *st,
                                           bool with_realloc) {
  const dv_graph_allocator_t hooks = {
      .alloc = tracking_alloc,
      .realloc = with_realloc ? tracking_realloc : NULL,
      .free = tracking_free,
      .user_data = st,
  };
  return hooks;
}

// --- Helpers -----------------------------------------------------------

static dv_graph_config_t base_config(bool directed, bool growable) {
  const dv_graph_config_t config = {
      .directed = directed,
      .weighted = false,
      .growable = growable,
      .cache_aligned = false,
      .reject_parallel_edges = false,
      .reject_self_loops = false,
      .initial_vertex_capacity = TEST_VERTICES,
      .initial_edge_capacity = TEST_EDGES,
      .max_vertex_capacity = 0u,
      .max_edge_capacity = 0u,
  };
  return config;
}

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

// Adds `count` vertices and stores their handles, returning false if any add
// fails, so a test body reads as the property it is checking.
static bool add_vertices(dv_graph_t *g, dv_vertex_t *out, uint32_t count) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (dv_graph_add_vertex(g, &out[i]) != DV_GRAPH_OK)
      return false;
  }
  return true;
}

static uint32_t count_iter(dv_graph_edge_iter_t *it) {
  uint32_t seen = 0u;
  while (dv_graph_edge_iter_next(it, NULL))
    ++seen;
  return seen;
}

// --- Configuration -----------------------------------------------------

static bool test_init_rejects_bad_config(void) {
  dv_graph_t g;
  const dv_graph_config_t valid = base_config(true, false);

  ASSERT_STATUS(dv_graph_init(NULL, &valid, NULL), DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_init(&g, NULL, NULL), DV_GRAPH_ERR_INVALID);

  dv_graph_config_t bad = valid;
  bad.growable = true;
  bad.max_vertex_capacity = TEST_VERTICES - 1u;
  ASSERT_STATUS(dv_graph_init(&g, &bad, NULL), DV_GRAPH_ERR_INVALID);

  bad = valid;
  bad.growable = true;
  bad.max_edge_capacity = TEST_EDGES - 1u;
  ASSERT_STATUS(dv_graph_init(&g, &bad, NULL), DV_GRAPH_ERR_INVALID);

  bad = valid;
  bad.initial_vertex_capacity = DV_GRAPH_MAX_CAPACITY + 1u;
  ASSERT_STATUS(dv_graph_init(&g, &bad, NULL), DV_GRAPH_ERR_OVERFLOW);

  tracking_state_t st;
  tracking_init(&st);
  dv_graph_allocator_t hooks = tracking_hooks(&st, true);
  hooks.free = NULL;
  ASSERT_STATUS(dv_graph_init(&g, &valid, &hooks), DV_GRAPH_ERR_INVALID);

  hooks = tracking_hooks(&st, true);
  hooks.alloc = NULL;
  ASSERT_STATUS(dv_graph_init(&g, &valid, &hooks), DV_GRAPH_ERR_INVALID);

  // A rejected init leaves a zeroed control block, which destroy accepts.
  dv_graph_destroy(&g);
  return true;
}

static bool test_empty_graph_accessors(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 0u);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 0u);
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), TEST_VERTICES);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), TEST_EDGES);
  ASSERT_TRUE(dv_graph_is_directed(&g));
  ASSERT_FALSE(dv_graph_is_weighted(&g));
  ASSERT_GRAPH_VALID(&g);

  const dv_vertex_t none = dv_vertex_none();
  ASSERT_TRUE(dv_vertex_is_none(none));
  ASSERT_TRUE(dv_edge_is_none(dv_edge_none()));
  ASSERT_FALSE(dv_graph_vertex_valid(&g, none));
  ASSERT_EQ_U64(dv_graph_degree(&g, none), 0u);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, none), 0u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, none), 0u);
  ASSERT_STATUS(dv_graph_remove_vertex(&g, none), DV_GRAPH_ERR_STALE);
  ASSERT_STATUS(dv_graph_remove_edge(&g, dv_edge_none()), DV_GRAPH_ERR_STALE);
  ASSERT_TRUE(dv_vertex_is_none(dv_graph_vertex_at(&g, 0u)));
  ASSERT_TRUE(dv_edge_is_none(dv_graph_edge_at(&g, 0u)));

  // The zeroed handle a caller gets from a plain struct declaration is
  // rejected the same way, which is the point of reserving generation 0.
  const dv_vertex_t zeroed = {0u, 0u};
  ASSERT_FALSE(dv_graph_vertex_valid(&g, zeroed));

  dv_graph_edge_iter_t it;
  ASSERT_STATUS(dv_graph_edges_begin(&g, none, &it), DV_GRAPH_ERR_STALE);
  ASSERT_EQ_U64(count_iter(&it), 0u);

  dv_graph_vertex_iter_t vit;
  dv_graph_vertices_begin(&g, &vit);
  ASSERT_FALSE(dv_graph_vertex_iter_next(&vit, NULL));

  bool reached = true;
  ASSERT_STATUS(dv_graph_reachable(&g, none, none, &reached),
                DV_GRAPH_ERR_STALE);

  dv_graph_destroy(&g);
  dv_graph_destroy(&g); // idempotent
  return true;
}

static bool test_status_strings(void) {
  const dv_graph_status_t all[] = {
      DV_GRAPH_OK,        DV_GRAPH_ERR_INVALID, DV_GRAPH_ERR_STALE,
      DV_GRAPH_ERR_NOT_FOUND, DV_GRAPH_ERR_EXISTS,  DV_GRAPH_ERR_FULL,
      DV_GRAPH_ERR_NOMEM, DV_GRAPH_ERR_OVERFLOW,
  };

  for (size_t i = 0u; i < sizeof(all) / sizeof(all[0]); ++i) {
    const char *name = dv_graph_status_str(all[i]);
    ASSERT_TRUE(name != NULL);
    ASSERT_TRUE(strncmp(name, "DV_GRAPH_", 9u) == 0);
  }

  ASSERT_TRUE(strcmp(dv_graph_status_str((dv_graph_status_t)99),
                     "DV_GRAPH_ERR_UNKNOWN") == 0);
  return true;
}

// --- Vertices ----------------------------------------------------------

static bool test_vertex_lifecycle(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[4];
  ASSERT_TRUE(add_vertices(&g, v, 4u));
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 4u);
  ASSERT_GRAPH_VALID(&g);

  for (uint32_t i = 0u; i < 4u; ++i) {
    ASSERT_TRUE(dv_graph_vertex_valid(&g, v[i]));
    ASSERT_FALSE(dv_vertex_is_none(v[i]));
    ASSERT_TRUE(dv_vertex_eq(dv_graph_vertex_at(&g, v[i].index), v[i]));
  }

  ASSERT_OK(dv_graph_remove_vertex(&g, v[1]));
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 3u);
  ASSERT_FALSE(dv_graph_vertex_valid(&g, v[1]));
  ASSERT_STATUS(dv_graph_remove_vertex(&g, v[1]), DV_GRAPH_ERR_STALE);
  ASSERT_TRUE(dv_vertex_is_none(dv_graph_vertex_at(&g, v[1].index)));
  ASSERT_GRAPH_VALID(&g);

  // The freed slot is the next one handed out, and the handle it carries is
  // distinguishable from the one that named the same slot before.
  dv_vertex_t recycled = dv_vertex_none();
  ASSERT_OK(dv_graph_add_vertex(&g, &recycled));
  ASSERT_EQ_U64(recycled.index, v[1].index);
  ASSERT_TRUE(recycled.generation != v[1].generation);
  ASSERT_TRUE(dv_graph_vertex_valid(&g, recycled));
  ASSERT_FALSE(dv_graph_vertex_valid(&g, v[1]));
  ASSERT_FALSE(dv_vertex_eq(recycled, v[1]));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_vertex_iterator(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[5];
  ASSERT_TRUE(add_vertices(&g, v, 5u));
  ASSERT_OK(dv_graph_remove_vertex(&g, v[2]));

  dv_graph_vertex_iter_t it;
  dv_graph_vertices_begin(&g, &it);

  dv_vertex_t seen = dv_vertex_none();
  uint32_t count = 0u;
  while (dv_graph_vertex_iter_next(&it, &seen)) {
    ASSERT_TRUE(dv_graph_vertex_valid(&g, seen));
    ASSERT_FALSE(dv_vertex_eq(seen, v[2]));
    ++count;
  }
  ASSERT_EQ_U64(count, 4u);

  // A mutation between two advances stops the walk rather than reporting a
  // torn view of it.
  dv_graph_vertices_begin(&g, &it);
  ASSERT_TRUE(dv_graph_vertex_iter_next(&it, &seen));
  ASSERT_OK(dv_graph_add_vertex(&g, NULL));
  ASSERT_FALSE(dv_graph_vertex_iter_next(&it, &seen));

  dv_graph_destroy(&g);
  return true;
}

// --- Edges -------------------------------------------------------------

static bool test_directed_edges(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[3];
  ASSERT_TRUE(add_vertices(&g, v, 3u));

  dv_edge_t ab = dv_edge_none();
  dv_edge_t bc = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, &ab));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[2], 0.0, &bc));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[0]), 1u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, v[0]), 0u);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[1]), 1u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, v[1]), 1u);
  ASSERT_EQ_U64(dv_graph_degree(&g, v[1]), 2u);

  // Direction is honored in both the lookup and the miss.
  ASSERT_TRUE(dv_graph_has_edge(&g, v[0], v[1]));
  ASSERT_FALSE(dv_graph_has_edge(&g, v[1], v[0]));
  ASSERT_STATUS(dv_graph_find_edge(&g, v[1], v[0], NULL),
                DV_GRAPH_ERR_NOT_FOUND);

  dv_edge_t found = dv_edge_none();
  ASSERT_OK(dv_graph_find_edge(&g, v[0], v[1], &found));
  ASSERT_TRUE(dv_edge_eq(found, ab));

  dv_vertex_t src = dv_vertex_none();
  dv_vertex_t dst = dv_vertex_none();
  ASSERT_OK(dv_graph_edge_endpoints(&g, bc, &src, &dst));
  ASSERT_TRUE(dv_vertex_eq(src, v[1]));
  ASSERT_TRUE(dv_vertex_eq(dst, v[2]));

  ASSERT_OK(dv_graph_remove_edge(&g, ab));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_FALSE(dv_graph_edge_valid(&g, ab));
  ASSERT_STATUS(dv_graph_remove_edge(&g, ab), DV_GRAPH_ERR_STALE);
  ASSERT_STATUS(dv_graph_edge_endpoints(&g, ab, NULL, NULL),
                DV_GRAPH_ERR_STALE);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[0]), 0u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_OK(dv_graph_remove_edge_between(&g, v[1], v[2]));
  ASSERT_STATUS(dv_graph_remove_edge_between(&g, v[1], v[2]),
                DV_GRAPH_ERR_NOT_FOUND);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 0u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_undirected_edges(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(false, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[3];
  ASSERT_TRUE(add_vertices(&g, v, 3u));

  dv_edge_t ab = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, &ab));
  ASSERT_OK(dv_graph_add_edge(&g, v[2], v[1], 0.0, NULL));
  ASSERT_GRAPH_VALID(&g);

  // The pair is unordered: one stored edge answers both lookups.
  ASSERT_TRUE(dv_graph_has_edge(&g, v[0], v[1]));
  ASSERT_TRUE(dv_graph_has_edge(&g, v[1], v[0]));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_EQ_U64(dv_graph_degree(&g, v[1]), 2u);
  ASSERT_EQ_U64(dv_graph_degree(&g, v[0]), 1u);

  // And the incident walk reaches it from either end, whichever list it is
  // physically on.
  dv_graph_edge_iter_t it;
  ASSERT_OK(dv_graph_edges_begin(&g, v[1], &it));

  dv_graph_edge_ref_t ref;
  uint32_t neighbors = 0u;
  bool saw_first = false;
  bool saw_third = false;
  while (dv_graph_edge_iter_next(&it, &ref)) {
    ++neighbors;
    saw_first = saw_first || dv_vertex_eq(ref.neighbor, v[0]);
    saw_third = saw_third || dv_vertex_eq(ref.neighbor, v[2]);
  }
  ASSERT_EQ_U64(neighbors, 2u);
  ASSERT_TRUE(saw_first);
  ASSERT_TRUE(saw_third);

  ASSERT_OK(dv_graph_remove_edge_between(&g, v[1], v[0]));
  ASSERT_FALSE(dv_graph_has_edge(&g, v[0], v[1]));
  ASSERT_EQ_U64(dv_graph_degree(&g, v[0]), 0u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_self_loops(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(false, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v = dv_vertex_none();
  ASSERT_OK(dv_graph_add_vertex(&g, &v));
  ASSERT_OK(dv_graph_add_edge(&g, v, v, 0.0, NULL));
  ASSERT_GRAPH_VALID(&g);

  // A self-loop sits on both of the vertex's lists, so it counts twice and is
  // yielded twice -- one convention, applied consistently.
  ASSERT_EQ_U64(dv_graph_degree(&g, v), 2u);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v), 1u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, v), 1u);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);

  dv_graph_edge_iter_t it;
  ASSERT_OK(dv_graph_edges_begin(&g, v, &it));
  ASSERT_EQ_U64(count_iter(&it), 2u);

  ASSERT_OK(dv_graph_out_edges_begin(&g, v, &it));
  ASSERT_EQ_U64(count_iter(&it), 1u);

  // Removing the vertex releases the loop exactly once.
  ASSERT_OK(dv_graph_remove_vertex(&g, v));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 0u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);

  config.reject_self_loops = true;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_OK(dv_graph_add_vertex(&g, &v));
  ASSERT_STATUS(dv_graph_add_edge(&g, v, v, 0.0, NULL), DV_GRAPH_ERR_INVALID);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 0u);

  dv_graph_destroy(&g);
  return true;
}

static bool test_parallel_edges(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[2];
  ASSERT_TRUE(add_vertices(&g, v, 2u));

  // Parallel edges are allowed by default and are distinct handles.
  dv_edge_t first = dv_edge_none();
  dv_edge_t second = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, &first));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, &second));
  ASSERT_FALSE(dv_edge_eq(first, second));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[0]), 2u);

  // remove_edge_between drops one of them, not both.
  ASSERT_OK(dv_graph_remove_edge_between(&g, v[0], v[1]));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_TRUE(dv_graph_has_edge(&g, v[0], v[1]));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);

  config.reject_parallel_edges = true;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_STATUS(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL),
                DV_GRAPH_ERR_EXISTS);
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL)); // other direction
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  dv_graph_destroy(&g);

  // On an undirected graph the reverse orientation is the same edge.
  config.directed = false;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_STATUS(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL),
                DV_GRAPH_ERR_EXISTS);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_remove_vertex_drops_incident_edges(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[4];
  ASSERT_TRUE(add_vertices(&g, v, 4u));

  // v[1] carries edges in both directions plus a self-loop.
  dv_edge_t kept = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[2], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[3], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[3], 0.0, &kept));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 5u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_OK(dv_graph_remove_vertex(&g, v[1]));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 3u);
  ASSERT_TRUE(dv_graph_edge_valid(&g, kept));
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[0]), 1u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, v[3]), 1u);
  ASSERT_EQ_U64(dv_graph_out_degree(&g, v[3]), 0u);
  ASSERT_EQ_U64(dv_graph_in_degree(&g, v[2]), 0u);
  ASSERT_GRAPH_VALID(&g);

  // An edge released with its vertex is stale, not merely unreachable.
  ASSERT_STATUS(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL),
                DV_GRAPH_ERR_STALE);
  ASSERT_STATUS(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL),
                DV_GRAPH_ERR_STALE);

  dv_graph_destroy(&g);
  return true;
}

static bool test_weights(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.weighted = true;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_TRUE(dv_graph_is_weighted(&g));

  dv_vertex_t v[2];
  ASSERT_TRUE(add_vertices(&g, v, 2u));

  dv_edge_t e = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 2.5, &e));

  double weight = 0.0;
  ASSERT_OK(dv_graph_edge_weight(&g, e, &weight));
  ASSERT_TRUE(weight == 2.5);

  const uint64_t version = dv_graph_version(&g);
  ASSERT_OK(dv_graph_set_edge_weight(&g, e, -1.25));
  ASSERT_OK(dv_graph_edge_weight(&g, e, &weight));
  ASSERT_TRUE(weight == -1.25);

  // A weight is not topology, so iterators outlive the change.
  ASSERT_EQ_U64(dv_graph_version(&g), version);

  dv_graph_edge_iter_t it;
  dv_graph_edge_ref_t ref;
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(ref.weight == -1.25);

  ASSERT_STATUS(dv_graph_edge_weight(&g, dv_edge_none(), &weight),
                DV_GRAPH_ERR_STALE);
  dv_graph_destroy(&g);

  // An unweighted graph refuses both accessors rather than inventing a value.
  config.weighted = false;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 7.0, &e));
  ASSERT_STATUS(dv_graph_edge_weight(&g, e, &weight), DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_set_edge_weight(&g, e, 1.0), DV_GRAPH_ERR_INVALID);

  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(ref.weight == 0.0);

  dv_graph_destroy(&g);
  return true;
}

// --- Iteration ---------------------------------------------------------

static bool test_iteration_is_insertion_ordered(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[5];
  ASSERT_TRUE(add_vertices(&g, v, 5u));

  dv_edge_t added[4];
  for (uint32_t i = 1u; i < 5u; ++i)
    ASSERT_OK(dv_graph_add_edge(&g, v[0], v[i], (double)i, &added[i - 1u]));

  dv_graph_edge_iter_t it;
  dv_graph_edge_ref_t ref;
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));

  for (uint32_t i = 0u; i < 4u; ++i) {
    ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
    ASSERT_TRUE(dv_edge_eq(ref.edge, added[i]));
    ASSERT_TRUE(dv_vertex_eq(ref.src, v[0]));
    ASSERT_TRUE(dv_vertex_eq(ref.neighbor, v[i + 1u]));
  }
  ASSERT_FALSE(dv_graph_edge_iter_next(&it, &ref));

  // Removing from the middle keeps the order of what is left.
  ASSERT_OK(dv_graph_remove_edge(&g, added[1]));
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_edge_eq(ref.edge, added[0]));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_edge_eq(ref.edge, added[2]));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_edge_eq(ref.edge, added[3]));
  ASSERT_FALSE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_iterator_invalidation(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[3];
  ASSERT_TRUE(add_vertices(&g, v, 3u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[2], 0.0, NULL));

  dv_graph_edge_iter_t it;
  dv_graph_edge_ref_t ref;
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));

  const uint64_t before = dv_graph_version(&g);
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[2], 0.0, NULL));
  ASSERT_TRUE(dv_graph_version(&g) != before);
  ASSERT_FALSE(dv_graph_edge_iter_next(&it, &ref));

  // An update phase batches the mutations into one version step, so an
  // iterator taken before it is invalidated once rather than per edge.
  const uint64_t open = dv_graph_version(&g);
  ASSERT_OK(dv_graph_begin_update(&g));
  ASSERT_STATUS(dv_graph_begin_update(&g), DV_GRAPH_ERR_INVALID);
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[2], v[0], 0.0, NULL));
  ASSERT_EQ_U64(dv_graph_version(&g), open);
  ASSERT_OK(dv_graph_commit_update(&g));
  ASSERT_EQ_U64(dv_graph_version(&g), open + 1u);
  ASSERT_STATUS(dv_graph_commit_update(&g), DV_GRAPH_ERR_INVALID);

  // A phase that changed nothing does not disturb live iterators.
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_OK(dv_graph_begin_update(&g));
  ASSERT_OK(dv_graph_commit_update(&g));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_in_and_out_iterators(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[4];
  ASSERT_TRUE(add_vertices(&g, v, 4u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[3], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[3], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[3], v[2], 0.0, NULL));

  dv_graph_edge_iter_t it;
  ASSERT_OK(dv_graph_in_edges_begin(&g, v[3], &it));
  ASSERT_EQ_U64(count_iter(&it), 2u);

  ASSERT_OK(dv_graph_out_edges_begin(&g, v[3], &it));
  ASSERT_EQ_U64(count_iter(&it), 1u);

  // On a directed graph the incident walk is both lists, in that order.
  dv_graph_edge_ref_t ref;
  ASSERT_OK(dv_graph_edges_begin(&g, v[3], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_vertex_eq(ref.src, v[3]));
  ASSERT_TRUE(dv_vertex_eq(ref.neighbor, v[2]));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_vertex_eq(ref.dst, v[3]));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(dv_vertex_eq(ref.dst, v[3]));
  ASSERT_FALSE(dv_graph_edge_iter_next(&it, &ref));

  ASSERT_STATUS(dv_graph_out_edges_begin(&g, v[3], NULL),
                DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_in_edges_begin(NULL, v[3], &it),
                DV_GRAPH_ERR_INVALID);
  ASSERT_EQ_U64(count_iter(&it), 0u);

  dv_graph_destroy(&g);
  return true;
}

// --- Traversal ---------------------------------------------------------

typedef struct {
  uint32_t order[16];
  uint32_t depth[16];
  uint32_t count;
  uint32_t stop_after; // 0 means never stop early
} walk_t;

static bool walk_visit(dv_vertex_t vertex, uint32_t depth, void *user_data) {
  walk_t *w = (walk_t *)user_data;

  if (w->count < 16u) {
    w->order[w->count] = vertex.index;
    w->depth[w->count] = depth;
  }
  ++w->count;

  return w->stop_after == 0u || w->count < w->stop_after;
}

// 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3, 3 -> 4, and a disconnected 5.
static bool build_walk_graph(dv_graph_t *g, dv_vertex_t *v, bool directed) {
  const dv_graph_config_t config = base_config(directed, true);
  if (dv_graph_init(g, &config, NULL) != DV_GRAPH_OK)
    return false;
  if (!add_vertices(g, v, 6u))
    return false;

  const dv_graph_edge_spec_t specs[] = {
      {v[0], v[1], 0.0}, {v[0], v[2], 0.0}, {v[1], v[3], 0.0},
      {v[2], v[3], 0.0}, {v[3], v[4], 0.0},
  };

  return dv_graph_add_edges(g, specs, 5u, NULL) == 5u;
}

static bool test_bfs_order_and_depth(void) {
  dv_graph_t g;
  dv_vertex_t v[6];
  ASSERT_TRUE(build_walk_graph(&g, v, true));

  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));

  // Breadth first: every vertex at its shortest hop count, and nothing from
  // the disconnected component.
  ASSERT_EQ_U64(w.count, 5u);
  ASSERT_EQ_U64(w.order[0], v[0].index);
  ASSERT_EQ_U64(w.order[1], v[1].index);
  ASSERT_EQ_U64(w.order[2], v[2].index);
  ASSERT_EQ_U64(w.order[3], v[3].index);
  ASSERT_EQ_U64(w.order[4], v[4].index);
  ASSERT_EQ_U64(w.depth[0], 0u);
  ASSERT_EQ_U64(w.depth[1], 1u);
  ASSERT_EQ_U64(w.depth[2], 1u);
  ASSERT_EQ_U64(w.depth[3], 2u);
  ASSERT_EQ_U64(w.depth[4], 3u);

  // Direction is respected: nothing reaches the start from a sink.
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_bfs(&g, v[4], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 1u);

  memset(&w, 0, sizeof(w));
  w.stop_after = 3u;
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 3u);

  ASSERT_STATUS(dv_graph_bfs(&g, v[0], NULL, &w), DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_bfs(&g, dv_vertex_none(), walk_visit, &w),
                DV_GRAPH_ERR_STALE);

  dv_graph_destroy(&g);
  return true;
}

static bool test_dfs_order(void) {
  dv_graph_t g;
  dv_vertex_t v[6];
  ASSERT_TRUE(build_walk_graph(&g, v, true));

  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_dfs(&g, v[0], walk_visit, &w));

  // Depth first, resuming each cursor where it left off: 0, 1, 3, 4, then back
  // up to the second edge out of 0.
  ASSERT_EQ_U64(w.count, 5u);
  ASSERT_EQ_U64(w.order[0], v[0].index);
  ASSERT_EQ_U64(w.order[1], v[1].index);
  ASSERT_EQ_U64(w.order[2], v[3].index);
  ASSERT_EQ_U64(w.order[3], v[4].index);
  ASSERT_EQ_U64(w.order[4], v[2].index);
  ASSERT_EQ_U64(w.depth[2], 2u);
  ASSERT_EQ_U64(w.depth[3], 3u);
  ASSERT_EQ_U64(w.depth[4], 1u);

  memset(&w, 0, sizeof(w));
  w.stop_after = 1u;
  ASSERT_OK(dv_graph_dfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 1u);

  dv_graph_destroy(&g);
  return true;
}

static bool test_undirected_traversal_follows_both_lists(void) {
  dv_graph_t g;
  const dv_graph_config_t config = base_config(false, true);
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[3];
  ASSERT_TRUE(add_vertices(&g, v, 3u));

  // Both edges are stored pointing away from the middle vertex, so a walk that
  // only followed out-lists would never leave v[0].
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[2], v[1], 0.0, NULL));

  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 3u);
  ASSERT_EQ_U64(w.order[0], v[0].index);
  ASSERT_EQ_U64(w.order[1], v[1].index);
  ASSERT_EQ_U64(w.order[2], v[2].index);
  ASSERT_EQ_U64(w.depth[2], 2u);

  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_dfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 3u);

  dv_graph_destroy(&g);
  return true;
}

static bool test_reachability(void) {
  dv_graph_t g;
  dv_vertex_t v[6];
  ASSERT_TRUE(build_walk_graph(&g, v, true));

  bool reached = false;
  ASSERT_OK(dv_graph_reachable(&g, v[0], v[4], &reached));
  ASSERT_TRUE(reached);

  ASSERT_OK(dv_graph_reachable(&g, v[4], v[0], &reached));
  ASSERT_FALSE(reached);

  ASSERT_OK(dv_graph_reachable(&g, v[0], v[5], &reached));
  ASSERT_FALSE(reached);

  ASSERT_OK(dv_graph_reachable(&g, v[5], v[5], &reached));
  ASSERT_TRUE(reached); // a vertex reaches itself in zero hops

  ASSERT_STATUS(dv_graph_reachable(&g, v[0], v[4], NULL),
                DV_GRAPH_ERR_INVALID);
  dv_graph_destroy(&g);

  // The same shape undirected reaches back the other way.
  ASSERT_TRUE(build_walk_graph(&g, v, false));
  ASSERT_OK(dv_graph_reachable(&g, v[4], v[0], &reached));
  ASSERT_TRUE(reached);
  ASSERT_OK(dv_graph_reachable(&g, v[4], v[5], &reached));
  ASSERT_FALSE(reached);

  dv_graph_destroy(&g);
  return true;
}

// --- Capacity ----------------------------------------------------------

static bool test_fixed_capacity_boundary(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, false);
  config.initial_vertex_capacity = 2u;
  config.initial_edge_capacity = 1u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[2];
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_STATUS(dv_graph_add_vertex(&g, NULL), DV_GRAPH_ERR_FULL);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 2u);

  dv_edge_t e = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, &e));
  ASSERT_STATUS(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL),
                DV_GRAPH_ERR_FULL);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_STATUS(dv_graph_reserve_vertices(&g, 3u), DV_GRAPH_ERR_FULL);
  ASSERT_STATUS(dv_graph_reserve_edges(&g, 2u), DV_GRAPH_ERR_FULL);
  ASSERT_OK(dv_graph_reserve_vertices(&g, 2u)); // already has the room
  ASSERT_OK(dv_graph_shrink_to_fit(&g));        // nothing to give back
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 2u);

  // A released slot is reused rather than counted against the cap again.
  ASSERT_OK(dv_graph_remove_edge(&g, e));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL));
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 1u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_growth_and_ceiling(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.initial_vertex_capacity = 0u;
  config.initial_edge_capacity = 0u;
  config.max_vertex_capacity = 3u;
  config.max_edge_capacity = 2u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 0u);

  dv_vertex_t v[3];
  ASSERT_TRUE(add_vertices(&g, v, 3u));
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 3u); // clamped to the ceiling
  ASSERT_STATUS(dv_graph_add_vertex(&g, NULL), DV_GRAPH_ERR_FULL);

  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[2], 0.0, NULL));
  ASSERT_STATUS(dv_graph_add_edge(&g, v[2], v[0], 0.0, NULL),
                DV_GRAPH_ERR_FULL);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), 2u);
  ASSERT_STATUS(dv_graph_reserve_edges(&g, 3u), DV_GRAPH_ERR_FULL);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);

  // Without a ceiling the pools follow the growth curve.
  config.max_vertex_capacity = 0u;
  config.max_edge_capacity = 0u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  for (uint32_t i = 0u; i < 100u; ++i)
    ASSERT_OK(dv_graph_add_vertex(&g, NULL));

  ASSERT_TRUE(dv_graph_vertex_capacity(&g) >= 100u);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 100u);
  ASSERT_STATUS(dv_graph_reserve_vertices(&g, DV_GRAPH_MAX_CAPACITY + 1u),
                DV_GRAPH_ERR_OVERFLOW);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

static bool test_reserve_shrink_clear_reset(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.initial_vertex_capacity = 4u;
  config.initial_edge_capacity = 4u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  ASSERT_OK(dv_graph_reserve_vertices(&g, 100u));
  ASSERT_OK(dv_graph_reserve_edges(&g, 100u));
  ASSERT_TRUE(dv_graph_vertex_capacity(&g) >= 100u);
  ASSERT_TRUE(dv_graph_edge_capacity(&g) >= 100u);

  dv_vertex_t v[5];
  ASSERT_TRUE(add_vertices(&g, v, 5u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[2], 0.0, NULL));

  // Shrink comes back to the high-water mark, not to the live count: the
  // handles that name those slots have to keep working.
  ASSERT_OK(dv_graph_shrink_to_fit(&g));
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 5u);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), 2u);
  ASSERT_TRUE(dv_graph_vertex_valid(&g, v[4]));
  ASSERT_GRAPH_VALID(&g);

  // Clear keeps the pools and invalidates every outstanding handle.
  dv_graph_clear(&g);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 0u);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 0u);
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 5u);
  ASSERT_FALSE(dv_graph_vertex_valid(&g, v[0]));
  ASSERT_GRAPH_VALID(&g);

  dv_vertex_t again = dv_vertex_none();
  ASSERT_OK(dv_graph_add_vertex(&g, &again));
  ASSERT_FALSE(dv_vertex_eq(again, v[0]));
  ASSERT_GRAPH_VALID(&g);

  // Reset additionally returns the pools to their configured capacities.
  ASSERT_OK(dv_graph_reset(&g));
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 0u);
  ASSERT_EQ_U64(dv_graph_vertex_capacity(&g), 4u);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), 4u);
  ASSERT_GRAPH_VALID(&g);
  ASSERT_TRUE(add_vertices(&g, v, 4u));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

// --- Bulk --------------------------------------------------------------

static bool test_bulk_operations(void) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.initial_vertex_capacity = 0u;
  config.initial_edge_capacity = 0u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  dv_vertex_t v[10];
  const uint64_t before = dv_graph_version(&g);
  ASSERT_EQ_U64(dv_graph_add_vertices(&g, 10u, v), 10u);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 10u);

  // A batch is one version step, not ten.
  ASSERT_EQ_U64(dv_graph_version(&g), before + 1u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_edge_spec_t specs[9];
  dv_edge_t edges[9];
  for (uint32_t i = 0u; i < 9u; ++i) {
    specs[i].src = v[i];
    specs[i].dst = v[i + 1u];
    specs[i].weight = (double)i;
  }

  const uint64_t mid = dv_graph_version(&g);
  ASSERT_EQ_U64(dv_graph_add_edges(&g, specs, 9u, edges), 9u);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 9u);
  ASSERT_EQ_U64(dv_graph_version(&g), mid + 1u);
  ASSERT_GRAPH_VALID(&g);

  // A rejected spec stops the batch where it is, and reports how far it got.
  ASSERT_OK(dv_graph_remove_vertex(&g, v[5]));
  specs[0].src = v[0];
  specs[0].dst = v[1];
  specs[1].src = v[5]; // now stale
  specs[1].dst = v[2];
  ASSERT_EQ_U64(dv_graph_add_edges(&g, specs, 3u, NULL), 1u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_EQ_U64(dv_graph_add_vertices(&g, 0u, NULL), 0u);
  ASSERT_EQ_U64(dv_graph_add_edges(&g, NULL, 3u, NULL), 0u);
  ASSERT_EQ_U64(dv_graph_add_vertices(NULL, 3u, NULL), 0u);

  dv_graph_destroy(&g);

  // A fixed graph fills what it can and reports the partial count.
  config.growable = false;
  config.initial_vertex_capacity = 4u;
  config.initial_edge_capacity = 2u;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));
  ASSERT_EQ_U64(dv_graph_add_vertices(&g, 10u, v), 4u);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 4u);

  for (uint32_t i = 0u; i < 3u; ++i) {
    specs[i].src = v[i];
    specs[i].dst = v[i + 1u];
    specs[i].weight = 0.0;
  }
  ASSERT_EQ_U64(dv_graph_add_edges(&g, specs, 3u, NULL), 2u);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  return true;
}

// --- Allocator ---------------------------------------------------------

// Exercises every block the graph owns -- both pools, the weights and the
// traversal scratch -- and then checks that all of it came back.
static bool exercise_allocator(bool with_realloc) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_graph_allocator_t hooks = tracking_hooks(&st, with_realloc);

  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.weighted = true;
  config.initial_vertex_capacity = 2u;
  config.initial_edge_capacity = 2u;
  ASSERT_OK(dv_graph_init(&g, &config, &hooks));

  dv_vertex_t v[32];
  ASSERT_TRUE(add_vertices(&g, v, 32u));
  for (uint32_t i = 1u; i < 32u; ++i)
    ASSERT_OK(dv_graph_add_edge(&g, v[i - 1u], v[i], (double)i, NULL));

  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 32u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_TRUE(st.alloc_calls > 0u);
  ASSERT_TRUE(st.live_bytes > 0u);
  ASSERT_TRUE(st.align_respected);
  if (with_realloc)
    ASSERT_TRUE(st.realloc_calls > 0u);
  else
    ASSERT_EQ_U64(st.realloc_calls, 0u);

  ASSERT_OK(dv_graph_shrink_to_fit(&g));
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

static bool test_allocator_accounting_with_realloc(void) {
  return exercise_allocator(true);
}

static bool test_allocator_accounting_without_realloc(void) {
  return exercise_allocator(false);
}

static bool test_allocation_failure_is_recoverable(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_graph_allocator_t hooks = tracking_hooks(&st, true);

  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.initial_vertex_capacity = 2u;
  config.initial_edge_capacity = 2u;

  // Two blocks for init, and nothing left for the first growth.
  st.budget = 2u;
  ASSERT_OK(dv_graph_init(&g, &config, &hooks));

  dv_vertex_t v[2];
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_STATUS(dv_graph_add_vertex(&g, NULL), DV_GRAPH_ERR_NOMEM);
  ASSERT_EQ_U64(dv_graph_vertex_count(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 0.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 0.0, NULL));
  ASSERT_STATUS(dv_graph_add_edge(&g, v[0], v[0], 0.0, NULL),
                DV_GRAPH_ERR_NOMEM);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  // A traversal that cannot size its scratch fails the same way, and leaves
  // the graph alone.
  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_STATUS(dv_graph_bfs(&g, v[0], walk_visit, &w), DV_GRAPH_ERR_NOMEM);
  ASSERT_EQ_U64(w.count, 0u);
  ASSERT_GRAPH_VALID(&g);

  // With the budget restored the same calls go through.
  st.budget = SIZE_MAX;
  ASSERT_OK(dv_graph_add_vertex(&g, NULL));
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));
  ASSERT_EQ_U64(w.count, 2u);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// The weights array is resized before the edge pool grows, so a refusal in
// either half leaves the array covering the pool. The reverse order would hand
// out an edge slot with no weight behind it.
static bool test_weighted_growth_partial_failure(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_graph_allocator_t hooks = tracking_hooks(&st, true);

  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.weighted = true;
  config.initial_vertex_capacity = 2u;
  config.initial_edge_capacity = 2u;

  // Exactly the three blocks init needs: the two pools and the weights.
  st.budget = 3u;
  ASSERT_OK(dv_graph_init(&g, &config, &hooks));

  dv_vertex_t v[2];
  ASSERT_TRUE(add_vertices(&g, v, 2u));
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[1], 1.0, NULL));
  ASSERT_OK(dv_graph_add_edge(&g, v[1], v[0], 2.0, NULL));

  // The weights resize is refused, so the pool never grows.
  ASSERT_STATUS(dv_graph_add_edge(&g, v[0], v[0], 3.0, NULL),
                DV_GRAPH_ERR_NOMEM);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  // Now the weights go through and the pool is refused: the array is left
  // larger than the pool, which is wasteful and safe.
  st.budget = 1u;
  ASSERT_STATUS(dv_graph_add_edge(&g, v[0], v[0], 3.0, NULL),
                DV_GRAPH_ERR_NOMEM);
  ASSERT_EQ_U64(dv_graph_edge_count(&g), 2u);
  ASSERT_EQ_U64(dv_graph_edge_capacity(&g), 2u);
  ASSERT_GRAPH_VALID(&g);

  // With the budget restored the pool catches up to the array already sized
  // for it, and nothing that was stored earlier moved.
  st.budget = SIZE_MAX;
  dv_edge_t e = dv_edge_none();
  ASSERT_OK(dv_graph_add_edge(&g, v[0], v[0], 3.5, &e));
  ASSERT_TRUE(dv_graph_edge_capacity(&g) > 2u);

  double weight = 0.0;
  ASSERT_OK(dv_graph_edge_weight(&g, e, &weight));
  ASSERT_TRUE(weight == 3.5);

  dv_graph_edge_iter_t it;
  dv_graph_edge_ref_t ref;
  ASSERT_OK(dv_graph_out_edges_begin(&g, v[0], &it));
  ASSERT_TRUE(dv_graph_edge_iter_next(&it, &ref));
  ASSERT_TRUE(ref.weight == 1.0);
  ASSERT_GRAPH_VALID(&g);

  dv_graph_destroy(&g);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

static bool test_fixed_capacity_never_allocates(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_graph_allocator_t hooks = tracking_hooks(&st, true);

  dv_graph_t g;
  dv_graph_config_t config = base_config(true, false);
  config.weighted = true;
  config.initial_vertex_capacity = 16u;
  config.initial_edge_capacity = 32u;
  ASSERT_OK(dv_graph_init(&g, &config, &hooks));

  // Everything the graph will ever need, including the traversal scratch, was
  // taken during init. That is the whole promise of fixed capacity.
  const size_t allocs = st.alloc_calls;
  const size_t reallocs = st.realloc_calls;
  const size_t frees = st.free_calls;

  dv_vertex_t v[16];
  ASSERT_TRUE(add_vertices(&g, v, 16u));
  for (uint32_t i = 1u; i < 16u; ++i)
    ASSERT_OK(dv_graph_add_edge(&g, v[i - 1u], v[i], (double)i, NULL));

  walk_t w;
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_bfs(&g, v[0], walk_visit, &w));
  memset(&w, 0, sizeof(w));
  ASSERT_OK(dv_graph_dfs(&g, v[0], walk_visit, &w));

  ASSERT_OK(dv_graph_remove_vertex(&g, v[8]));
  ASSERT_OK(dv_graph_add_vertex(&g, NULL));
  dv_graph_clear(&g);
  ASSERT_OK(dv_graph_reserve_traversal(&g));
  ASSERT_GRAPH_VALID(&g);

  ASSERT_EQ_U64(st.alloc_calls, allocs);
  ASSERT_EQ_U64(st.realloc_calls, reallocs);
  ASSERT_EQ_U64(st.free_calls, frees);

  dv_graph_destroy(&g);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

static bool test_cache_aligned_pools(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_graph_allocator_t hooks = tracking_hooks(&st, true);

  dv_graph_t g;
  dv_graph_config_t config = base_config(true, true);
  config.cache_aligned = true;
  ASSERT_OK(dv_graph_init(&g, &config, &hooks));

  ASSERT_TRUE(st.last_align >= DV_CACHE_LINE_SIZE);
  ASSERT_TRUE(st.align_respected);
  ASSERT_EQ_U64((uintptr_t)g.vertices % DV_CACHE_LINE_SIZE, 0u);
  ASSERT_EQ_U64((uintptr_t)g.edges % DV_CACHE_LINE_SIZE, 0u);

  dv_graph_destroy(&g);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// --- Randomized differential -------------------------------------------

typedef struct {
  dv_vertex_t src;
  dv_vertex_t dst;
  dv_edge_t handle;
} model_edge_t;

typedef struct {
  dv_vertex_t vertices[DIFF_VERTICES];
  model_edge_t edges[DIFF_EDGES];
  uint32_t vertex_count;
  uint32_t edge_count;
} model_t;

static void model_drop_edge(model_t *m, uint32_t index) {
  m->edges[index] = m->edges[--m->edge_count];
}

static void model_drop_vertex(model_t *m, uint32_t index) {
  const dv_vertex_t gone = m->vertices[index];

  for (uint32_t i = m->edge_count; i-- > 0u;) {
    if (dv_vertex_eq(m->edges[i].src, gone) ||
        dv_vertex_eq(m->edges[i].dst, gone))
      model_drop_edge(m, i);
  }

  m->vertices[index] = m->vertices[--m->vertex_count];
}

// Compares the graph's own view of one vertex against the model's: recorded
// degrees, and the multiset of edges an incident walk yields.
static bool model_check_vertex(const dv_graph_t *g, const model_t *m,
                               dv_vertex_t vertex) {
  uint32_t out_degree = 0u;
  uint32_t in_degree = 0u;

  for (uint32_t i = 0u; i < m->edge_count; ++i) {
    if (dv_vertex_eq(m->edges[i].src, vertex))
      ++out_degree;
    if (dv_vertex_eq(m->edges[i].dst, vertex))
      ++in_degree;
  }

  ASSERT_EQ_U64(dv_graph_out_degree(g, vertex), out_degree);
  ASSERT_EQ_U64(dv_graph_in_degree(g, vertex), in_degree);
  ASSERT_EQ_U64(dv_graph_degree(g, vertex), out_degree + in_degree);

  dv_graph_edge_iter_t it;
  ASSERT_OK(dv_graph_edges_begin(g, vertex, &it));

  dv_graph_edge_ref_t ref;
  uint32_t walked = 0u;
  while (dv_graph_edge_iter_next(&it, &ref)) {
    ++walked;

    bool matched = false;
    for (uint32_t i = 0u; i < m->edge_count && !matched; ++i)
      matched = dv_edge_eq(m->edges[i].handle, ref.edge);

    ASSERT_TRUE(matched);
    ASSERT_TRUE(dv_vertex_eq(ref.src, vertex) || dv_vertex_eq(ref.dst, vertex));
  }

  ASSERT_EQ_U64(walked, out_degree + in_degree);
  return true;
}

static bool run_differential(bool directed, uint64_t seed) {
  dv_graph_t g;
  dv_graph_config_t config = base_config(directed, true);
  config.weighted = true;
  config.initial_vertex_capacity = 4u;
  config.initial_edge_capacity = 4u;
  config.max_vertex_capacity = DIFF_VERTICES;
  config.max_edge_capacity = DIFF_EDGES;
  ASSERT_OK(dv_graph_init(&g, &config, NULL));

  model_t m;
  memset(&m, 0, sizeof(m));

  uint64_t rng = seed;
  for (uint32_t op = 0u; op < DIFF_OPS; ++op) {
    const uint32_t roll = (uint32_t)(xorshift64(&rng) % 100u);

    if (roll < 35u) {
      if (m.vertex_count < DIFF_VERTICES) {
        dv_vertex_t added = dv_vertex_none();
        ASSERT_OK(dv_graph_add_vertex(&g, &added));
        m.vertices[m.vertex_count++] = added;
      }
    } else if (roll < 45u) {
      if (m.vertex_count > 0u) {
        const uint32_t pick =
            (uint32_t)(xorshift64(&rng) % m.vertex_count);
        ASSERT_OK(dv_graph_remove_vertex(&g, m.vertices[pick]));
        model_drop_vertex(&m, pick);
      }
    } else if (roll < 85u) {
      if (m.vertex_count > 0u && m.edge_count < DIFF_EDGES) {
        const uint32_t a = (uint32_t)(xorshift64(&rng) % m.vertex_count);
        const uint32_t b = (uint32_t)(xorshift64(&rng) % m.vertex_count);

        dv_edge_t added = dv_edge_none();
        ASSERT_OK(dv_graph_add_edge(&g, m.vertices[a], m.vertices[b],
                                    (double)op, &added));

        m.edges[m.edge_count].src = m.vertices[a];
        m.edges[m.edge_count].dst = m.vertices[b];
        m.edges[m.edge_count].handle = added;
        ++m.edge_count;
      }
    } else if (roll < 95u) {
      if (m.edge_count > 0u) {
        const uint32_t pick = (uint32_t)(xorshift64(&rng) % m.edge_count);
        ASSERT_OK(dv_graph_remove_edge(&g, m.edges[pick].handle));
        model_drop_edge(&m, pick);
      }
    } else if (m.vertex_count > 0u) {
      const uint32_t pick = (uint32_t)(xorshift64(&rng) % m.vertex_count);
      if (!model_check_vertex(&g, &m, m.vertices[pick]))
        return false;
    }

    ASSERT_EQ_U64(dv_graph_vertex_count(&g), m.vertex_count);
    ASSERT_EQ_U64(dv_graph_edge_count(&g), m.edge_count);

    // The full O(V + E) invariant sweep is too heavy for every operation, so
    // it runs often enough to bracket any single one.
    if ((op & 63u) == 0u)
      ASSERT_GRAPH_VALID(&g);
  }

  ASSERT_GRAPH_VALID(&g);

  for (uint32_t i = 0u; i < m.vertex_count; ++i) {
    if (!model_check_vertex(&g, &m, m.vertices[i]))
      return false;
  }

  dv_graph_destroy(&g);
  return true;
}

static bool test_randomized_differential_directed(void) {
  return run_differential(true, 0x9E3779B97F4A7C15ull);
}

static bool test_randomized_differential_undirected(void) {
  return run_differential(false, 0xD1B54A32D192ED03ull);
}

// --- Mutex wrapper -----------------------------------------------------

typedef struct {
  dv_graph_mt_t *graph;
  uint32_t edges;
  bool ok;
} mt_worker_t;

static void *mt_worker(void *arg) {
  mt_worker_t *w = (mt_worker_t *)arg;

  dv_vertex_t a = dv_vertex_none();
  dv_vertex_t b = dv_vertex_none();
  if (dv_graph_mt_add_vertex(w->graph, &a) != DV_GRAPH_OK ||
      dv_graph_mt_add_vertex(w->graph, &b) != DV_GRAPH_OK) {
    w->ok = false;
    return NULL;
  }

  for (uint32_t i = 0u; i < w->edges; ++i) {
    if (dv_graph_mt_add_edge(w->graph, a, b, (double)i, NULL) !=
        DV_GRAPH_OK) {
      w->ok = false;
      return NULL;
    }
  }

  // Each thread's own subgraph is its own to check, and the counts it reads
  // are stable because nothing else touches its two vertices.
  w->ok = dv_graph_mt_degree(w->graph, a) == w->edges &&
          dv_graph_mt_has_edge(w->graph, a, b);
  return NULL;
}

static void mt_check_valid(const dv_graph_t *g, void *user_data) {
  bool *ok = (bool *)user_data;
  *ok = dv_graph_is_valid(g);
}

static bool test_mt_wrapper_conserves_topology(void) {
  dv_graph_mt_t graph;
  dv_graph_config_t config = base_config(true, true);
  config.initial_vertex_capacity = 0u;
  config.initial_edge_capacity = 0u;
  ASSERT_OK(dv_graph_mt_init(&graph, &config, NULL));

  pthread_t threads[MT_THREADS];
  mt_worker_t workers[MT_THREADS];

  for (uint32_t i = 0u; i < MT_THREADS; ++i) {
    workers[i].graph = &graph;
    workers[i].edges = MT_EDGES_PER_THREAD;
    workers[i].ok = true;
    ASSERT_EQ_U64(pthread_create(&threads[i], NULL, mt_worker, &workers[i]),
                  0u);
  }

  for (uint32_t i = 0u; i < MT_THREADS; ++i) {
    ASSERT_EQ_U64(pthread_join(threads[i], NULL), 0u);
    ASSERT_TRUE(workers[i].ok);
  }

  ASSERT_EQ_U64(dv_graph_mt_vertex_count(&graph), MT_THREADS * 2u);
  ASSERT_EQ_U64(dv_graph_mt_edge_count(&graph),
                MT_THREADS * MT_EDGES_PER_THREAD);

  bool valid = false;
  ASSERT_OK(dv_graph_mt_read(&graph, mt_check_valid, &valid));
  ASSERT_TRUE(valid);

  dv_graph_mt_clear(&graph);
  ASSERT_EQ_U64(dv_graph_mt_vertex_count(&graph), 0u);
  ASSERT_EQ_U64(dv_graph_mt_edge_count(&graph), 0u);

  dv_graph_mt_destroy(&graph);
  return true;
}

typedef struct {
  dv_vertex_t path[4];
  uint32_t reached;
  bool ok;
} mt_section_t;

// One critical section: build a path and read it back. Split across separate
// wrapper calls the handles could be stale by the time they are used.
static void mt_build_path(dv_graph_t *g, void *user_data) {
  mt_section_t *s = (mt_section_t *)user_data;

  s->ok = true;
  for (uint32_t i = 0u; i < 4u; ++i)
    s->ok = s->ok && dv_graph_add_vertex(g, &s->path[i]) == DV_GRAPH_OK;

  for (uint32_t i = 1u; i < 4u && s->ok; ++i)
    s->ok = dv_graph_add_edge(g, s->path[i - 1u], s->path[i], 0.0, NULL) ==
            DV_GRAPH_OK;
}

static bool mt_count_visit(dv_vertex_t vertex, uint32_t depth,
                           void *user_data) {
  mt_section_t *s = (mt_section_t *)user_data;
  (void)vertex;
  (void)depth;

  ++s->reached;
  return true;
}

static bool test_mt_critical_section(void) {
  dv_graph_mt_t graph;
  const dv_graph_config_t config = base_config(true, true);
  ASSERT_OK(dv_graph_mt_init(&graph, &config, NULL));

  mt_section_t section;
  memset(&section, 0, sizeof(section));
  ASSERT_OK(dv_graph_mt_write(&graph, mt_build_path, &section));
  ASSERT_TRUE(section.ok);
  ASSERT_EQ_U64(dv_graph_mt_vertex_count(&graph), 4u);
  ASSERT_EQ_U64(dv_graph_mt_edge_count(&graph), 3u);

  dv_edge_t found = dv_edge_none();
  ASSERT_OK(dv_graph_mt_find_edge(&graph, section.path[0], section.path[1],
                                  &found));
  ASSERT_TRUE(dv_graph_edge_valid(&graph.graph, found));

  bool reached = false;
  ASSERT_OK(dv_graph_mt_reachable(&graph, section.path[0], section.path[3],
                                  &reached));
  ASSERT_TRUE(reached);
  ASSERT_OK(dv_graph_mt_reachable(&graph, section.path[3], section.path[0],
                                  &reached));
  ASSERT_FALSE(reached);

  ASSERT_OK(dv_graph_mt_bfs(&graph, section.path[0], mt_count_visit,
                            &section));
  ASSERT_EQ_U64(section.reached, 4u);

  ASSERT_OK(dv_graph_mt_remove_edge(&graph, found));
  ASSERT_OK(dv_graph_mt_remove_vertex(&graph, section.path[3]));
  ASSERT_EQ_U64(dv_graph_mt_vertex_count(&graph), 3u);
  ASSERT_EQ_U64(dv_graph_mt_edge_count(&graph), 1u);

  ASSERT_STATUS(dv_graph_mt_write(&graph, NULL, NULL), DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_mt_read(&graph, NULL, NULL), DV_GRAPH_ERR_INVALID);
  ASSERT_STATUS(dv_graph_mt_add_vertex(NULL, NULL), DV_GRAPH_ERR_INVALID);
  ASSERT_EQ_U64(dv_graph_mt_vertex_count(NULL), 0u);

  dv_graph_mt_destroy(&graph);
  return true;
}

int main(void) {
  const test_case_t tests[] = {
      {"init rejects bad config", test_init_rejects_bad_config},
      {"empty graph accessors", test_empty_graph_accessors},
      {"status strings", test_status_strings},
      {"vertex lifecycle and handle reuse", test_vertex_lifecycle},
      {"vertex iterator", test_vertex_iterator},
      {"directed edges", test_directed_edges},
      {"undirected edges", test_undirected_edges},
      {"self loops", test_self_loops},
      {"parallel edges", test_parallel_edges},
      {"remove vertex drops incident edges",
       test_remove_vertex_drops_incident_edges},
      {"weights", test_weights},
      {"iteration is insertion ordered", test_iteration_is_insertion_ordered},
      {"iterator invalidation and update phase", test_iterator_invalidation},
      {"in and out iterators", test_in_and_out_iterators},
      {"bfs order and depth", test_bfs_order_and_depth},
      {"dfs order", test_dfs_order},
      {"undirected traversal follows both lists",
       test_undirected_traversal_follows_both_lists},
      {"reachability", test_reachability},
      {"fixed capacity boundary", test_fixed_capacity_boundary},
      {"growth and ceiling", test_growth_and_ceiling},
      {"reserve shrink clear reset", test_reserve_shrink_clear_reset},
      {"bulk operations", test_bulk_operations},
      {"allocator accounting with realloc",
       test_allocator_accounting_with_realloc},
      {"allocator accounting without realloc",
       test_allocator_accounting_without_realloc},
      {"allocation failure is recoverable",
       test_allocation_failure_is_recoverable},
      {"weighted growth partial failure",
       test_weighted_growth_partial_failure},
      {"fixed capacity never allocates", test_fixed_capacity_never_allocates},
      {"cache aligned pools", test_cache_aligned_pools},
      {"randomized differential directed",
       test_randomized_differential_directed},
      {"randomized differential undirected",
       test_randomized_differential_undirected},
      {"mt wrapper conserves topology", test_mt_wrapper_conserves_topology},
      {"mt critical section", test_mt_critical_section},
  };

  const size_t ntests = sizeof(tests) / sizeof(tests[0]);

  for (size_t i = 0u; i < ntests; ++i) {
    printf("[TEST] %s\n", tests[i].name);
    if (!tests[i].fn()) {
      fprintf(stderr, "[FAIL] %s\n", tests[i].name);
      return EXIT_FAILURE;
    }
    printf("[PASS] %s\n", tests[i].name);
  }

  printf("All %zu tests passed.\n", ntests);
  return EXIT_SUCCESS;
}
