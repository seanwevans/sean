#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

extern "C" {
#include "dv_queue_bench.h"
}

#if __has_include("MPMCQueue.h")
#include "MPMCQueue.h"
#define HAVE_RIGTORP 1
#elif __has_include("third_party/rigtorp/MPMCQueue.h")
#include "third_party/rigtorp/MPMCQueue.h"
#define HAVE_RIGTORP 1
#else
#define HAVE_RIGTORP 0
#endif

#if __has_include("atomic_queue/atomic_queue.h")
#include "atomic_queue/atomic_queue.h"
#define HAVE_ATOMIC_QUEUE 1
#elif __has_include("third_party/atomic_queue/include/atomic_queue/atomic_queue.h")
#include "third_party/atomic_queue/include/atomic_queue/atomic_queue.h"
#define HAVE_ATOMIC_QUEUE 1
#else
#define HAVE_ATOMIC_QUEUE 0
#endif

struct BenchConfig {
  size_t producers = 4u;
  size_t consumers = 4u;
  size_t items_per_producer = 10000000u;
  size_t capacity = 65536u;
  size_t runs = 7u;
};

struct BenchResult {
  const char *name = "";
  double seconds = 0.0;
  double ops_per_sec = 0.0;
  uint64_t produced_sum = 0u;
  uint64_t consumed_sum = 0u;
  uint64_t produced_xor = 0u;
  uint64_t consumed_xor = 0u;
  size_t produced_count = 0u;
  size_t consumed_count = 0u;
  bool ok = false;
};

static bool parse_size_arg(const char *s, size_t &out) {
  if (!s || *s == '\0')
    return false;
  char *end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (!end || *end != '\0')
    return false;
  out = static_cast<size_t>(v);
  return true;
}

static BenchConfig parse_args(int argc, char **argv) {
  BenchConfig cfg;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--producers") == 0 && i + 1 < argc) {
      (void)parse_size_arg(argv[++i], cfg.producers);
    } else if (std::strcmp(argv[i], "--consumers") == 0 && i + 1 < argc) {
      (void)parse_size_arg(argv[++i], cfg.consumers);
    } else if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
      (void)parse_size_arg(argv[++i], cfg.items_per_producer);
    } else if (std::strcmp(argv[i], "--capacity") == 0 && i + 1 < argc) {
      (void)parse_size_arg(argv[++i], cfg.capacity);
    } else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
      (void)parse_size_arg(argv[++i], cfg.runs);
    }
  }
  return cfg;
}

static uint64_t xor_upto(uint64_t n) {
  switch (n & 3u) {
  case 0u: return n;
  case 1u: return 1u;
  case 2u: return n + 1u;
  default: return 0u;
  }
}

static uint64_t xor_range(uint64_t first, uint64_t last) {
  return first == 0u ? xor_upto(last) : (xor_upto(last) ^ xor_upto(first - 1u));
}

static uint64_t expected_sum_for_thread(size_t thread_id, size_t items_per_producer) {
  const uint64_t n = static_cast<uint64_t>(items_per_producer);
  const uint64_t first = static_cast<uint64_t>(thread_id) * n + 1u;
  const uint64_t last = first + n - 1u;
  return (n * (first + last)) / 2u;
}

static uint64_t expected_xor_for_thread(size_t thread_id, size_t items_per_producer) {
  const uint64_t n = static_cast<uint64_t>(items_per_producer);
  const uint64_t first = static_cast<uint64_t>(thread_id) * n + 1u;
  const uint64_t last = first + n - 1u;
  return xor_range(first, last);
}

static void pin_thread_round_robin(size_t index) {
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  const unsigned hw = std::thread::hardware_concurrency();
  const unsigned cpu = (hw > 0u) ? static_cast<unsigned>(index % hw) : 0u;
  CPU_SET(cpu, &cpuset);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
  (void)index;
#endif
}

struct IQueue {
  virtual ~IQueue() = default;
  virtual bool enqueue(uintptr_t value) = 0;
  virtual bool dequeue(uintptr_t &value) = 0;
  virtual const char *name() const = 0;
};

struct DVQueueAdapter final : IQueue {
  dvq_bench_handle *h = nullptr;

  explicit DVQueueAdapter(size_t capacity) { h = dvq_bench_create(capacity); }
  ~DVQueueAdapter() override { dvq_bench_destroy(h); }
  bool good() const { return h != nullptr; }

  bool enqueue(uintptr_t value) override {
    return dvq_bench_enqueue(h, value);
  }

  bool dequeue(uintptr_t &value) override {
    return dvq_bench_dequeue(h, &value);
  }

  const char *name() const override { return "dv_queue"; }
};

#if HAVE_RIGTORP
struct RigtorpAdapter final : IQueue {
  rigtorp::MPMCQueue<uintptr_t> q;
  explicit RigtorpAdapter(size_t capacity) : q(capacity) {}
  bool enqueue(uintptr_t value) override { return q.try_push(value); }
  bool dequeue(uintptr_t &value) override { return q.try_pop(value); }
  const char *name() const override { return "rigtorp_mpmc"; }
};
#endif

#if HAVE_ATOMIC_QUEUE
template <typename Q>
struct AtomicQueueAdapter final : IQueue {
  Q q;
  explicit AtomicQueueAdapter(size_t capacity) : q(static_cast<unsigned>(capacity)) {}
  bool enqueue(uintptr_t value) override { return q.try_push(value); }
  bool dequeue(uintptr_t &value) override { return q.try_pop(value); }
  const char *name() const override { return "atomic_queue"; }
};
#endif

static BenchResult run_once(IQueue &q, const BenchConfig &cfg) {
  std::atomic<bool> start{false};
  std::atomic<bool> producers_done{false};

  std::atomic<uint64_t> producer_sum{0u};
  std::atomic<uint64_t> consumer_sum{0u};
  std::atomic<uint64_t> producer_xor{0u};
  std::atomic<uint64_t> consumer_xor{0u};
  std::atomic<size_t> produced_count{0u};
  std::atomic<size_t> consumed_count{0u};

  const size_t expected_total = cfg.producers * cfg.items_per_producer;

  std::vector<std::thread> consumers;
  std::vector<std::thread> producers;
  consumers.reserve(cfg.consumers);
  producers.reserve(cfg.producers);

  for (size_t t = 0u; t < cfg.consumers; ++t) {
    consumers.emplace_back([&, t]() {
      pin_thread_round_robin(t);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

      uint64_t local_sum = 0u;
      uint64_t local_xor = 0u;

      for (;;) {
        uintptr_t value = 0u;
        if (q.dequeue(value)) {
          local_sum += static_cast<uint64_t>(value);
          local_xor ^= static_cast<uint64_t>(value);
          consumed_count.fetch_add(1u, std::memory_order_relaxed);
          continue;
        }

        const bool done = producers_done.load(std::memory_order_acquire);
        const size_t consumed = consumed_count.load(std::memory_order_relaxed);
        if (done && consumed >= expected_total)
          break;

        std::this_thread::yield();
      }

      consumer_sum.fetch_add(local_sum, std::memory_order_relaxed);
      consumer_xor.fetch_xor(local_xor, std::memory_order_relaxed);
    });
  }

  for (size_t t = 0u; t < cfg.producers; ++t) {
    producers.emplace_back([&, t]() {
      pin_thread_round_robin(cfg.consumers + t);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

      uint64_t local_sum = 0u;
      uint64_t local_xor = 0u;

      for (size_t i = 0u; i < cfg.items_per_producer; ++i) {
        const uintptr_t value =
            static_cast<uintptr_t>(static_cast<uint64_t>(t) * cfg.items_per_producer + i + 1u);

        while (!q.enqueue(value))
          std::this_thread::yield();

        local_sum += static_cast<uint64_t>(value);
        local_xor ^= static_cast<uint64_t>(value);
        produced_count.fetch_add(1u, std::memory_order_relaxed);
      }

      producer_sum.fetch_add(local_sum, std::memory_order_relaxed);
      producer_xor.fetch_xor(local_xor, std::memory_order_relaxed);
    });
  }

  const auto t0 = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);

  for (auto &th : producers)
    th.join();

  producers_done.store(true, std::memory_order_release);

  for (auto &th : consumers)
    th.join();

  const auto t1 = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();

  uint64_t expected_sum = 0u;
  uint64_t expected_xor = 0u;
  for (size_t t = 0u; t < cfg.producers; ++t) {
    expected_sum += expected_sum_for_thread(t, cfg.items_per_producer);
    expected_xor ^= expected_xor_for_thread(t, cfg.items_per_producer);
  }

  BenchResult r;
  r.name = q.name();
  r.seconds = seconds;
  r.ops_per_sec = seconds > 0.0 ? (2.0 * static_cast<double>(expected_total) / seconds) : 0.0;
  r.produced_sum = producer_sum.load(std::memory_order_relaxed);
  r.consumed_sum = consumer_sum.load(std::memory_order_relaxed);
  r.produced_xor = producer_xor.load(std::memory_order_relaxed);
  r.consumed_xor = consumer_xor.load(std::memory_order_relaxed);
  r.produced_count = produced_count.load(std::memory_order_relaxed);
  r.consumed_count = consumed_count.load(std::memory_order_relaxed);
  r.ok =
      (r.produced_count == expected_total) &&
      (r.consumed_count == expected_total) &&
      (r.produced_sum == expected_sum) &&
      (r.consumed_sum == expected_sum) &&
      (r.produced_xor == expected_xor) &&
      (r.consumed_xor == expected_xor);

  return r;
}

template <typename MakeQueue>
static void bench_backend(const BenchConfig &cfg, MakeQueue make_queue, const char *name) {
  std::vector<double> samples;
  samples.reserve(cfg.runs);

  BenchResult last{};
  for (size_t run = 0u; run < cfg.runs; ++run) {
    std::unique_ptr<IQueue> q = make_queue();
    if (!q) {
      std::fprintf(stderr, "failed to init backend=%s\n", name);
      return;
    }

    last = run_once(*q, cfg);
    if (!last.ok) {
      std::fprintf(stderr,
                   "backend=%s run=%zu FAILED produced=%zu consumed=%zu "
                   "psum=%" PRIu64 " csum=%" PRIu64 " pxor=%" PRIu64 " cxor=%" PRIu64 "\n",
                   name, run, last.produced_count, last.consumed_count,
                   last.produced_sum, last.consumed_sum,
                   last.produced_xor, last.consumed_xor);
      return;
    }

    samples.push_back(last.ops_per_sec);
  }

  std::sort(samples.begin(), samples.end());
  const double median = samples[samples.size() / 2u];
  const double best = samples.back();
  const double worst = samples.front();

  std::printf(
      "queue=%s prod=%zu cons=%zu items=%zu cap=%zu runs=%zu "
      "median_ops_per_sec=%.3f best=%.3f worst=%.3f seconds_last=%.6f\n",
      name, cfg.producers, cfg.consumers, cfg.items_per_producer, cfg.capacity,
      cfg.runs, median, best, worst, last.seconds);
}

int main(int argc, char **argv) {
  const BenchConfig cfg = parse_args(argc, argv);

  if (cfg.producers == 0u || cfg.consumers == 0u ||
      cfg.items_per_producer == 0u || cfg.capacity < 2u) {
    std::fprintf(stderr, "bad config\n");
    return EXIT_FAILURE;
  }

  if ((cfg.capacity & (cfg.capacity - 1u)) != 0u) {
    std::fprintf(stderr, "capacity must be a power of two for dv_queue\n");
    return EXIT_FAILURE;
  }

  std::printf("benchmark start: prod=%zu cons=%zu items=%zu cap=%zu runs=%zu\n",
              cfg.producers, cfg.consumers, cfg.items_per_producer,
              cfg.capacity, cfg.runs);

  bench_backend(
      cfg,
      [&]() -> std::unique_ptr<IQueue> {
        auto p = std::make_unique<DVQueueAdapter>(cfg.capacity);
        if (!p->good())
          return {};
        return p;
      },
      "dv_queue");

#if HAVE_RIGTORP
  bench_backend(
      cfg,
      [&]() -> std::unique_ptr<IQueue> {
        return std::make_unique<RigtorpAdapter>(cfg.capacity);
      },
      "rigtorp_mpmc");
#else
  std::printf("queue=rigtorp_mpmc skipped (header not found)\n");
#endif

#if HAVE_ATOMIC_QUEUE
  bench_backend(
      cfg,
      [&]() -> std::unique_ptr<IQueue> {
        using Q = atomic_queue::AtomicQueueB2<uintptr_t>;
        return std::make_unique<AtomicQueueAdapter<Q>>(cfg.capacity);
      },
      "atomic_queue");
#else
  std::printf("queue=atomic_queue skipped (header not found)\n");
#endif

  return EXIT_SUCCESS;
}