// unit_tests.c

#include "dv_queue.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_QUEUE_SIZE 8u
#define SMOKE_THREADS 4u
#define SMOKE_ITEMS_PER_THREAD 10000u

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

#define ASSERT_PTR_EQ(a, b)                                                    \
  do {                                                                         \
    void *_a = (void *)(a);                                                    \
    void *_b = (void *)(b);                                                    \
    if (_a != _b) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_PTR_EQ failed: %s != %s (got=%p expected=%p) (%s:%d)\n", \
              #a, #b, _a, _b, __FILE__, __LINE__);                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

typedef bool (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} test_case_t;

static bool test_init_rejects_bad_sizes(void) {
  ASSERT_TRUE(mpmc_init(0u) == NULL);
  ASSERT_TRUE(mpmc_init(1u) == NULL);
  ASSERT_TRUE(mpmc_init(3u) == NULL);
  ASSERT_TRUE(mpmc_init(6u) == NULL);

  mpmc_queue_t *q = mpmc_init(2u);
  ASSERT_TRUE(q != NULL);
  mpmc_free(q);

  q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);
  mpmc_free(q);

  return true;
}

static bool test_size_empty_and_null_handling(void) {
  void *out = (void *)0x1;

  ASSERT_EQ_U64(mpmc_size(NULL), 0u);
  ASSERT_FALSE(mpmc_enqueue(NULL, (void *)(uintptr_t)123u));
  ASSERT_FALSE(mpmc_dequeue(NULL, &out));
  ASSERT_FALSE(mpmc_dequeue(NULL, NULL));
  ASSERT_FALSE(mpmc_dequeue((mpmc_queue_t *)0x1, NULL));
  ASSERT_EQ_U64(mpmc_enqueue_bulk(NULL, NULL, 4u), 0u);
  ASSERT_EQ_U64(mpmc_dequeue_bulk(NULL, NULL, 4u), 0u);

  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  ASSERT_EQ_U64(mpmc_size(q), 0u);
  out = (void *)0x1;
  ASSERT_FALSE(mpmc_dequeue(q, &out));
  ASSERT_PTR_EQ(out, NULL);

  mpmc_free(q);
  return true;
}

static bool test_single_enqueue_dequeue(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  void *in = (void *)(uintptr_t)0x1234u;
  void *out = NULL;

  ASSERT_TRUE(mpmc_enqueue(q, in));
  ASSERT_EQ_U64(mpmc_size(q), 1u);

  ASSERT_TRUE(mpmc_dequeue(q, &out));
  ASSERT_PTR_EQ(out, in);
  ASSERT_EQ_U64(mpmc_size(q), 0u);

  out = (void *)0x1;
  ASSERT_FALSE(mpmc_dequeue(q, &out));
  ASSERT_PTR_EQ(out, NULL);

  mpmc_free(q);
  return true;
}

static bool test_fifo_order_small(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  for (uintptr_t i = 1u; i <= 4u; ++i) {
    ASSERT_TRUE(mpmc_enqueue(q, (void *)i));
  }

  for (uintptr_t i = 1u; i <= 4u; ++i) {
    void *out = NULL;
    ASSERT_TRUE(mpmc_dequeue(q, &out));
    ASSERT_PTR_EQ(out, (void *)i);
  }

  ASSERT_EQ_U64(mpmc_size(q), 0u);
  mpmc_free(q);
  return true;
}

static bool test_queue_full_and_empty_boundaries(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  for (uintptr_t i = 0u; i < TEST_QUEUE_SIZE; ++i) {
    ASSERT_TRUE(mpmc_enqueue(q, (void *)(i + 1u)));
  }

  ASSERT_EQ_U64(mpmc_size(q), TEST_QUEUE_SIZE);
  ASSERT_FALSE(mpmc_enqueue(q, (void *)(uintptr_t)999u));

  for (uintptr_t i = 0u; i < TEST_QUEUE_SIZE; ++i) {
    void *out = NULL;
    ASSERT_TRUE(mpmc_dequeue(q, &out));
    ASSERT_PTR_EQ(out, (void *)(i + 1u));
  }

  ASSERT_EQ_U64(mpmc_size(q), 0u);

  void *out = (void *)0x1;
  ASSERT_FALSE(mpmc_dequeue(q, &out));
  ASSERT_PTR_EQ(out, NULL);

  mpmc_free(q);
  return true;
}

static bool test_wraparound_behavior(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  for (uintptr_t round = 0u; round < 32u; ++round) {
    for (uintptr_t i = 0u; i < TEST_QUEUE_SIZE; ++i) {
      const uintptr_t value = round * 100u + i + 1u;
      ASSERT_TRUE(mpmc_enqueue(q, (void *)value));
    }

    for (uintptr_t i = 0u; i < TEST_QUEUE_SIZE; ++i) {
      const uintptr_t expected = round * 100u + i + 1u;
      void *out = NULL;
      ASSERT_TRUE(mpmc_dequeue(q, &out));
      ASSERT_PTR_EQ(out, (void *)expected);
    }

    ASSERT_EQ_U64(mpmc_size(q), 0u);
  }

  mpmc_free(q);
  return true;
}

static bool test_enqueue_bulk_partial_on_full(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  void *items[12];
  for (uintptr_t i = 0u; i < 12u; ++i)
    items[i] = (void *)(i + 1u);

  const size_t pushed = mpmc_enqueue_bulk(q, items, 12u);
  ASSERT_EQ_U64(pushed, TEST_QUEUE_SIZE);
  ASSERT_EQ_U64(mpmc_size(q), TEST_QUEUE_SIZE);

  for (uintptr_t i = 0u; i < TEST_QUEUE_SIZE; ++i) {
    void *out = NULL;
    ASSERT_TRUE(mpmc_dequeue(q, &out));
    ASSERT_PTR_EQ(out, (void *)(i + 1u));
  }

  mpmc_free(q);
  return true;
}

static bool test_dequeue_bulk_partial_on_empty(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  for (uintptr_t i = 0u; i < 3u; ++i) {
    ASSERT_TRUE(mpmc_enqueue(q, (void *)(i + 10u)));
  }

  void *items[8];
  memset(items, 0xA5, sizeof(items));

  const size_t popped = mpmc_dequeue_bulk(q, items, 8u);
  ASSERT_EQ_U64(popped, 3u);
  ASSERT_PTR_EQ(items[0], (void *)10u);
  ASSERT_PTR_EQ(items[1], (void *)11u);
  ASSERT_PTR_EQ(items[2], (void *)12u);
  ASSERT_EQ_U64(mpmc_size(q), 0u);

  mpmc_free(q);
  return true;
}

static bool test_bulk_zero_count_is_noop(void) {
  mpmc_queue_t *q = mpmc_init(TEST_QUEUE_SIZE);
  ASSERT_TRUE(q != NULL);

  void *items[2] = {(void *)1u, (void *)2u};

  ASSERT_EQ_U64(mpmc_enqueue_bulk(q, items, 0u), 0u);
  ASSERT_EQ_U64(mpmc_dequeue_bulk(q, items, 0u), 0u);
  ASSERT_EQ_U64(mpmc_size(q), 0u);

  mpmc_free(q);
  return true;
}

typedef struct {
  mpmc_queue_t *q;
  size_t thread_id;
} smoke_arg_t;

static atomic_uint_least64_t smoke_prod_sum = 0;
static atomic_uint_least64_t smoke_cons_sum = 0;
static atomic_size_t smoke_prod_count = 0;
static atomic_size_t smoke_cons_count = 0;
static atomic_bool smoke_done = false;

static void *smoke_producer(void *arg) {
  smoke_arg_t *a = (smoke_arg_t *)arg;
  uint64_t local_sum = 0u;

  for (size_t i = 0u; i < SMOKE_ITEMS_PER_THREAD; ++i) {
    const uint64_t value =
        ((uint64_t)a->thread_id * (uint64_t)SMOKE_ITEMS_PER_THREAD) + i + 1u;

    while (!mpmc_enqueue(a->q, (void *)(uintptr_t)value))
      dv_cpu_relax();

    local_sum += value;
    atomic_fetch_add_explicit(&smoke_prod_count, 1u, memory_order_relaxed);
  }

  atomic_fetch_add_explicit(&smoke_prod_sum, local_sum, memory_order_relaxed);
  return NULL;
}

static void *smoke_consumer(void *arg) {
  smoke_arg_t *a = (smoke_arg_t *)arg;
  uint64_t local_sum = 0u;
  const size_t expected_total = SMOKE_THREADS * SMOKE_ITEMS_PER_THREAD;

  for (;;) {
    void *out = NULL;
    if (mpmc_dequeue(a->q, &out)) {
      local_sum += (uint64_t)(uintptr_t)out;
      atomic_fetch_add_explicit(&smoke_cons_count, 1u, memory_order_relaxed);
      continue;
    }

    const bool done = atomic_load_explicit(&smoke_done, memory_order_acquire);
    const size_t consumed =
        atomic_load_explicit(&smoke_cons_count, memory_order_relaxed);

    if (done && consumed >= expected_total)
      break;

    dv_cpu_relax();
  }

  atomic_fetch_add_explicit(&smoke_cons_sum, local_sum, memory_order_relaxed);
  return NULL;
}

static bool test_concurrent_smoke(void) {
  mpmc_queue_t *q = mpmc_init(1024u);
  ASSERT_TRUE(q != NULL);

  atomic_store_explicit(&smoke_prod_sum, 0u, memory_order_relaxed);
  atomic_store_explicit(&smoke_cons_sum, 0u, memory_order_relaxed);
  atomic_store_explicit(&smoke_prod_count, 0u, memory_order_relaxed);
  atomic_store_explicit(&smoke_cons_count, 0u, memory_order_relaxed);
  atomic_store_explicit(&smoke_done, false, memory_order_relaxed);

  pthread_t prod[SMOKE_THREADS];
  pthread_t cons[SMOKE_THREADS];
  smoke_arg_t pargs[SMOKE_THREADS];
  smoke_arg_t cargs[SMOKE_THREADS];

  for (size_t i = 0u; i < SMOKE_THREADS; ++i) {
    pargs[i].q = q;
    pargs[i].thread_id = i;
    cargs[i].q = q;
    cargs[i].thread_id = i;
  }

  for (size_t i = 0u; i < SMOKE_THREADS; ++i) {
    ASSERT_EQ_U64(pthread_create(&cons[i], NULL, smoke_consumer, &cargs[i]), 0);
  }

  for (size_t i = 0u; i < SMOKE_THREADS; ++i) {
    ASSERT_EQ_U64(pthread_create(&prod[i], NULL, smoke_producer, &pargs[i]), 0);
  }

  for (size_t i = 0u; i < SMOKE_THREADS; ++i) {
    ASSERT_EQ_U64(pthread_join(prod[i], NULL), 0);
  }

  atomic_store_explicit(&smoke_done, true, memory_order_release);

  for (size_t i = 0u; i < SMOKE_THREADS; ++i) {
    ASSERT_EQ_U64(pthread_join(cons[i], NULL), 0);
  }

  const size_t expected_count = SMOKE_THREADS * SMOKE_ITEMS_PER_THREAD;
  uint64_t expected_sum = 0u;
  for (size_t t = 0u; t < SMOKE_THREADS; ++t) {
    const uint64_t first =
        ((uint64_t)t * (uint64_t)SMOKE_ITEMS_PER_THREAD) + 1u;
    const uint64_t last = first + (uint64_t)SMOKE_ITEMS_PER_THREAD - 1u;
    expected_sum += ((uint64_t)SMOKE_ITEMS_PER_THREAD * (first + last)) / 2u;
  }

  ASSERT_EQ_U64(atomic_load_explicit(&smoke_prod_count, memory_order_relaxed),
                expected_count);
  ASSERT_EQ_U64(atomic_load_explicit(&smoke_cons_count, memory_order_relaxed),
                expected_count);
  ASSERT_EQ_U64(atomic_load_explicit(&smoke_prod_sum, memory_order_relaxed),
                expected_sum);
  ASSERT_EQ_U64(atomic_load_explicit(&smoke_cons_sum, memory_order_relaxed),
                expected_sum);
  ASSERT_EQ_U64(mpmc_size(q), 0u);

  mpmc_free(q);
  return true;
}

int main(void) {
  const test_case_t tests[] = {
      {"init rejects bad sizes", test_init_rejects_bad_sizes},
      {"size empty and null handling", test_size_empty_and_null_handling},
      {"single enqueue dequeue", test_single_enqueue_dequeue},
      {"fifo order small", test_fifo_order_small},
      {"queue full and empty boundaries", test_queue_full_and_empty_boundaries},
      {"wraparound behavior", test_wraparound_behavior},
      {"enqueue bulk partial on full", test_enqueue_bulk_partial_on_full},
      {"dequeue bulk partial on empty", test_dequeue_bulk_partial_on_empty},
      {"bulk zero count is noop", test_bulk_zero_count_is_noop},
      {"concurrent smoke", test_concurrent_smoke},
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