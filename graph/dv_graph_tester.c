// dv_graph_tester.c - soak harness for the graph module.
//
// Each phase drives one behaviour past the scale the unit tests sample: a
// full-capacity build and teardown, traversal over shapes with a known answer,
// a long churn of mutations checked against a running model, growth and
// shrinkage with every byte accounted for, bulk ingestion against the
// one-at-a-time path, and the wrapper under contention. Every phase reports
// independently and the run fails if any one of them does.

#include "dv_graph.h"
#include "dv_graph_mt.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GRAPH_VERTICES
#define GRAPH_VERTICES 65536u
#endif

#ifndef GRAPH_EDGES
#define GRAPH_EDGES 524288u
#endif

#ifndef GRAPH_CYCLES
#define GRAPH_CYCLES 8u
#endif

#ifndef GRAPH_THREADS
#define GRAPH_THREADS 8u
#endif

#ifndef GRAPH_SEED
#define GRAPH_SEED 0x9E3779B97F4A7C15ull
#endif

#ifndef BATCH_SIZE
#define BATCH_SIZE 64u
#endif

#define CHURN_VERTICES 2048u
#define CHURN_EDGES 8192u
#define CHURN_OPS 100000u

// --- Accounting allocator ----------------------------------------------

typedef struct {
  size_t live_bytes;
  size_t peak_bytes;
  size_t alloc_calls;
  size_t realloc_calls;
  size_t free_calls;
  bool align_respected;
} account_t;

static void *account_raw_alloc(size_t size, size_t align) {
  const size_t padded = (size + align - 1u) & ~(align - 1u);
  return aligned_alloc(align, padded);
}

static void *account_alloc(size_t size, size_t align, void *user_data) {
  account_t *a = (account_t *)user_data;

  void *mem = account_raw_alloc(size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    a->align_respected = false;

  ++a->alloc_calls;
  a->live_bytes += size;
  if (a->live_bytes > a->peak_bytes)
    a->peak_bytes = a->live_bytes;

  return mem;
}

static void *account_realloc(void *ptr, size_t old_size, size_t new_size,
                             size_t align, void *user_data) {
  account_t *a = (account_t *)user_data;

  void *mem = account_raw_alloc(new_size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    a->align_respected = false;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);
  free(ptr);

  ++a->realloc_calls;
  a->live_bytes -= old_size;
  a->live_bytes += new_size;
  if (a->live_bytes > a->peak_bytes)
    a->peak_bytes = a->live_bytes;

  return mem;
}

static void account_free(void *ptr, size_t size, size_t align,
                         void *user_data) {
  account_t *a = (account_t *)user_data;
  (void)align;

  if (!ptr)
    return;

  ++a->free_calls;
  a->live_bytes -= size;
  free(ptr);
}

static dv_graph_allocator_t account_hooks(account_t *a) {
  memset(a, 0, sizeof(*a));
  a->align_respected = true;

  const dv_graph_allocator_t hooks = {
      .alloc = account_alloc,
      .realloc = account_realloc,
      .free = account_free,
      .user_data = a,
  };
  return hooks;
}

// --- Shared helpers ----------------------------------------------------

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static void fail_pthread(int err, const char *what) {
  if (err == 0)
    return;

  fprintf(stderr, "%s failed: %s\n", what, strerror(err));
  exit(EXIT_FAILURE);
}

static void report(const char *phase, bool ok) {
  printf("\t%-28s %s\n", phase, ok ? "ok" : "FAILED");
}

static dv_graph_config_t graph_config(bool directed, uint32_t vertices,
                                      uint32_t edges, bool growable) {
  const dv_graph_config_t config = {
      .directed = directed,
      .weighted = true,
      .growable = growable,
      .cache_aligned = true,
      .reject_parallel_edges = false,
      .reject_self_loops = false,
      .initial_vertex_capacity = vertices,
      .initial_edge_capacity = edges,
      .max_vertex_capacity = 0u,
      .max_edge_capacity = 0u,
  };
  return config;
}

// --- Phase 1: build and tear down at capacity --------------------------

// Fills a fixed graph to both caps, checks that the caps hold, then removes
// every vertex and requires the edge pool to have emptied with them.
static bool phase_build_teardown(void) {
  account_t account;
  const dv_graph_allocator_t hooks = account_hooks(&account);

  dv_graph_t g;
  const dv_graph_config_t config =
      graph_config(true, GRAPH_VERTICES, GRAPH_EDGES, false);
  if (dv_graph_init(&g, &config, &hooks) != DV_GRAPH_OK)
    return false;

  dv_vertex_t *vertices =
      (dv_vertex_t *)malloc(GRAPH_VERTICES * sizeof(*vertices));
  if (!vertices) {
    dv_graph_destroy(&g);
    return false;
  }

  uint64_t rng = GRAPH_SEED;
  bool ok = true;

  for (uint32_t cycle = 0u; cycle < GRAPH_CYCLES && ok; ++cycle) {
    for (uint32_t i = 0u; i < GRAPH_VERTICES && ok; ++i)
      ok = dv_graph_add_vertex(&g, &vertices[i]) == DV_GRAPH_OK;

    ok = ok && dv_graph_add_vertex(&g, NULL) == DV_GRAPH_ERR_FULL;
    ok = ok && dv_graph_vertex_count(&g) == GRAPH_VERTICES;

    for (uint32_t i = 0u; i < GRAPH_EDGES && ok; ++i) {
      const uint32_t a = (uint32_t)(xorshift64(&rng) % GRAPH_VERTICES);
      const uint32_t b = (uint32_t)(xorshift64(&rng) % GRAPH_VERTICES);
      ok = dv_graph_add_edge(&g, vertices[a], vertices[b], (double)i, NULL) ==
           DV_GRAPH_OK;
    }

    ok = ok && dv_graph_add_edge(&g, vertices[0], vertices[1], 0.0, NULL) ==
                   DV_GRAPH_ERR_FULL;
    ok = ok && dv_graph_edge_count(&g) == GRAPH_EDGES;
    ok = ok && dv_graph_is_valid(&g);

    // Removing the vertices has to take every edge with them: the edge pool
    // ends empty without a single explicit edge removal.
    for (uint32_t i = 0u; i < GRAPH_VERTICES && ok; ++i)
      ok = dv_graph_remove_vertex(&g, vertices[i]) == DV_GRAPH_OK;

    ok = ok && dv_graph_vertex_count(&g) == 0u;
    ok = ok && dv_graph_edge_count(&g) == 0u;
    ok = ok && dv_graph_is_valid(&g);
  }

  // A fixed graph never reaches for the allocator after init: three blocks for
  // the pools and the weights, two more for the traversal scratch.
  ok = ok && account.alloc_calls == 5u;
  ok = ok && account.realloc_calls == 0u;
  ok = ok && account.align_respected;

  free(vertices);
  dv_graph_destroy(&g);

  ok = ok && account.live_bytes == 0u;
  return ok;
}

// --- Phase 2: traversal over known shapes ------------------------------

typedef struct {
  uint32_t *expect_depth; // indexed by vertex slot
  uint32_t visits;
  uint32_t previous_depth;
  bool ok;
} depth_check_t;

static bool depth_visit(dv_vertex_t vertex, uint32_t depth, void *user_data) {
  depth_check_t *check = (depth_check_t *)user_data;

  // Breadth-first order is non-decreasing in depth, and every vertex arrives
  // at the distance the shape says it should.
  if (depth < check->previous_depth ||
      check->expect_depth[vertex.index] != depth)
    check->ok = false;

  check->previous_depth = depth;
  ++check->visits;
  return true;
}

static bool count_visit(dv_vertex_t vertex, uint32_t depth, void *user_data) {
  uint32_t *count = (uint32_t *)user_data;
  (void)vertex;
  (void)depth;

  ++*count;
  return true;
}

// A directed path of N vertices, plus a second component of the same size that
// nothing in the first can reach.
static bool phase_traversal_shapes(void) {
  const uint32_t n = GRAPH_VERTICES / 2u;

  dv_graph_t g;
  const dv_graph_config_t config =
      graph_config(true, 2u * n, 2u * n, false);
  if (dv_graph_init(&g, &config, NULL) != DV_GRAPH_OK)
    return false;

  dv_vertex_t *vertices = (dv_vertex_t *)malloc(2u * n * sizeof(*vertices));
  uint32_t *expect = (uint32_t *)malloc(2u * (size_t)n * sizeof(*expect));
  if (!vertices || !expect) {
    free(vertices);
    free(expect);
    dv_graph_destroy(&g);
    return false;
  }

  bool ok = true;
  for (uint32_t i = 0u; i < 2u * n && ok; ++i)
    ok = dv_graph_add_vertex(&g, &vertices[i]) == DV_GRAPH_OK;

  for (uint32_t i = 1u; i < n && ok; ++i) {
    ok = dv_graph_add_edge(&g, vertices[i - 1u], vertices[i], 1.0, NULL) ==
         DV_GRAPH_OK;
    ok = ok && dv_graph_add_edge(&g, vertices[n + i - 1u], vertices[n + i],
                                 1.0, NULL) == DV_GRAPH_OK;
  }

  for (uint32_t i = 0u; i < n; ++i)
    expect[vertices[i].index] = i;

  depth_check_t check = {expect, 0u, 0u, true};
  ok = ok && dv_graph_bfs(&g, vertices[0], depth_visit, &check) == DV_GRAPH_OK;
  ok = ok && check.ok && check.visits == n;

  // Depth-first covers the same component, and on a path in the same order.
  uint32_t seen = 0u;
  ok = ok && dv_graph_dfs(&g, vertices[0], count_visit, &seen) == DV_GRAPH_OK;
  ok = ok && seen == n;

  bool reached = true;
  ok = ok && dv_graph_reachable(&g, vertices[0], vertices[n - 1u], &reached) ==
                 DV_GRAPH_OK;
  ok = ok && reached;
  ok = ok && dv_graph_reachable(&g, vertices[0], vertices[n], &reached) ==
                 DV_GRAPH_OK;
  ok = ok && !reached;
  ok = ok && dv_graph_reachable(&g, vertices[n - 1u], vertices[0], &reached) ==
                 DV_GRAPH_OK;
  ok = ok && !reached;

  // The same path undirected reaches backwards, and the far end is the far
  // end from either direction.
  dv_graph_destroy(&g);

  const dv_graph_config_t undirected =
      graph_config(false, 2u * n, 2u * n, false);
  if (dv_graph_init(&g, &undirected, NULL) != DV_GRAPH_OK)
    ok = false;

  for (uint32_t i = 0u; i < n && ok; ++i)
    ok = dv_graph_add_vertex(&g, &vertices[i]) == DV_GRAPH_OK;

  // Alternating orientation, so a walk that ignored either list would stall.
  for (uint32_t i = 1u; i < n && ok; ++i) {
    const dv_vertex_t a = (i & 1u) ? vertices[i - 1u] : vertices[i];
    const dv_vertex_t b = (i & 1u) ? vertices[i] : vertices[i - 1u];
    ok = dv_graph_add_edge(&g, a, b, 1.0, NULL) == DV_GRAPH_OK;
  }

  for (uint32_t i = 0u; i < n; ++i)
    expect[vertices[i].index] = i;

  check.visits = 0u;
  check.previous_depth = 0u;
  check.ok = true;
  ok = ok && dv_graph_bfs(&g, vertices[0], depth_visit, &check) == DV_GRAPH_OK;
  ok = ok && check.ok && check.visits == n;

  ok = ok && dv_graph_reachable(&g, vertices[n - 1u], vertices[0], &reached) ==
                 DV_GRAPH_OK;
  ok = ok && reached;
  ok = ok && dv_graph_is_valid(&g);

  free(vertices);
  free(expect);
  dv_graph_destroy(&g);
  return ok;
}

// --- Phase 3: churn against a running model ----------------------------

// The model keeps stable slots so an edge can name its endpoints by slot and
// survive any number of removals elsewhere. Degrees are maintained as the
// operations happen, which is what makes the check O(1) per mutation.
typedef struct {
  dv_vertex_t handle;
  uint32_t out_degree;
  uint32_t in_degree;
  bool live;
} churn_vertex_t;

typedef struct {
  dv_edge_t handle;
  uint32_t src;
  uint32_t dst;
} churn_edge_t;

typedef struct {
  churn_vertex_t vertices[CHURN_VERTICES];
  uint32_t live[CHURN_VERTICES];
  uint32_t live_count;
  churn_edge_t edges[CHURN_EDGES];
  uint32_t edge_count;
  uint32_t free_slot;
} churn_model_t;

static void churn_drop_edge(churn_model_t *m, uint32_t index) {
  churn_edge_t *e = &m->edges[index];
  --m->vertices[e->src].out_degree;
  --m->vertices[e->dst].in_degree;

  m->edges[index] = m->edges[--m->edge_count];
}

static bool phase_churn(void) {
  dv_graph_t g;
  dv_graph_config_t config = graph_config(true, 16u, 16u, true);
  config.max_vertex_capacity = CHURN_VERTICES;
  config.max_edge_capacity = CHURN_EDGES;
  if (dv_graph_init(&g, &config, NULL) != DV_GRAPH_OK)
    return false;

  churn_model_t *m = (churn_model_t *)calloc(1u, sizeof(*m));
  if (!m) {
    dv_graph_destroy(&g);
    return false;
  }

  uint64_t rng = GRAPH_SEED ^ 0x5DEECE66Dull;
  bool ok = true;

  for (uint32_t op = 0u; op < CHURN_OPS && ok; ++op) {
    const uint32_t roll = (uint32_t)(xorshift64(&rng) % 100u);

    if (roll < 30u) {
      if (m->free_slot < CHURN_VERTICES) {
        const uint32_t slot = m->free_slot++;
        ok = dv_graph_add_vertex(&g, &m->vertices[slot].handle) == DV_GRAPH_OK;
        m->vertices[slot].live = true;
        m->vertices[slot].out_degree = 0u;
        m->vertices[slot].in_degree = 0u;
        m->live[m->live_count++] = slot;
      }
    } else if (roll < 32u) {
      if (m->live_count > 0u) {
        const uint32_t pick = (uint32_t)(xorshift64(&rng) % m->live_count);
        const uint32_t slot = m->live[pick];

        ok = dv_graph_remove_vertex(&g, m->vertices[slot].handle) ==
             DV_GRAPH_OK;

        for (uint32_t i = m->edge_count; i-- > 0u;) {
          if (m->edges[i].src == slot || m->edges[i].dst == slot)
            churn_drop_edge(m, i);
        }

        m->vertices[slot].live = false;
        m->live[pick] = m->live[--m->live_count];
      }
    } else if (roll < 80u) {
      if (m->live_count > 0u && m->edge_count < CHURN_EDGES) {
        const uint32_t a = m->live[xorshift64(&rng) % m->live_count];
        const uint32_t b = m->live[xorshift64(&rng) % m->live_count];

        dv_edge_t added = dv_edge_none();
        ok = dv_graph_add_edge(&g, m->vertices[a].handle,
                               m->vertices[b].handle, (double)op,
                               &added) == DV_GRAPH_OK;

        m->edges[m->edge_count].handle = added;
        m->edges[m->edge_count].src = a;
        m->edges[m->edge_count].dst = b;
        ++m->edge_count;
        ++m->vertices[a].out_degree;
        ++m->vertices[b].in_degree;
      }
    } else if (roll < 95u) {
      if (m->edge_count > 0u) {
        const uint32_t pick = (uint32_t)(xorshift64(&rng) % m->edge_count);
        ok = dv_graph_remove_edge(&g, m->edges[pick].handle) == DV_GRAPH_OK;
        churn_drop_edge(m, pick);
      }
    } else if (m->live_count > 0u) {
      // Spot-check one vertex against the model's own degree accounting, and
      // that an incident walk yields exactly that many edges.
      const uint32_t slot = m->live[xorshift64(&rng) % m->live_count];
      const churn_vertex_t *v = &m->vertices[slot];

      ok = dv_graph_out_degree(&g, v->handle) == v->out_degree;
      ok = ok && dv_graph_in_degree(&g, v->handle) == v->in_degree;

      dv_graph_edge_iter_t it;
      ok = ok && dv_graph_edges_begin(&g, v->handle, &it) == DV_GRAPH_OK;

      uint32_t walked = 0u;
      while (ok && dv_graph_edge_iter_next(&it, NULL))
        ++walked;

      ok = ok && walked == v->out_degree + v->in_degree;
    }

    ok = ok && dv_graph_vertex_count(&g) == m->live_count;
    ok = ok && dv_graph_edge_count(&g) == m->edge_count;

    if (ok && (op & 1023u) == 0u)
      ok = dv_graph_is_valid(&g);
  }

  ok = ok && dv_graph_is_valid(&g);

  // Whatever survived, removing every live vertex empties the edge pool too.
  for (uint32_t i = 0u; i < m->live_count && ok; ++i)
    ok = dv_graph_remove_vertex(&g, m->vertices[m->live[i]].handle) ==
         DV_GRAPH_OK;

  ok = ok && dv_graph_vertex_count(&g) == 0u;
  ok = ok && dv_graph_edge_count(&g) == 0u;
  ok = ok && dv_graph_is_valid(&g);

  free(m);
  dv_graph_destroy(&g);
  return ok;
}

// --- Phase 4: growth and shrinkage with byte accounting ----------------

static bool phase_grow_shrink(void) {
  const uint32_t n = GRAPH_VERTICES / 8u;

  account_t account;
  const dv_graph_allocator_t hooks = account_hooks(&account);

  dv_graph_t g;
  const dv_graph_config_t config = graph_config(true, 0u, 0u, true);
  if (dv_graph_init(&g, &config, &hooks) != DV_GRAPH_OK)
    return false;

  dv_vertex_t *vertices = (dv_vertex_t *)malloc(n * sizeof(*vertices));
  if (!vertices) {
    dv_graph_destroy(&g);
    return false;
  }

  bool ok = true;
  size_t baseline = 0u;

  for (uint32_t cycle = 0u; cycle < GRAPH_CYCLES && ok; ++cycle) {
    for (uint32_t i = 0u; i < n && ok; ++i)
      ok = dv_graph_add_vertex(&g, &vertices[i]) == DV_GRAPH_OK;

    for (uint32_t i = 1u; i < n && ok; ++i)
      ok = dv_graph_add_edge(&g, vertices[i - 1u], vertices[i], 1.0, NULL) ==
           DV_GRAPH_OK;

    // Over-reserve, then give it back: shrink returns capacity to the
    // high-water mark, so the reserved surplus is released and the live
    // elements are untouched.
    const size_t before = account.live_bytes;
    ok = ok && dv_graph_reserve_vertices(&g, 4u * n) == DV_GRAPH_OK;
    ok = ok && dv_graph_reserve_edges(&g, 4u * n) == DV_GRAPH_OK;
    ok = ok && account.live_bytes > before;

    ok = ok && dv_graph_shrink_to_fit(&g) == DV_GRAPH_OK;
    ok = ok && dv_graph_vertex_capacity(&g) == n;
    ok = ok && dv_graph_edge_capacity(&g) == n - 1u;
    ok = ok && account.live_bytes <= before;
    ok = ok && dv_graph_vertex_count(&g) == n;
    ok = ok && dv_graph_is_valid(&g);

    uint32_t seen = 0u;
    ok = ok && dv_graph_bfs(&g, vertices[0], count_visit, &seen) ==
                   DV_GRAPH_OK;
    ok = ok && seen == n;

    ok = ok && dv_graph_reset(&g) == DV_GRAPH_OK;
    ok = ok && dv_graph_vertex_capacity(&g) == 0u;
    ok = ok && dv_graph_edge_capacity(&g) == 0u;
    ok = ok && dv_graph_is_valid(&g);

    // Every cycle has to come back to the same footprint. A leak of one block
    // per cycle would show up here as a rising baseline.
    if (cycle == 0u)
      baseline = account.live_bytes;
    else
      ok = ok && account.live_bytes == baseline;
  }

  ok = ok && account.align_respected;

  free(vertices);
  dv_graph_destroy(&g);

  ok = ok && account.live_bytes == 0u;
  return ok;
}

// --- Phase 5: bulk ingestion against one-at-a-time ---------------------

// Same edges, same order, one graph built in batches and one edge by edge.
// Slot allocation is deterministic, so the two must agree index for index.
static bool phase_bulk_matches_single(void) {
  const uint32_t n = GRAPH_VERTICES / 4u;
  const uint32_t edges = n * 2u;

  dv_graph_t bulk;
  dv_graph_t single;
  const dv_graph_config_t config = graph_config(true, n, edges, false);

  if (dv_graph_init(&bulk, &config, NULL) != DV_GRAPH_OK)
    return false;
  if (dv_graph_init(&single, &config, NULL) != DV_GRAPH_OK) {
    dv_graph_destroy(&bulk);
    return false;
  }

  dv_vertex_t *bulk_vertices = (dv_vertex_t *)malloc(n * sizeof(dv_vertex_t));
  dv_vertex_t *single_vertices =
      (dv_vertex_t *)malloc(n * sizeof(dv_vertex_t));
  dv_graph_edge_spec_t *specs =
      (dv_graph_edge_spec_t *)malloc(BATCH_SIZE * sizeof(*specs));

  bool ok = bulk_vertices && single_vertices && specs;

  ok = ok && dv_graph_add_vertices(&bulk, n, bulk_vertices) == n;
  for (uint32_t i = 0u; i < n && ok; ++i)
    ok = dv_graph_add_vertex(&single, &single_vertices[i]) == DV_GRAPH_OK;

  uint64_t rng = GRAPH_SEED ^ 0xA5A5A5A5A5A5A5A5ull;
  uint32_t written = 0u;

  while (ok && written < edges) {
    const uint32_t batch =
        (edges - written) < BATCH_SIZE ? (edges - written) : BATCH_SIZE;

    for (uint32_t i = 0u; i < batch; ++i) {
      const uint32_t a = (uint32_t)(xorshift64(&rng) % n);
      const uint32_t b = (uint32_t)(xorshift64(&rng) % n);

      specs[i].src = bulk_vertices[a];
      specs[i].dst = bulk_vertices[b];
      specs[i].weight = (double)(written + i);

      ok = ok && dv_graph_add_edge(&single, single_vertices[a],
                                   single_vertices[b],
                                   (double)(written + i), NULL) == DV_GRAPH_OK;
    }

    ok = ok && dv_graph_add_edges(&bulk, specs, batch, NULL) == batch;
    written += batch;
  }

  ok = ok && dv_graph_edge_count(&bulk) == dv_graph_edge_count(&single);
  ok = ok && dv_graph_edge_count(&bulk) == edges;

  for (uint32_t i = 0u; i < n && ok; ++i) {
    ok = dv_graph_out_degree(&bulk, bulk_vertices[i]) ==
         dv_graph_out_degree(&single, single_vertices[i]);
    ok = ok && dv_graph_in_degree(&bulk, bulk_vertices[i]) ==
                   dv_graph_in_degree(&single, single_vertices[i]);
  }

  // And the adjacency lists agree edge for edge, in order.
  for (uint32_t i = 0u; i < n && ok; ++i) {
    dv_graph_edge_iter_t bulk_it;
    dv_graph_edge_iter_t single_it;

    ok = dv_graph_out_edges_begin(&bulk, bulk_vertices[i], &bulk_it) ==
         DV_GRAPH_OK;
    ok = ok && dv_graph_out_edges_begin(&single, single_vertices[i],
                                        &single_it) == DV_GRAPH_OK;

    dv_graph_edge_ref_t a;
    dv_graph_edge_ref_t b;
    while (ok && dv_graph_edge_iter_next(&bulk_it, &a)) {
      ok = dv_graph_edge_iter_next(&single_it, &b);
      ok = ok && a.neighbor.index == b.neighbor.index;
      ok = ok && a.weight == b.weight;
    }
    ok = ok && !dv_graph_edge_iter_next(&single_it, &b);
  }

  ok = ok && dv_graph_is_valid(&bulk);
  ok = ok && dv_graph_is_valid(&single);

  free(bulk_vertices);
  free(single_vertices);
  free(specs);
  dv_graph_destroy(&bulk);
  dv_graph_destroy(&single);
  return ok;
}

// --- Phase 6: wrapper under contention ---------------------------------

typedef struct {
  dv_graph_mt_t *graph;
  uint32_t edges;
  bool ok;
} contend_t;

static void *contend_worker(void *arg) {
  contend_t *c = (contend_t *)arg;

  dv_vertex_t a = dv_vertex_none();
  dv_vertex_t b = dv_vertex_none();
  if (dv_graph_mt_add_vertex(c->graph, &a) != DV_GRAPH_OK ||
      dv_graph_mt_add_vertex(c->graph, &b) != DV_GRAPH_OK) {
    c->ok = false;
    return NULL;
  }

  for (uint32_t i = 0u; i < c->edges; ++i) {
    dv_edge_t e = dv_edge_none();
    if (dv_graph_mt_add_edge(c->graph, a, b, (double)i, &e) != DV_GRAPH_OK) {
      c->ok = false;
      return NULL;
    }

    // Half of them come straight back out, so the pools recycle under
    // contention rather than only growing.
    if ((i & 1u) == 0u && dv_graph_mt_remove_edge(c->graph, e) != DV_GRAPH_OK) {
      c->ok = false;
      return NULL;
    }
  }

  c->ok = dv_graph_mt_degree(c->graph, a) == c->edges / 2u;
  return NULL;
}

static void contend_validate(const dv_graph_t *g, void *user_data) {
  bool *ok = (bool *)user_data;
  *ok = dv_graph_is_valid(g);
}

static bool phase_mt_contention(void) {
  dv_graph_mt_t graph;
  const dv_graph_config_t config = graph_config(true, 0u, 0u, true);
  if (dv_graph_mt_init(&graph, &config, NULL) != DV_GRAPH_OK)
    return false;

  pthread_t threads[GRAPH_THREADS];
  contend_t workers[GRAPH_THREADS];
  const uint32_t per_thread = GRAPH_EDGES / GRAPH_THREADS;

  for (uint32_t i = 0u; i < GRAPH_THREADS; ++i) {
    workers[i].graph = &graph;
    workers[i].edges = per_thread;
    workers[i].ok = true;
    fail_pthread(
        pthread_create(&threads[i], NULL, contend_worker, &workers[i]),
        "pthread_create");
  }

  bool ok = true;
  for (uint32_t i = 0u; i < GRAPH_THREADS; ++i) {
    fail_pthread(pthread_join(threads[i], NULL), "pthread_join");
    ok = ok && workers[i].ok;
  }

  ok = ok && dv_graph_mt_vertex_count(&graph) == GRAPH_THREADS * 2u;
  ok = ok && dv_graph_mt_edge_count(&graph) ==
                 GRAPH_THREADS * (per_thread - per_thread / 2u);

  bool valid = false;
  ok = ok && dv_graph_mt_read(&graph, contend_validate, &valid) == DV_GRAPH_OK;
  ok = ok && valid;

  dv_graph_mt_destroy(&graph);
  return ok;
}

// --- Driver ------------------------------------------------------------

typedef struct {
  const char *name;
  bool (*fn)(void);
} phase_t;

int main(void) {
  const phase_t phases[] = {
      {"build and teardown", phase_build_teardown},
      {"traversal shapes", phase_traversal_shapes},
      {"churn against model", phase_churn},
      {"grow and shrink cycles", phase_grow_shrink},
      {"bulk matches single", phase_bulk_matches_single},
      {"mutex wrapper contention", phase_mt_contention},
  };

  const size_t nphases = sizeof(phases) / sizeof(phases[0]);

  printf("Initializing\n");
  printf("\tVertices:         %zu\n", (size_t)GRAPH_VERTICES);
  printf("\tEdges:            %zu\n", (size_t)GRAPH_EDGES);
  printf("\tCycles:           %zu\n", (size_t)GRAPH_CYCLES);
  printf("\tChurn Ops:        %zu\n", (size_t)CHURN_OPS);
  printf("\tBatch Size:       %zu\n", (size_t)BATCH_SIZE);
  printf("\tThreads:          %zu\n", (size_t)GRAPH_THREADS);
  printf("\tCache Line:       %zu\n", (size_t)DV_CACHE_LINE_SIZE);

  printf("Running %zu phases...\n", nphases);

  bool all_ok = true;
  for (size_t i = 0u; i < nphases; ++i) {
    printf("[PHASE] %s\n", phases[i].name);
    const bool ok = phases[i].fn();
    report(phases[i].name, ok);
    all_ok = all_ok && ok;
  }

  if (all_ok) {
    puts("GREAT SUCCESS!");
    return EXIT_SUCCESS;
  }

  puts("I AM DIE.");
  return EXIT_FAILURE;
}
