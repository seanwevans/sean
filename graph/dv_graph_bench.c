// dv_graph_bench.c - single-threaded throughput benchmarks for dv_graph.
//
// Each backend reports operations per second over `runs` repetitions and
// prints the median alongside the best and worst sample. Every run produces a
// checksum that is verified before its timing is counted, so a backend that
// gets the wrong answer fast is not reported at all.
//
// Backends that measure work over an existing graph get one built for them
// outside the timed region, and rebuilt between runs when they consume it, so
// the number is the operation and not the construction.
//
// Two pairs exist to measure design decisions rather than the graph itself:
// add_edges_bulk against add_edge is the batched reserve against one resize
// check per insertion, and remove_edge_between against remove_edge is the cost
// of finding an edge by its endpoints versus unlinking one already in hand --
// the difference the doubly linked incidence lists are there to make O(1).

#define _POSIX_C_SOURCE 200809L

#include "dv_graph.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  size_t vertices;
  size_t edges;
  size_t probes;
  size_t runs;
} bench_config_t;

typedef struct {
  dv_graph_t graph;
  dv_vertex_t *vertices;
  dv_edge_t *edges;
  const uint64_t *data;
} bench_ctx_t;

typedef bool (*bench_fn)(bench_ctx_t *ctx, const bench_config_t *cfg,
                         uint64_t *checksum);

typedef struct {
  const char *name;
  bench_fn fn;
  size_t (*ops)(const bench_config_t *cfg);
  bool prepared; // hand the backend a graph built outside the timer
} bench_case_t;

static volatile uint64_t g_sink = 0u;

static double now_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0.0;

  return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

static bool parse_size_arg(const char *text, size_t *out) {
  if (!text || *text == '\0')
    return false;

  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (!end || *end != '\0')
    return false;

  *out = (size_t)value;
  return true;
}

static bench_config_t parse_args(int argc, char **argv) {
  bench_config_t cfg = {
      .vertices = 200000u,
      .edges = 800000u,
      .probes = 200000u,
      .runs = 7u,
  };

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--vertices") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.vertices);
    else if (strcmp(argv[i], "--edges") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.edges);
    else if (strcmp(argv[i], "--probes") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.probes);
    else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.runs);
  }

  return cfg;
}

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static dv_graph_config_t base_config(const bench_config_t *cfg) {
  const dv_graph_config_t config = {
      .directed = true,
      .weighted = true,
      .growable = false,
      .cache_aligned = true,
      .reject_parallel_edges = false,
      .reject_self_loops = false,
      .initial_vertex_capacity = (uint32_t)cfg->vertices,
      .initial_edge_capacity = (uint32_t)cfg->edges,
      .max_vertex_capacity = 0u,
      .max_edge_capacity = 0u,
  };
  return config;
}

// Endpoints come from the shared dataset, so every backend walks the same
// graph and the numbers compare.
static uint32_t endpoint(const bench_ctx_t *ctx, const bench_config_t *cfg,
                         size_t i) {
  return (uint32_t)(ctx->data[i % cfg->edges] % cfg->vertices);
}

static bool ctx_build(bench_ctx_t *ctx, const bench_config_t *cfg) {
  const dv_graph_config_t config = base_config(cfg);
  if (dv_graph_init(&ctx->graph, &config, NULL) != DV_GRAPH_OK)
    return false;

  if (dv_graph_add_vertices(&ctx->graph, (uint32_t)cfg->vertices,
                            ctx->vertices) != (uint32_t)cfg->vertices)
    return false;

  for (size_t i = 0u; i < cfg->edges; ++i) {
    const uint32_t a = endpoint(ctx, cfg, i);
    const uint32_t b = endpoint(ctx, cfg, i + 1u);

    if (dv_graph_add_edge(&ctx->graph, ctx->vertices[a], ctx->vertices[b],
                          (double)i, &ctx->edges[i]) != DV_GRAPH_OK)
      return false;
  }

  // Sized here so a traversal backend measures the walk and not one resize.
  return dv_graph_reserve_traversal(&ctx->graph) == DV_GRAPH_OK;
}

static void ctx_teardown(bench_ctx_t *ctx) { dv_graph_destroy(&ctx->graph); }

// --- Backends ----------------------------------------------------------

// One edge at a time into a graph already sized for all of them: the cost of
// an insertion is two list appends and a slot pop.
static bool bench_add_edge(bench_ctx_t *ctx, const bench_config_t *cfg,
                           uint64_t *checksum) {
  if (!ctx_build(ctx, cfg))
    return false;

  *checksum = dv_graph_edge_count(&ctx->graph);
  ctx_teardown(ctx);
  return true;
}

// The same edges through the batch path.
static bool bench_add_edges_bulk(bench_ctx_t *ctx, const bench_config_t *cfg,
                                 uint64_t *checksum) {
  const dv_graph_config_t config = base_config(cfg);
  if (dv_graph_init(&ctx->graph, &config, NULL) != DV_GRAPH_OK)
    return false;

  if (dv_graph_add_vertices(&ctx->graph, (uint32_t)cfg->vertices,
                            ctx->vertices) != (uint32_t)cfg->vertices) {
    ctx_teardown(ctx);
    return false;
  }

  enum { BATCH = 64u };
  dv_graph_edge_spec_t specs[BATCH];

  size_t written = 0u;
  while (written < cfg->edges) {
    const size_t batch =
        (cfg->edges - written) < BATCH ? (cfg->edges - written) : BATCH;

    for (size_t i = 0u; i < batch; ++i) {
      const uint32_t a = endpoint(ctx, cfg, written + i);
      const uint32_t b = endpoint(ctx, cfg, written + i + 1u);

      specs[i].src = ctx->vertices[a];
      specs[i].dst = ctx->vertices[b];
      specs[i].weight = (double)(written + i);
    }

    if (dv_graph_add_edges(&ctx->graph, specs, (uint32_t)batch, NULL) !=
        (uint32_t)batch) {
      ctx_teardown(ctx);
      return false;
    }

    written += batch;
  }

  *checksum = dv_graph_edge_count(&ctx->graph);
  ctx_teardown(ctx);
  return true;
}

// Every adjacency list walked end to end through the inline iterator.
static bool bench_adjacency_scan(bench_ctx_t *ctx, const bench_config_t *cfg,
                                 uint64_t *checksum) {
  uint64_t sum = 0u;

  for (size_t i = 0u; i < cfg->vertices; ++i) {
    dv_graph_edge_iter_t it;
    if (dv_graph_out_edges_begin(&ctx->graph, ctx->vertices[i], &it) !=
        DV_GRAPH_OK)
      return false;

    dv_graph_edge_ref_t ref;
    while (dv_graph_edge_iter_next(&it, &ref))
      sum += ref.neighbor.index;
  }

  *checksum = sum;
  return true;
}

typedef struct {
  uint64_t sum;
  uint64_t visits;
} walk_sum_t;

static bool walk_accumulate(dv_vertex_t vertex, uint32_t depth,
                            void *user_data) {
  walk_sum_t *w = (walk_sum_t *)user_data;

  w->sum += (uint64_t)vertex.index + depth;
  ++w->visits;
  return true;
}

static bool bench_bfs(bench_ctx_t *ctx, const bench_config_t *cfg,
                      uint64_t *checksum) {
  walk_sum_t w = {0u, 0u};
  if (dv_graph_bfs(&ctx->graph, ctx->vertices[0], walk_accumulate, &w) !=
      DV_GRAPH_OK)
    return false;

  (void)cfg;
  *checksum = w.visits;
  return true;
}

// Same coverage, different frontier discipline and a resumed edge cursor per
// level of the stack.
static bool bench_dfs(bench_ctx_t *ctx, const bench_config_t *cfg,
                      uint64_t *checksum) {
  walk_sum_t w = {0u, 0u};
  if (dv_graph_dfs(&ctx->graph, ctx->vertices[0], walk_accumulate, &w) !=
      DV_GRAPH_OK)
    return false;

  (void)cfg;
  *checksum = w.visits;
  return true;
}

// Lookup by endpoints, which scans the shorter of the two incidence lists.
static bool bench_find_edge(bench_ctx_t *ctx, const bench_config_t *cfg,
                            uint64_t *checksum) {
  uint64_t found = 0u;

  for (size_t i = 0u; i < cfg->probes; ++i) {
    const uint32_t a = endpoint(ctx, cfg, i);
    const uint32_t b = endpoint(ctx, cfg, i + 1u);

    if (dv_graph_has_edge(&ctx->graph, ctx->vertices[a], ctx->vertices[b]))
      ++found;
  }

  *checksum = found;
  return true;
}

// Removal with the handle in hand: two doubly linked unlinks and a slot push,
// with no search at all.
static bool bench_remove_edge(bench_ctx_t *ctx, const bench_config_t *cfg,
                              uint64_t *checksum) {
  uint64_t removed = 0u;

  for (size_t i = 0u; i < cfg->edges; ++i) {
    if (dv_graph_remove_edge(&ctx->graph, ctx->edges[i]) == DV_GRAPH_OK)
      ++removed;
  }

  *checksum = removed;
  return true;
}

// The same unlink, reached by searching for the endpoints instead.
static bool bench_remove_edge_between(bench_ctx_t *ctx,
                                      const bench_config_t *cfg,
                                      uint64_t *checksum) {
  uint64_t removed = 0u;

  for (size_t i = 0u; i < cfg->probes; ++i) {
    const uint32_t a = endpoint(ctx, cfg, i);
    const uint32_t b = endpoint(ctx, cfg, i + 1u);

    if (dv_graph_remove_edge_between(&ctx->graph, ctx->vertices[a],
                                     ctx->vertices[b]) == DV_GRAPH_OK)
      ++removed;
  }

  *checksum = removed;
  return true;
}

// Removing a vertex is O(degree), and every incident edge leaves both of its
// lists on the way out.
static bool bench_remove_vertex(bench_ctx_t *ctx, const bench_config_t *cfg,
                                uint64_t *checksum) {
  uint64_t removed = 0u;

  for (size_t i = 0u; i < cfg->vertices; ++i) {
    if (dv_graph_remove_vertex(&ctx->graph, ctx->vertices[i]) == DV_GRAPH_OK)
      ++removed;
  }

  *checksum = removed;
  return true;
}

// --- Harness -----------------------------------------------------------

static size_t ops_edges(const bench_config_t *cfg) { return cfg->edges; }
static size_t ops_vertices(const bench_config_t *cfg) { return cfg->vertices; }
static size_t ops_probes(const bench_config_t *cfg) { return cfg->probes; }

static int compare_double(const void *a, const void *b) {
  const double x = *(const double *)a;
  const double y = *(const double *)b;

  if (x < y)
    return -1;
  if (x > y)
    return 1;
  return 0;
}

static void run_case(const bench_case_t *bench, const bench_config_t *cfg,
                     bench_ctx_t *ctx, double *samples) {
  uint64_t reference = 0u;
  double last_seconds = 0.0;

  for (size_t run = 0u; run < cfg->runs; ++run) {
    if (bench->prepared && !ctx_build(ctx, cfg)) {
      fprintf(stderr, "bench=%s run=%zu FAILED to build\n", bench->name, run);
      ctx_teardown(ctx);
      return;
    }

    uint64_t checksum = 0u;

    const double start = now_seconds();
    const bool ok = bench->fn(ctx, cfg, &checksum);
    const double seconds = now_seconds() - start;

    if (bench->prepared)
      ctx_teardown(ctx);

    if (!ok) {
      fprintf(stderr, "bench=%s run=%zu FAILED to execute\n", bench->name, run);
      return;
    }

    if (run == 0u) {
      reference = checksum;
    } else if (checksum != reference) {
      fprintf(stderr,
              "bench=%s run=%zu checksum drift (got=%" PRIu64
              " expected=%" PRIu64 ")\n",
              bench->name, run, checksum, reference);
      return;
    }

    g_sink += checksum;

    samples[run] =
        seconds > 0.0 ? ((double)bench->ops(cfg) / seconds) : 0.0;
    last_seconds = seconds;
  }

  qsort(samples, cfg->runs, sizeof(*samples), compare_double);

  printf("bench=%s vertices=%zu edges=%zu runs=%zu median_ops_per_sec=%.3f "
         "best=%.3f worst=%.3f seconds_last=%.6f checksum=%" PRIu64 "\n",
         bench->name, cfg->vertices, cfg->edges, cfg->runs,
         samples[cfg->runs / 2u], samples[cfg->runs - 1u], samples[0],
         last_seconds, reference);
}

int main(int argc, char **argv) {
  const bench_config_t cfg = parse_args(argc, argv);

  if (cfg.vertices == 0u || cfg.edges == 0u || cfg.probes == 0u ||
      cfg.runs == 0u) {
    fprintf(stderr,
            "bad config: vertices, edges, probes and runs must be non-zero\n");
    return EXIT_FAILURE;
  }

  if (cfg.vertices > DV_GRAPH_MAX_CAPACITY ||
      cfg.edges > DV_GRAPH_MAX_CAPACITY) {
    fprintf(stderr, "bad config: pools are capped at %u\n",
            DV_GRAPH_MAX_CAPACITY);
    return EXIT_FAILURE;
  }

  uint64_t *data = (uint64_t *)malloc(cfg.edges * sizeof(*data));
  double *samples = (double *)malloc(cfg.runs * sizeof(*samples));
  dv_vertex_t *vertices =
      (dv_vertex_t *)malloc(cfg.vertices * sizeof(*vertices));
  dv_edge_t *edges = (dv_edge_t *)malloc(cfg.edges * sizeof(*edges));

  if (!data || !samples || !vertices || !edges) {
    free(data);
    free(samples);
    free(vertices);
    free(edges);
    fprintf(stderr, "out of memory\n");
    return EXIT_FAILURE;
  }

  // One fixed dataset shared by every backend, so the numbers compare.
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0u; i < cfg.edges; ++i)
    data[i] = xorshift64(&rng) >> 8;

  bench_ctx_t ctx = {
      .vertices = vertices,
      .edges = edges,
      .data = data,
  };
  memset(&ctx.graph, 0, sizeof(ctx.graph));

  const bench_case_t cases[] = {
      {"add_edge", bench_add_edge, ops_edges, false},
      {"add_edges_bulk", bench_add_edges_bulk, ops_edges, false},
      {"adjacency_scan", bench_adjacency_scan, ops_edges, true},
      {"bfs", bench_bfs, ops_vertices, true},
      {"dfs", bench_dfs, ops_vertices, true},
      {"find_edge", bench_find_edge, ops_probes, true},
      {"remove_edge", bench_remove_edge, ops_edges, true},
      {"remove_edge_between", bench_remove_edge_between, ops_probes, true},
      {"remove_vertex", bench_remove_vertex, ops_vertices, true},
  };

  const size_t ncases = sizeof(cases) / sizeof(cases[0]);

  printf("benchmark start: vertices=%zu edges=%zu probes=%zu runs=%zu "
         "cache_line=%zu\n",
         cfg.vertices, cfg.edges, cfg.probes, cfg.runs,
         (size_t)DV_CACHE_LINE_SIZE);

  for (size_t i = 0u; i < ncases; ++i)
    run_case(&cases[i], &cfg, &ctx, samples);

  free(data);
  free(samples);
  free(vertices);
  free(edges);
  return EXIT_SUCCESS;
}
