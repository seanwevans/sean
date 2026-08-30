// dv_heap_bench.c - single-threaded throughput benchmarks for dv_heap.
//
// Each backend reports operations per second over `runs` repetitions and
// prints the median alongside the best and worst sample. Every run produces a
// checksum that is verified before its timing is counted, so a backend that
// gets the wrong answer fast is not reported at all.
//
// Two of the backends exist to measure design decisions rather than the heap
// itself: build_floyd against push_sequence is the O(n) construction against
// the O(n log n) one, and custom_comparator against fixed_generic_u64 is the
// cost of an indirect call in the comparison loop against an inline key load.

#define _POSIX_C_SOURCE 200809L

#include "dv_heap.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  size_t items;
  size_t capacity;
  size_t k;
  size_t runs;
} bench_config_t;

typedef bool (*bench_fn)(const bench_config_t *cfg, const uint64_t *data,
                         uint64_t *checksum);

typedef struct {
  const char *name;
  bench_fn fn;
  size_t ops_per_item;
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
      .items = 2000000u,
      .capacity = 0u,
      .k = 64u,
      .runs = 7u,
  };

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--items") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.items);
    else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.capacity);
    else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.k);
    else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.runs);
  }

  if (cfg.capacity == 0u)
    cfg.capacity = cfg.items;

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

static dv_heap_config_t base_config(dv_heap_order_t order, size_t capacity) {
  const dv_heap_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = capacity,
      .max_capacity = 0u,
      .growable = false,
      .cache_aligned = true,
      .order = order,
      .key_type = DV_HEAP_KEY_U64,
      .key_offset = 0u,
      .key_indirect = false,
      .compare = NULL,
      .compare_user_data = NULL,
      .on_move = NULL,
      .move_user_data = NULL,
  };
  return config;
}

// --- Backends ----------------------------------------------------------

// Fill the heap and drain it: the baseline push/pop cost with an inline key.
static bool bench_push_pop(const bench_config_t *cfg, const uint64_t *data,
                           uint64_t *checksum) {
  dv_heap_t h;
  const dv_heap_config_t config = base_config(DV_HEAP_MIN, cfg->items);

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  for (size_t i = 0u; i < cfg->items; ++i)
    (void)dv_heap_push(&h, &data[i]);

  uint64_t sum = 0u;
  for (size_t i = 0u; i < cfg->items; ++i) {
    uint64_t value = 0u;
    (void)dv_heap_pop(&h, &value);
    sum += value;
  }

  dv_heap_destroy(&h);
  *checksum = sum;
  return true;
}

// Construction only, one element at a time: O(n log n).
static bool bench_push_sequence(const bench_config_t *cfg, const uint64_t *data,
                                uint64_t *checksum) {
  dv_heap_t h;
  const dv_heap_config_t config = base_config(DV_HEAP_MIN, cfg->items);

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  for (size_t i = 0u; i < cfg->items; ++i)
    (void)dv_heap_push(&h, &data[i]);

  const uint64_t *top = (const uint64_t *)dv_heap_top(&h);
  *checksum = top ? *top : 0u;

  dv_heap_destroy(&h);
  return true;
}

// The same construction bottom-up: O(n). Both leave the same root, so the
// checksums are directly comparable and so are the timings.
static bool bench_build_floyd(const bench_config_t *cfg, const uint64_t *data,
                              uint64_t *checksum) {
  dv_heap_t h;
  const dv_heap_config_t config = base_config(DV_HEAP_MIN, cfg->items);

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  if (dv_heap_build(&h, data, cfg->items) != DV_HEAP_OK) {
    dv_heap_destroy(&h);
    return false;
  }

  const uint64_t *top = (const uint64_t *)dv_heap_top(&h);
  *checksum = top ? *top : 0u;

  dv_heap_destroy(&h);
  return true;
}

static int compare_u64(const void *a, const void *b, void *user_data) {
  (void)user_data;

  const uint64_t x = *(const uint64_t *)a;
  const uint64_t y = *(const uint64_t *)b;

  if (x < y)
    return -1;
  if (x > y)
    return 1;
  return 0;
}

// Identical ordering to bench_push_pop, expressed as a callback instead of a
// built-in key. The gap between the two is the cost of the indirect call.
static bool bench_custom_comparator(const bench_config_t *cfg,
                                    const uint64_t *data, uint64_t *checksum) {
  dv_heap_t h;
  dv_heap_config_t config = base_config(DV_HEAP_MIN, cfg->items);
  config.key_type = DV_HEAP_KEY_CUSTOM;
  config.compare = compare_u64;

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  for (size_t i = 0u; i < cfg->items; ++i)
    (void)dv_heap_push(&h, &data[i]);

  uint64_t sum = 0u;
  for (size_t i = 0u; i < cfg->items; ++i) {
    uint64_t value = 0u;
    (void)dv_heap_pop(&h, &value);
    sum += value;
  }

  dv_heap_destroy(&h);
  *checksum = sum;
  return true;
}

// Bounded top-k over a stream: a size-k max-heap and one replace_top per
// candidate, which is the workload the design doc names.
static bool bench_top_k(const bench_config_t *cfg, const uint64_t *data,
                        uint64_t *checksum) {
  dv_heap_t h;
  const dv_heap_config_t config = base_config(DV_HEAP_MAX, cfg->k);

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  for (size_t i = 0u; i < cfg->items; ++i) {
    if (dv_heap_size(&h) < cfg->k) {
      (void)dv_heap_push(&h, &data[i]);
      continue;
    }

    const uint64_t *top = (const uint64_t *)dv_heap_top(&h);
    if (top && data[i] < *top)
      (void)dv_heap_replace_top(&h, &data[i], NULL);
  }

  uint64_t sum = 0u;
  uint64_t value = 0u;
  while (dv_heap_pop(&h, &value) == DV_HEAP_OK)
    sum += value;

  dv_heap_destroy(&h);
  *checksum = sum;
  return true;
}

typedef struct {
  uint64_t key;
  size_t heap_index;
} bench_node_t;

static void bench_on_move(void *slot, size_t index, void *user_data) {
  (void)user_data;
  bench_node_t *node = *(bench_node_t **)slot;
  node->heap_index = index;
}

// A pointer payload with an index map and a stream of decrease_key updates:
// the scheduling shape, with the on_move hook active on every element move.
static bool bench_decrease_key(const bench_config_t *cfg, const uint64_t *data,
                               uint64_t *checksum) {
  const size_t live = cfg->k * 64u;
  const size_t count = live < cfg->items ? live : cfg->items;

  dv_heap_config_t config = base_config(DV_HEAP_MIN, count);
  config.elem_size = sizeof(void *);
  config.elem_align = alignof(void *);
  config.key_offset = offsetof(bench_node_t, key);
  config.key_indirect = true;
  config.on_move = bench_on_move;

  dv_heap_t h;
  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  bench_node_t *nodes = (bench_node_t *)calloc(count, sizeof(*nodes));
  if (!nodes) {
    dv_heap_destroy(&h);
    return false;
  }

  for (size_t i = 0u; i < count; ++i) {
    nodes[i].key = data[i];
    bench_node_t *ptr = &nodes[i];
    (void)dv_heap_push(&h, &ptr);
  }

  // Walk the data stream lowering keys, which is the expensive direction: a
  // decrease_key sifts up through the ancestors and reports every move. A key
  // that bottoms out wraps back to the top of the range rather than becoming a
  // no-op, so the workload neither decays nor stops touching the sift loops.
  uint64_t sum = 0u;
  for (size_t i = 0u; i < cfg->items; ++i) {
    bench_node_t *node = &nodes[data[i] % count];

    node->key =
        node->key > 1u ? (node->key >> 1) : (data[i] | ((uint64_t)1u << 40));
    (void)dv_heap_update_at(&h, node->heap_index, &node);
    sum ^= node->key;
  }

  bench_node_t *node = NULL;
  while (dv_heap_pop(&h, &node) == DV_HEAP_OK)
    sum += node->key;

  free(nodes);
  dv_heap_destroy(&h);
  *checksum = sum;
  return true;
}

// --- Driver ------------------------------------------------------------

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
                     const uint64_t *data, double *samples) {
  uint64_t reference = 0u;
  double last_seconds = 0.0;

  for (size_t run = 0u; run < cfg->runs; ++run) {
    uint64_t checksum = 0u;

    const double start = now_seconds();
    const bool ok = bench->fn(cfg, data, &checksum);
    const double seconds = now_seconds() - start;

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

    samples[run] = seconds > 0.0
                       ? ((double)(cfg->items * bench->ops_per_item) / seconds)
                       : 0.0;
    last_seconds = seconds;
  }

  qsort(samples, cfg->runs, sizeof(*samples), compare_double);

  printf("bench=%s items=%zu k=%zu runs=%zu median_ops_per_sec=%.3f "
         "best=%.3f worst=%.3f seconds_last=%.6f checksum=%" PRIu64 "\n",
         bench->name, cfg->items, cfg->k, cfg->runs, samples[cfg->runs / 2u],
         samples[cfg->runs - 1u], samples[0], last_seconds, reference);
}

int main(int argc, char **argv) {
  const bench_config_t cfg = parse_args(argc, argv);

  if (cfg.items == 0u || cfg.k == 0u || cfg.runs == 0u) {
    fprintf(stderr, "bad config: items, k and runs must be non-zero\n");
    return EXIT_FAILURE;
  }

  uint64_t *data = (uint64_t *)malloc(cfg.items * sizeof(*data));
  double *samples = (double *)malloc(cfg.runs * sizeof(*samples));
  if (!data || !samples) {
    free(data);
    free(samples);
    fprintf(stderr, "out of memory\n");
    return EXIT_FAILURE;
  }

  // One fixed dataset shared by every backend, so the numbers compare.
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0u; i < cfg.items; ++i)
    data[i] = xorshift64(&rng) >> 8;

  const bench_case_t cases[] = {
      {"push_pop_u64", bench_push_pop, 2u},
      {"push_sequence", bench_push_sequence, 1u},
      {"build_floyd", bench_build_floyd, 1u},
      {"custom_comparator", bench_custom_comparator, 2u},
      {"top_k_replace", bench_top_k, 1u},
      {"update_key", bench_decrease_key, 1u},
  };

  const size_t ncases = sizeof(cases) / sizeof(cases[0]);

  printf("benchmark start: items=%zu k=%zu runs=%zu cache_line=%zu\n",
         cfg.items, cfg.k, cfg.runs, (size_t)DV_CACHE_LINE_SIZE);

  for (size_t i = 0u; i < ncases; ++i)
    run_case(&cases[i], &cfg, data, samples);

  free(data);
  free(samples);
  return EXIT_SUCCESS;
}
