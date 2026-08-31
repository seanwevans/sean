CC      ?= cc
AR      ?= ar

CACHE_SIZE ?= 64
VERTICES   ?= 65536
EDGES      ?= 524288
CYCLES     ?= 8
THREADS    ?= 8
BATCH      ?= 64

# Architecture tuning for optimized builds. Defaults to the host CPU, which
# produces the fastest binaries but ties them to the building machine and can
# fault (SIGILL) if run elsewhere. Override for portable builds, e.g.
#   make release ARCHFLAGS=            (generic baseline)
#   make release ARCHFLAGS="-march=x86-64-v2"
ARCHFLAGS ?= -march=native -mtune=native

CPPFLAGS := -DGRAPH_VERTICES=$(VERTICES) \
            -DGRAPH_EDGES=$(EDGES) \
            -DGRAPH_CYCLES=$(CYCLES) \
            -DGRAPH_THREADS=$(THREADS) \
            -DBATCH_SIZE=$(BATCH) \
            -DDV_CACHE_LINE_SIZE=$(CACHE_SIZE)

WARNFLAGS := -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
             -Wshadow -Wdouble-promotion -Wnull-dereference -Wformat=2

COMMON_CFLAGS   := -std=c2x -pthread -MMD -MP
COMMON_LDFLAGS  := -pthread

RELEASE_CFLAGS  := $(COMMON_CFLAGS) $(WARNFLAGS) -O3 $(ARCHFLAGS) -DNDEBUG
RELEASE_LDFLAGS := $(COMMON_LDFLAGS)

DEBUG_CFLAGS    := $(COMMON_CFLAGS) $(WARNFLAGS) -g3 -O0
DEBUG_LDFLAGS   := $(COMMON_LDFLAGS)

TSAN_CFLAGS     := $(DEBUG_CFLAGS) -fsanitize=thread
TSAN_LDFLAGS    := $(DEBUG_LDFLAGS) -fsanitize=thread

UBSAN_CFLAGS    := $(DEBUG_CFLAGS) -fsanitize=undefined
UBSAN_LDFLAGS   := $(DEBUG_LDFLAGS) -fsanitize=undefined

ASAN_CFLAGS     := $(DEBUG_CFLAGS) -fsanitize=address,undefined
ASAN_LDFLAGS    := $(DEBUG_LDFLAGS) -fsanitize=address,undefined

# --- Library Target ---
LIB_TARGET := libsean_graph.a
LIB_SRCS   := dv_graph.c dv_graph_mt.c
LIB_OBJS   := $(LIB_SRCS:.c=.o)
LIB_DEPS   := $(LIB_OBJS:.o=.d)

# --- Soak Harness ---
TARGET  := graph_test
SRCS    := dv_graph_tester.c
OBJS    := $(SRCS:.c=.o)
DEPS    := $(OBJS:.o=.d)

# --- Unit Tests ---
UNIT_TEST_TARGET := unit_tests
UNIT_TEST_SRCS   := unit_tests.c
UNIT_TEST_OBJS   := $(UNIT_TEST_SRCS:.c=.o)
UNIT_TEST_DEPS   := $(UNIT_TEST_OBJS:.o=.d)

# --- Benchmarks ---
BENCH_TARGET := bench
BENCH_SRCS   := dv_graph_bench.c
BENCH_OBJS   := dv_graph_bench_run.o
BENCH_DEPS   := $(BENCH_OBJS:.o=.d)
BENCH_CFLAGS := -std=c2x -pthread -MMD -MP $(WARNFLAGS) -O3 $(ARCHFLAGS) -DNDEBUG

.PHONY: all debug release tsan ubsan asan clean \
        run run-release run-tsan run-ubsan run-asan \
        unit run-unit \
        bench run-bench clean-bench

all: debug

debug: CFLAGS := $(DEBUG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: $(LIB_TARGET) $(TARGET)

release: CFLAGS := $(RELEASE_CFLAGS)
release: LDFLAGS := $(RELEASE_LDFLAGS)
release: clean $(LIB_TARGET) $(TARGET)

tsan: CFLAGS := $(TSAN_CFLAGS)
tsan: LDFLAGS := $(TSAN_LDFLAGS)
tsan: clean $(LIB_TARGET) $(TARGET) $(UNIT_TEST_TARGET)

ubsan: CFLAGS := $(UBSAN_CFLAGS)
ubsan: LDFLAGS := $(UBSAN_LDFLAGS)
ubsan: clean $(LIB_TARGET) $(TARGET) $(UNIT_TEST_TARGET)

asan: CFLAGS := $(ASAN_CFLAGS)
asan: LDFLAGS := $(ASAN_LDFLAGS)
asan: clean $(LIB_TARGET) $(TARGET) $(UNIT_TEST_TARGET)

# Build libsean_graph.a
$(LIB_TARGET): $(LIB_OBJS)
	$(AR) rcs $@ $^

# General C compilation
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Link the soak harness against the library
$(TARGET): $(OBJS) $(LIB_TARGET)
	$(CC) $(OBJS) -L. -lsean_graph $(LDFLAGS) -o $@

# Link the unit tests against the library
unit: CFLAGS := $(DEBUG_CFLAGS)
unit: LDFLAGS := $(DEBUG_LDFLAGS)
unit: $(LIB_TARGET) $(UNIT_TEST_TARGET)

$(UNIT_TEST_TARGET): $(UNIT_TEST_OBJS) $(LIB_TARGET)
	$(CC) $(UNIT_TEST_OBJS) -L. -lsean_graph $(LDFLAGS) -o $@

# Run commands. The sanitizer targets build both binaries and run each, since
# the unit tests cover the allocator contracts and the harness covers scale.
run: debug
	./$(TARGET)

run-release: release
	./$(TARGET)

run-tsan: tsan
	./$(UNIT_TEST_TARGET)
	./$(TARGET)

run-ubsan: ubsan
	./$(UNIT_TEST_TARGET)
	./$(TARGET)

run-asan: asan
	./$(UNIT_TEST_TARGET)
	./$(TARGET)

run-unit: unit
	./$(UNIT_TEST_TARGET)

# Benchmarks are always optimized; a debug build measures the compiler, not
# the data structure.
bench: $(BENCH_TARGET)

dv_graph_bench_run.o: dv_graph_bench.c
	$(CC) $(BENCH_CFLAGS) $(CPPFLAGS) -c $< -o $@

dv_graph_bench_lib.o: dv_graph.c
	$(CC) $(BENCH_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BENCH_TARGET): $(BENCH_OBJS) dv_graph_bench_lib.o
	$(CC) $(BENCH_OBJS) dv_graph_bench_lib.o $(COMMON_LDFLAGS) -o $@

run-bench: bench
	./$(BENCH_TARGET)

clean-bench:
	rm -f $(BENCH_TARGET) $(BENCH_OBJS) dv_graph_bench_lib.o \
	      $(BENCH_DEPS) dv_graph_bench_lib.d

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) \
	      $(UNIT_TEST_OBJS) $(UNIT_TEST_DEPS) $(UNIT_TEST_TARGET) \
	      $(BENCH_TARGET) $(BENCH_OBJS) dv_graph_bench_lib.o \
	      $(BENCH_DEPS) dv_graph_bench_lib.d \
	      $(LIB_TARGET) $(LIB_OBJS) $(LIB_DEPS)

-include $(DEPS) $(UNIT_TEST_DEPS) $(BENCH_DEPS) $(LIB_DEPS) dv_graph_bench_lib.d
