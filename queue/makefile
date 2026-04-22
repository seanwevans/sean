CC      ?= cc
CXX     ?= g++
AR      ?= ar

CACHE_SIZE ?= 64
THREADS    ?= 8
ITEMS      ?= 10000000
QSIZE      ?= 65536

CPPFLAGS := -DTHREAD_COUNT=$(THREADS) \
            -DITEMS_PER_THREAD=$(ITEMS) \
            -DQUEUE_SIZE=$(QSIZE) \
            -DDV_CACHE_LINE_SIZE=$(CACHE_SIZE)

WARNFLAGS := -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
             -Wshadow -Wdouble-promotion -Wnull-dereference -Wformat=2

COMMON_CFLAGS   := -std=c2x -pthread -MMD -MP
COMMON_LDFLAGS  := -pthread

RELEASE_CFLAGS  := $(COMMON_CFLAGS) $(WARNFLAGS) -O3 -march=native -mtune=native -DNDEBUG
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
LIB_TARGET := libsean.a
LIB_SRCS   := dv_queue.c
# Future additions go here:
# LIB_SRCS += stack.c heap.c graph.c
LIB_OBJS   := $(LIB_SRCS:.c=.o)
LIB_DEPS   := $(LIB_OBJS:.o=.d)

# --- Queue Test ---
TARGET  := queue_test
SRCS    := dv_queue_tester.c
OBJS    := $(SRCS:.c=.o)
DEPS    := $(OBJS:.o=.d)

# --- Unit Tests ---
UNIT_TEST_TARGET := unit_tests
UNIT_TEST_SRCS   := unit_tests.c
UNIT_TEST_OBJS   := $(UNIT_TEST_SRCS:.c=.o)
UNIT_TEST_DEPS   := $(UNIT_TEST_OBJS:.o=.d)

# --- Benchmarks ---
BENCH_TARGET    := bench
BENCH_CPP       := benchmark_queues.cpp
BENCH_CPP_OBJ   := benchmark_queues.o
BENCH_C_OBJS    := dv_queue_bench_wrap.o
BENCH_DEPS      := $(BENCH_CPP_OBJ:.o=.d) $(BENCH_C_OBJS:.o=.d)

RIGTORP_INC      ?= third_party/rigtorp
ATOMIC_QUEUE_INC ?= third_party/atomic_queue/include

BENCH_CPPFLAGS := $(CPPFLAGS) -I. -I$(RIGTORP_INC) -I$(ATOMIC_QUEUE_INC)
BENCH_CFLAGS   := -std=c2x -pthread -MMD -MP $(WARNFLAGS) -O3 -march=native -mtune=native -DNDEBUG
BENCH_CXXFLAGS := -pthread -MMD -MP -O3 -DNDEBUG -march=native -mtune=native -std=c++17 -Wall -Wextra
BENCH_LDFLAGS  := -pthread

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
tsan: clean $(LIB_TARGET) $(TARGET)

ubsan: CFLAGS := $(UBSAN_CFLAGS)
ubsan: LDFLAGS := $(UBSAN_LDFLAGS)
ubsan: clean $(LIB_TARGET) $(TARGET)

asan: CFLAGS := $(ASAN_CFLAGS)
asan: LDFLAGS := $(ASAN_LDFLAGS)
asan: clean $(LIB_TARGET) $(TARGET)

# Build libsean.a
$(LIB_TARGET): $(LIB_OBJS)
	$(AR) rcs $@ $^

# General C compilation
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Run commands
run: debug
	./$(TARGET)

run-release: release
	./$(TARGET)

run-tsan: tsan
	./$(TARGET)

run-ubsan: ubsan
	./$(TARGET)

run-asan: asan
	./$(TARGET)

# Link Queue Test against libsean
$(TARGET): $(OBJS) $(LIB_TARGET)
	$(CC) $(OBJS) -L. -lsean $(LDFLAGS) -o $@

# Link Unit Tests against libsean
unit: CFLAGS := $(DEBUG_CFLAGS)
unit: LDFLAGS := $(DEBUG_LDFLAGS)
unit: $(LIB_TARGET) $(UNIT_TEST_TARGET)

$(UNIT_TEST_TARGET): $(UNIT_TEST_OBJS) $(LIB_TARGET)
	$(CC) $(UNIT_TEST_OBJS) -L. -lsean $(LDFLAGS) -o $@

run-unit: unit
	./$(UNIT_TEST_TARGET)

# Link Benchmarks against libsean
bench: CFLAGS := $(BENCH_CFLAGS)
bench: LDFLAGS := $(BENCH_LDFLAGS)
bench: $(LIB_TARGET) $(BENCH_TARGET)

dv_queue_bench_wrap.o: dv_queue_bench.c
	$(CC) $(BENCH_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BENCH_CPP_OBJ): $(BENCH_CPP)
	$(CXX) $(BENCH_CPPFLAGS) $(BENCH_CXXFLAGS) -c $< -o $@

$(BENCH_TARGET): $(BENCH_CPP_OBJ) $(BENCH_C_OBJS) $(LIB_TARGET)
	$(CXX) $(BENCH_CPP_OBJ) $(BENCH_C_OBJS) -L. -lsean $(BENCH_LDFLAGS) -o $@

run-bench: bench
	./$(BENCH_TARGET)

clean-bench:
	rm -f $(BENCH_TARGET) $(BENCH_CPP_OBJ) $(BENCH_C_OBJS) $(BENCH_DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) \
	      $(UNIT_TEST_OBJS) $(UNIT_TEST_DEPS) $(UNIT_TEST_TARGET) \
	      $(BENCH_TARGET) $(BENCH_CPP_OBJ) $(BENCH_C_OBJS) $(BENCH_DEPS) \
	      $(LIB_TARGET) $(LIB_OBJS) $(LIB_DEPS)

-include $(DEPS) $(UNIT_TEST_DEPS) $(BENCH_DEPS) $(LIB_DEPS)
