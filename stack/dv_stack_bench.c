// dv_stack_bench.c - single-threaded throughput benchmarks for dv_stack.
//
// Each backend reports operations per second over `runs` repetitions and
// prints the median alongside the best and worst sample, so a noisy machine is
// visible in the output rather than hidden by it. Every run also produces a
// checksum that is verified before its timing is counted; a backend that gets
// the wrong answer fast is not reported at all.

#define _POSIX_C_SOURCE 200809L

#include "dv_stack.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  size_t items;
  size_t capacity;
  size_t batch;
  size_t runs;
} bench_config_t;

typedef bool (*bench_fn)(const bench_config_t *cfg, uint64_t *checksum);

typedef struct {
  const char *name;
  bench_fn fn;
  size_t ops_per_item; // operations counted per item (push+pop is 2)
} bench_case_t;

// Consumes popped values so the compiler cannot delete the drain loops.
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
      .items = 20000000u,
      .capacity = 65536u,
      .batch = 64u,
      .runs = 7u,
  };

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--items") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.items);
    else if (strcmp(argv[i], "--capacity") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.capacity);
    else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc)
      (void)parse_size_arg(argv[++i], &cfg.batch);
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

static dv_stack_config_t fixed_config(size_t elem_size, size_t elem_align,
                                      size_t capacity) {
  const dv_stack_config_t config = {
      .elem_size = elem_size,
      .elem_align = elem_align,
      .initial_capacity = capacity,
      .max_capacity = 0u,
      .growable = false,
      .cache_aligned = true,
  };
  return config;
}

// --- Backends ----------------------------------------------------------

// Fill a fixed stack to the brim, drain it, repeat. This is the shape the
// module is tuned for: no allocator traffic and a single hot cache line at the
// top of the buffer.
static bool bench_fixed_generic(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  const dv_stack_config_t config =
      fixed_config(sizeof(uint64_t), alignof(uint64_t), cfg->capacity);

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  uint64_t sum = 0u;
  size_t remaining = cfg->items;

  while (remaining > 0u) {
    const size_t n = remaining < cfg->capacity ? remaining : cfg->capacity;

    for (uint64_t i = 0u; i < (uint64_t)n; ++i)
      (void)dv_stack_push(&s, &i);

    for (size_t i = 0u; i < n; ++i) {
      uint64_t out = 0u;
      (void)dv_stack_pop(&s, &out);
      sum += out;
    }

    remaining -= n;
  }

  dv_stack_destroy(&s);
  *checksum = sum;
  return true;
}

// Same shape with a cache-line-sized payload, which moves the cost from the
// index arithmetic to the copy itself.
static bool bench_fixed_wide(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  const dv_stack_config_t config =
      fixed_config(DV_CACHE_LINE_SIZE, DV_CACHE_LINE_SIZE, cfg->capacity);

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  unsigned char element[DV_CACHE_LINE_SIZE];
  unsigned char out[DV_CACHE_LINE_SIZE];
  memset(element, 0, sizeof(element));

  uint64_t sum = 0u;
  size_t remaining = cfg->items;

  while (remaining > 0u) {
    const size_t n = remaining < cfg->capacity ? remaining : cfg->capacity;

    for (size_t i = 0u; i < n; ++i) {
      memcpy(element, &i, sizeof(i));
      (void)dv_stack_push(&s, element);
    }

    for (size_t i = 0u; i < n; ++i) {
      (void)dv_stack_pop(&s, out);
      uint64_t value = 0u;
      memcpy(&value, out, sizeof(value));
      sum += value;
    }

    remaining -= n;
  }

  dv_stack_destroy(&s);
  *checksum = sum;
  return true;
}

// The pointer specialization writes slots directly instead of calling memcpy.
static bool bench_pointer(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  if (dv_stack_init_ptr(&s, cfg->capacity, false, NULL) != DV_STACK_OK)
    return false;

  uint64_t sum = 0u;
  size_t remaining = cfg->items;

  while (remaining > 0u) {
    const size_t n = remaining < cfg->capacity ? remaining : cfg->capacity;

    for (size_t i = 0u; i < n; ++i)
      (void)dv_stack_push_ptr(&s, (void *)(uintptr_t)(i + 1u));

    for (size_t i = 0u; i < n; ++i) {
      void *out = NULL;
      (void)dv_stack_pop_ptr(&s, &out);
      sum += (uint64_t)(uintptr_t)out;
    }

    remaining -= n;
  }

  dv_stack_destroy(&s);
  *checksum = sum;
  return true;
}

// Batch helpers amortize the per-element bounds check across a whole memcpy.
static bool bench_bulk(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  const dv_stack_config_t config =
      fixed_config(sizeof(uint64_t), alignof(uint64_t), cfg->capacity);

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  uint64_t *batch = (uint64_t *)malloc(cfg->batch * sizeof(*batch));
  uint64_t *drained = (uint64_t *)malloc(cfg->batch * sizeof(*drained));
  if (!batch || !drained) {
    free(batch);
    free(drained);
    dv_stack_destroy(&s);
    return false;
  }

  for (size_t i = 0u; i < cfg->batch; ++i)
    batch[i] = (uint64_t)i + 1u;

  uint64_t sum = 0u;
  size_t remaining = cfg->items;

  while (remaining > 0u) {
    const size_t n = remaining < cfg->batch ? remaining : cfg->batch;

    const size_t pushed = dv_stack_push_bulk(&s, batch, n);
    const size_t popped = dv_stack_pop_bulk(&s, drained, pushed);

    for (size_t i = 0u; i < popped; ++i)
      sum += drained[i];

    if (popped == 0u)
      break;

    remaining -= popped;
  }

  free(batch);
  free(drained);
  dv_stack_destroy(&s);
  *checksum = sum;
  return true;
}

// Growth from empty, including every reallocation and copy along the way.
// Counted as one operation per push so the number is the amortized push cost.
static bool bench_growth(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = 0u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
  };

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  for (uint64_t i = 0u; i < (uint64_t)cfg->items; ++i) {
    if (dv_stack_push(&s, &i) != DV_STACK_OK) {
      dv_stack_destroy(&s);
      return false;
    }
  }

  const uint64_t *top = (const uint64_t *)dv_stack_top(&s);
  const uint64_t size = (uint64_t)dv_stack_size(&s);
  const uint64_t observed = top ? *top : 0u;

  dv_stack_destroy(&s);
  *checksum = observed + size;
  return true;
}

// An unpredictable push/pop mix, which is what real LIFO workloads look like
// and what defeats the branch predictor.
static bool bench_random_mix(const bench_config_t *cfg, uint64_t *checksum) {
  dv_stack_t s;
  const dv_stack_config_t config =
      fixed_config(sizeof(uint64_t), alignof(uint64_t), cfg->capacity);

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint64_t sum = 0u;

  for (size_t i = 0u; i < cfg->items; ++i) {
    const uint64_t roll = xorshift64(&rng);

    if ((roll & 1u) != 0u) {
      const uint64_t value = roll >> 1;
      (void)dv_stack_push(&s, &value);
    } else {
      uint64_t out = 0u;
      if (dv_stack_pop(&s, &out) == DV_STACK_OK)
        sum += out;
    }
  }

  uint64_t out = 0u;
  while (dv_stack_pop(&s, &out) == DV_STACK_OK)
    sum += out;

  dv_stack_destroy(&s);
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
                     double *samples) {
  uint64_t reference = 0u;
  double last_seconds = 0.0;

  for (size_t run = 0u; run < cfg->runs; ++run) {
    uint64_t checksum = 0u;

    const double start = now_seconds();
    const bool ok = bench->fn(cfg, &checksum);
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

  printf("bench=%s items=%zu cap=%zu batch=%zu runs=%zu "
         "median_ops_per_sec=%.3f best=%.3f worst=%.3f seconds_last=%.6f "
         "checksum=%" PRIu64 "\n",
         bench->name, cfg->items, cfg->capacity, cfg->batch, cfg->runs,
         samples[cfg->runs / 2u], samples[cfg->runs - 1u], samples[0],
         last_seconds, reference);
}

int main(int argc, char **argv) {
  const bench_config_t cfg = parse_args(argc, argv);

  if (cfg.items == 0u || cfg.capacity == 0u || cfg.batch == 0u ||
      cfg.runs == 0u) {
    fprintf(stderr, "bad config: items, capacity, batch and runs must be "
                    "non-zero\n");
    return EXIT_FAILURE;
  }

  const bench_case_t cases[] = {
      {"fixed_generic_u64", bench_fixed_generic, 2u},
      {"fixed_generic_wide", bench_fixed_wide, 2u},
      {"fixed_pointer", bench_pointer, 2u},
      {"bulk_generic_u64", bench_bulk, 2u},
      {"growable_push", bench_growth, 1u},
      {"random_mix", bench_random_mix, 1u},
  };

  const size_t ncases = sizeof(cases) / sizeof(cases[0]);

  double *samples = (double *)malloc(cfg.runs * sizeof(*samples));
  if (!samples) {
    fprintf(stderr, "out of memory\n");
    return EXIT_FAILURE;
  }

  printf("benchmark start: items=%zu capacity=%zu batch=%zu runs=%zu "
         "cache_line=%zu\n",
         cfg.items, cfg.capacity, cfg.batch, cfg.runs,
         (size_t)DV_CACHE_LINE_SIZE);

  for (size_t i = 0u; i < ncases; ++i)
    run_case(&cases[i], &cfg, samples);

  free(samples);
  return EXIT_SUCCESS;
}
