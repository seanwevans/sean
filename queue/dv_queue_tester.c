// dv_queue_tester.c

#include "dv_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BATCH_SIZE 16u

static mpmc_queue_t *g_queue = NULL;

static atomic_uint_least64_t g_producer_sum = 0;
static atomic_uint_least64_t g_consumer_sum = 0;
static atomic_uint_least64_t g_producer_xor = 0;
static atomic_uint_least64_t g_consumer_xor = 0;
static atomic_size_t g_produced_count = 0;
static atomic_size_t g_consumed_count = 0;
static atomic_bool g_producers_done = false;

typedef struct {
  mpmc_queue_t *queue;
  size_t *count_slot;
} consumer_thread_args_t;

static void fail_pthread(int err, const char *what) {
  if (err == 0)
    return;

  fprintf(stderr, "%s failed: %s\n", what, strerror(err));
  if (g_queue) {
    mpmc_free(g_queue);
    g_queue = NULL;
  }
  exit(EXIT_FAILURE);
}

static void nanosleep_robust(const struct timespec *req) {
  struct timespec rem = *req;
  while (nanosleep(&rem, &rem) == -1) {
    if (errno != EINTR) {
      perror("nanosleep");
      return;
    }
  }
}

static uint64_t expected_sum_for_thread(size_t thread_id) {
  const uint64_t n = (uint64_t)ITEMS_PER_THREAD;
  const uint64_t first = ((uint64_t)thread_id * n) + 1u;
  const uint64_t last = first + n - 1u;
  return (n * (first + last)) / 2u;
}

static uint64_t expected_xor_upto(uint64_t n) {
  switch (n & 3u) {
  case 0u:
    return n;
  case 1u:
    return 1u;
  case 2u:
    return n + 1u;
  default:
    return 0u;
  }
}

static uint64_t expected_xor_range(uint64_t first, uint64_t last) {
  if (first == 0u)
    return expected_xor_upto(last);
  return expected_xor_upto(last) ^ expected_xor_upto(first - 1u);
}

static uint64_t expected_xor_for_thread(size_t thread_id) {
  const uint64_t n = (uint64_t)ITEMS_PER_THREAD;
  const uint64_t first = ((uint64_t)thread_id * n) + 1u;
  const uint64_t last = first + n - 1u;
  return expected_xor_range(first, last);
}

static void *producer_thread(void *arg) {
  const size_t thread_id = (size_t)(uintptr_t)arg;
  uint64_t local_sum = 0u;
  uint64_t local_xor = 0u;
  void *batch[BATCH_SIZE];

  size_t current_item = 0u;
  while (current_item < (size_t)ITEMS_PER_THREAD) {
    size_t batch_count = 0u;

    while (batch_count < BATCH_SIZE &&
           current_item < (size_t)ITEMS_PER_THREAD) {
      const uint64_t value =
          ((uint64_t)thread_id * (uint64_t)ITEMS_PER_THREAD) +
          (uint64_t)current_item + 1u;

      uint64_t *value_obj = (uint64_t *)malloc(sizeof(*value_obj));
      if (!value_obj) {
        perror("malloc");
        exit(EXIT_FAILURE);
      }
      *value_obj = value;

      batch[batch_count++] = value_obj;
      local_sum += value;
      local_xor ^= value;
      ++current_item;
    }

    size_t sent_total = 0u;
    while (sent_total < batch_count) {
      const size_t just_sent = mpmc_enqueue_bulk(g_queue, &batch[sent_total],
                                                 batch_count - sent_total);

      if (just_sent > 0u) {
        sent_total += just_sent;
        atomic_fetch_add_explicit(&g_produced_count, just_sent,
                                  memory_order_relaxed);
      }

      if (sent_total < batch_count)
        dv_cpu_relax();
    }
  }

  atomic_fetch_add_explicit(&g_producer_sum, local_sum, memory_order_relaxed);
  atomic_fetch_xor_explicit(&g_producer_xor, local_xor, memory_order_relaxed);

  return NULL;
}

static void *consumer_thread(void *arg) {
  const consumer_thread_args_t *const thread_args =
      (const consumer_thread_args_t *)arg;
  mpmc_queue_t *const queue = thread_args->queue;

  uint64_t local_sum = 0u;
  uint64_t local_xor = 0u;
  size_t local_count = 0u;
  void *batch[BATCH_SIZE];

  const size_t expected_total =
      ((size_t)THREAD_COUNT / 2u) * (size_t)ITEMS_PER_THREAD;

  for (;;) {
    const size_t n = mpmc_dequeue_bulk(queue, batch, BATCH_SIZE);

    if (n > 0u) {
      for (size_t i = 0u; i < n; ++i) {
        uint64_t *value_obj = (uint64_t *)batch[i];
        const uint64_t value = *value_obj;
        local_sum += value;
        local_xor ^= value;
        free(value_obj);
      }
      local_count += n;
      atomic_fetch_add_explicit(&g_consumed_count, n, memory_order_relaxed);
      continue;
    }

    const bool done =
        atomic_load_explicit(&g_producers_done, memory_order_acquire);
    const size_t consumed =
        atomic_load_explicit(&g_consumed_count, memory_order_relaxed);

    if (done && consumed >= expected_total) {
      void *single = NULL;
      if (!mpmc_dequeue(queue, &single))
        break;

      uint64_t *value_obj = (uint64_t *)single;
      const uint64_t value = *value_obj;
      local_sum += value;
      local_xor ^= value;
      free(value_obj);
      ++local_count;
      atomic_fetch_add_explicit(&g_consumed_count, 1u, memory_order_relaxed);
      continue;
    }

    dv_cpu_relax();
  }

  *thread_args->count_slot = local_count;
  atomic_fetch_add_explicit(&g_consumer_sum, local_sum, memory_order_relaxed);
  atomic_fetch_xor_explicit(&g_consumer_xor, local_xor, memory_order_relaxed);
  return NULL;
}

int main(void) {
  if ((THREAD_COUNT % 2u) != 0u || THREAD_COUNT < 2u) {
    fprintf(stderr, "THREAD_COUNT must be an even number >= 2.\n");
    return EXIT_FAILURE;
  }

  const size_t producer_threads = (size_t)THREAD_COUNT / 2u;
  const size_t consumer_threads = (size_t)THREAD_COUNT / 2u;
  const size_t expected_total = producer_threads * (size_t)ITEMS_PER_THREAD;

  uint64_t expected_sum = 0u;
  uint64_t expected_xor = 0u;
  for (size_t i = 0u; i < producer_threads; ++i) {
    expected_sum += expected_sum_for_thread(i);
    expected_xor ^= expected_xor_for_thread(i);
  }

  printf("Initializing\n");
  printf("\tQueue Size:       %zu\n", (size_t)QUEUE_SIZE);
  printf("\tItems/Producer:   %zu\n", (size_t)ITEMS_PER_THREAD);
  printf("\tBatch Size:       %zu\n", (size_t)BATCH_SIZE);
  printf("\tProducer Threads: %zu\n", producer_threads);
  printf("\tConsumer Threads: %zu\n", consumer_threads);

  g_queue = mpmc_init((size_t)QUEUE_SIZE);
  if (!g_queue) {
    fprintf(stderr, "Failed to initialize queue.\n");
    return EXIT_FAILURE;
  }

  pthread_t prod[THREAD_COUNT / 2u];
  pthread_t cons[THREAD_COUNT / 2u];
  size_t consumer_counts[THREAD_COUNT / 2u] = {0u};
  consumer_thread_args_t consumer_args[THREAD_COUNT / 2u];

  printf("Spawning %zu producers and %zu consumers...\n", producer_threads,
         consumer_threads);

  for (size_t i = 0u; i < consumer_threads; ++i) {
    consumer_args[i] = (consumer_thread_args_t){
        .queue = g_queue,
        .count_slot = &consumer_counts[i],
    };
    const int err =
        pthread_create(&cons[i], NULL, consumer_thread, &consumer_args[i]);
    fail_pthread(err, "pthread_create(consumer)");
  }

  for (size_t i = 0u; i < producer_threads; ++i) {
    const int err =
        pthread_create(&prod[i], NULL, producer_thread, (void *)(uintptr_t)i);
    fail_pthread(err, "pthread_create(producer)");
  }

  printf("Monitoring queue size...\n");
  const struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L}; // 100 ms
  for (size_t i = 0u; i < 25u; ++i) {
    nanosleep_robust(&ts);
    const size_t size = mpmc_size(g_queue);
    const size_t produced =
        atomic_load_explicit(&g_produced_count, memory_order_relaxed);
    const size_t consumed =
        atomic_load_explicit(&g_consumed_count, memory_order_relaxed);

    printf("\tSize: %zu | Produced: %zu | Consumed: %zu\n", size, produced,
           consumed);
  }

  for (size_t i = 0u; i < producer_threads; ++i) {
    const int err = pthread_join(prod[i], NULL);
    fail_pthread(err, "pthread_join(producer)");
  }

  atomic_store_explicit(&g_producers_done, true, memory_order_release);
  puts("Producers finished. Waiting for consumers to drain...");

  size_t joined_consumer_items = 0u;
  for (size_t i = 0u; i < consumer_threads; ++i) {
    const int err = pthread_join(cons[i], NULL);
    fail_pthread(err, "pthread_join(consumer)");
    joined_consumer_items += consumer_counts[i];
  }

  const uint64_t producer_sum =
      atomic_load_explicit(&g_producer_sum, memory_order_relaxed);
  const uint64_t consumer_sum =
      atomic_load_explicit(&g_consumer_sum, memory_order_relaxed);
  const uint64_t producer_xor =
      atomic_load_explicit(&g_producer_xor, memory_order_relaxed);
  const uint64_t consumer_xor =
      atomic_load_explicit(&g_consumer_xor, memory_order_relaxed);
  const size_t produced_count =
      atomic_load_explicit(&g_produced_count, memory_order_relaxed);
  const size_t consumed_count =
      atomic_load_explicit(&g_consumed_count, memory_order_relaxed);
  const size_t final_size = mpmc_size(g_queue);

  printf("Results:\n");
  printf("\tExpected Count: %zu\n", expected_total);
  printf("\tProduced Count: %zu\n", produced_count);
  printf("\tConsumed Count: %zu\n", consumed_count);
  printf("\tJoined Count:   %zu\n", joined_consumer_items);
  printf("\tExpected Sum:   %" PRIu64 "\n", expected_sum);
  printf("\tProducer Sum:   %" PRIu64 "\n", producer_sum);
  printf("\tConsumer Sum:   %" PRIu64 "\n", consumer_sum);
  printf("\tExpected XOR:   %" PRIu64 "\n", expected_xor);
  printf("\tProducer XOR:   %" PRIu64 "\n", producer_xor);
  printf("\tConsumer XOR:   %" PRIu64 "\n", consumer_xor);
  printf("\tFinal Size:     %zu\n", final_size);

  mpmc_free(g_queue);
  g_queue = NULL;

  const bool ok = (produced_count == expected_total) &&
                  (consumed_count == expected_total) &&
                  (joined_consumer_items == expected_total) &&
                  (producer_sum == expected_sum) &&
                  (consumer_sum == expected_sum) &&
                  (producer_xor == expected_xor) &&
                  (consumer_xor == expected_xor) && (final_size == 0u);

  if (ok) {
    puts("GREAT SUCCESS!");
    return EXIT_SUCCESS;
  }

  puts("I AM DIE.");
  return EXIT_FAILURE;
}
