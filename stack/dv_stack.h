// dv_stack.h - contiguous LIFO stack with explicit memory control.
//
// The stack stores `capacity` slots of `elem_size` bytes in one contiguous
// allocation and tracks a single `top` index that always points one past the
// last valid element. Push and pop are O(1), touch only the hot end of the
// buffer, and never call the allocator while the stack has room.
//
// Two element models share the same representation:
//   * generic mode  - byte-stride copies of fixed-size elements
//   * pointer mode  - direct `void *` slot writes, no memcpy on the hot path
//
// The core is single-threaded and unsynchronized by design. A mutex-guarded
// wrapper lives in dv_stack_mt.h and is deliberately kept out of this header
// so the hot path carries no synchronization cost.

#ifndef DV_STACK_H
#define DV_STACK_H

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef DV_CACHE_LINE_SIZE
#if defined(__aarch64__)
#define DV_CACHE_LINE_SIZE 128u
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
#define DV_CACHE_LINE_SIZE 64u
#else
#define DV_CACHE_LINE_SIZE 32u
#endif
#endif

// Smallest capacity a growable stack jumps to on its first growth. Growth is
// 1.5x thereafter, which keeps freed blocks reusable by the allocator.
#ifndef DV_STACK_MIN_CAPACITY
#define DV_STACK_MIN_CAPACITY 8u
#endif

// Debug-time contract checks. Compiled out by NDEBUG like plain assert, and
// suppressible independently for callers that want checked release builds of
// everything else.
#ifdef DV_STACK_NO_ASSERT
#define DV_STACK_ASSERT(expr) ((void)0)
#else
#define DV_STACK_ASSERT(expr) assert(expr)
#endif

typedef enum {
  DV_STACK_OK = 0,
  DV_STACK_ERR_INVALID,  // NULL or contract-violating arguments
  DV_STACK_ERR_EMPTY,    // pop/peek with no elements
  DV_STACK_ERR_FULL,     // push past a fixed or maximum capacity
  DV_STACK_ERR_NOMEM,    // allocator hook returned NULL
  DV_STACK_ERR_OVERFLOW, // byte size of the request is not representable
} dv_stack_status_t;

// Caller-defined memory hooks.
//
// `alloc` and `free` are mandatory. `realloc` is optional; when it is NULL the
// stack falls back to alloc + copy-live-bytes + free, so allocators that
// cannot resize in place need not pretend otherwise.
//
// `align` is always a power of two and is the alignment the returned block
// must satisfy. `old_size`/`size` are the exact byte counts previously handed
// out, so arena and slab allocators can reclaim without their own bookkeeping.
typedef struct {
  void *(*alloc)(size_t size, size_t align, void *user_data);
  void *(*realloc)(void *ptr, size_t old_size, size_t new_size, size_t align,
                   void *user_data);
  void (*free)(void *ptr, size_t size, size_t align, void *user_data);
  void *user_data;
} dv_stack_allocator_t;

typedef struct {
  size_t elem_size;        // bytes per element; must be non-zero
  size_t elem_align;       // 0 selects alignof(max_align_t); power of two
  size_t initial_capacity; // elements reserved by dv_stack_init
  size_t max_capacity;     // growth ceiling; 0 means unbounded
  bool growable;           // false pins capacity at initial_capacity
  bool cache_aligned;      // align the buffer to DV_CACHE_LINE_SIZE
} dv_stack_config_t;

// Field order is deliberate: buffer/top/capacity/elem_size are the only fields
// the push and pop fast paths read, and they share the first cache line.
typedef struct {
  unsigned char *buffer;
  size_t top;
  size_t capacity;
  size_t elem_size;

  size_t align;
  size_t initial_capacity;
  size_t max_capacity;
  dv_stack_allocator_t allocator;
  bool growable;
  bool ptr_mode; // elem_size and alignment permit direct void * slot access
} dv_stack_t;

// --- Lifecycle ---------------------------------------------------------

// Initializes `s` in place. On failure `*s` is left zeroed, which is a valid
// argument to dv_stack_destroy. `allocator` may be NULL to use malloc-backed
// default hooks; when supplied, its alloc and free members must be non-NULL.
dv_stack_status_t dv_stack_init(dv_stack_t *s, const dv_stack_config_t *config,
                                const dv_stack_allocator_t *allocator);

// Convenience initializer for the pointer specialization.
dv_stack_status_t dv_stack_init_ptr(dv_stack_t *s, size_t initial_capacity,
                                    bool growable,
                                    const dv_stack_allocator_t *allocator);

// Drops all elements and returns the buffer to the configured initial
// capacity, releasing any growth beyond it. The stack stays usable with the
// same config. The contents are cleared even if the resize itself fails.
dv_stack_status_t dv_stack_reset(dv_stack_t *s);

// Releases the buffer through the owning allocator and zeroes the control
// block. Idempotent, and safe on a zeroed or already-destroyed stack.
void dv_stack_destroy(dv_stack_t *s);

// --- Capacity management -----------------------------------------------

// Grows capacity to at least `capacity`. Never shrinks. Returns DV_STACK_OK
// when the stack already has the room, DV_STACK_ERR_FULL when the request
// exceeds a fixed or maximum capacity.
dv_stack_status_t dv_stack_reserve(dv_stack_t *s, size_t capacity);

// Releases unused capacity on a growable stack. A fixed stack has nothing to
// give back and succeeds without change. Shrinking an empty stack releases the
// buffer entirely.
dv_stack_status_t dv_stack_shrink_to_fit(dv_stack_t *s);

// Internal slow path shared by the inline push fast paths. Ensures room for
// one more element. Exposed only because the fast paths are inline.
dv_stack_status_t dv_stack_grow_for_push(dv_stack_t *s);

// --- Bulk operations ---------------------------------------------------

// Pushes up to `count` elements read from the `elems` array at the stack's own
// stride. Returns the number pushed, which is short of `count` only when the
// stack cannot grow far enough. elems[0] is pushed first, so elems[count - 1]
// ends up on top.
size_t dv_stack_push_bulk(dv_stack_t *s, const void *elems, size_t count);

// Pops up to `count` elements, equivalent to that many successive pops:
// out[0] receives the former top. `out` may be NULL to discard. Returns the
// number popped.
size_t dv_stack_pop_bulk(dv_stack_t *s, void *out, size_t count);

// --- Diagnostics -------------------------------------------------------

// Stable, never-NULL name for a status code.
const char *dv_stack_status_str(dv_stack_status_t status);

// --- Inline fast paths -------------------------------------------------

static inline size_t dv_stack_size(const dv_stack_t *s) {
  return s ? s->top : 0u;
}

static inline size_t dv_stack_capacity(const dv_stack_t *s) {
  return s ? s->capacity : 0u;
}

static inline size_t dv_stack_elem_size(const dv_stack_t *s) {
  return s ? s->elem_size : 0u;
}

static inline bool dv_stack_is_empty(const dv_stack_t *s) {
  return !s || s->top == 0u;
}

// True when the next push must call the allocator or fail. A growable stack
// that has room to grow reports full only at its ceiling.
static inline bool dv_stack_is_full(const dv_stack_t *s) {
  if (!s)
    return true;
  if (s->top < s->capacity)
    return false;
  if (!s->growable)
    return true;
  return s->max_capacity != 0u && s->capacity >= s->max_capacity;
}

// Address of the element `depth` slots below the top; depth 0 is the top.
// NULL when out of range. Valid until the next mutating call.
static inline void *dv_stack_at(dv_stack_t *s, size_t depth) {
  if (!s || depth >= s->top)
    return NULL;
  return s->buffer + ((s->top - 1u - depth) * s->elem_size);
}

static inline const void *dv_stack_at_const(const dv_stack_t *s, size_t depth) {
  if (!s || depth >= s->top)
    return NULL;
  return s->buffer + ((s->top - 1u - depth) * s->elem_size);
}

// Zero-copy view of the top element, or NULL when empty. Invalidated by any
// push that grows the buffer.
static inline void *dv_stack_top(dv_stack_t *s) { return dv_stack_at(s, 0u); }

static inline const void *dv_stack_top_const(const dv_stack_t *s) {
  return dv_stack_at_const(s, 0u);
}

static inline dv_stack_status_t dv_stack_push(dv_stack_t *s, const void *elem) {
  if (!s || !elem)
    return DV_STACK_ERR_INVALID;

  if (s->top == s->capacity) {
    const dv_stack_status_t status = dv_stack_grow_for_push(s);
    if (status != DV_STACK_OK)
      return status;
  }

  memcpy(s->buffer + (s->top * s->elem_size), elem, s->elem_size);
  ++s->top;
  return DV_STACK_OK;
}

// `out` may be NULL to discard the popped element.
static inline dv_stack_status_t dv_stack_pop(dv_stack_t *s, void *out) {
  if (!s)
    return DV_STACK_ERR_INVALID;
  if (s->top == 0u)
    return DV_STACK_ERR_EMPTY;

  --s->top;
  if (out)
    memcpy(out, s->buffer + (s->top * s->elem_size), s->elem_size);
  return DV_STACK_OK;
}

static inline dv_stack_status_t dv_stack_peek(const dv_stack_t *s, void *out) {
  if (!s || !out)
    return DV_STACK_ERR_INVALID;
  if (s->top == 0u)
    return DV_STACK_ERR_EMPTY;

  memcpy(out, s->buffer + ((s->top - 1u) * s->elem_size), s->elem_size);
  return DV_STACK_OK;
}

// Drops every element without touching the allocator. O(1).
static inline void dv_stack_clear(dv_stack_t *s) {
  if (s)
    s->top = 0u;
}

// --- Pointer specialization --------------------------------------------
//
// Available when the stack was configured with elem_size == sizeof(void *) and
// an element alignment suitable for void *, which dv_stack_init_ptr guarantees.
// These write and read slots directly instead of going through memcpy.

static inline bool dv_stack_is_ptr_mode(const dv_stack_t *s) {
  return s && s->ptr_mode;
}

static inline dv_stack_status_t dv_stack_push_ptr(dv_stack_t *s, void *value) {
  if (!s)
    return DV_STACK_ERR_INVALID;

  DV_STACK_ASSERT(s->ptr_mode && "stack is not in pointer mode");
  if (!s->ptr_mode)
    return DV_STACK_ERR_INVALID;

  if (s->top == s->capacity) {
    const dv_stack_status_t status = dv_stack_grow_for_push(s);
    if (status != DV_STACK_OK)
      return status;
  }

  ((void **)(void *)s->buffer)[s->top] = value;
  ++s->top;
  return DV_STACK_OK;
}

static inline dv_stack_status_t dv_stack_pop_ptr(dv_stack_t *s, void **out) {
  if (!s || !out)
    return DV_STACK_ERR_INVALID;

  DV_STACK_ASSERT(s->ptr_mode && "stack is not in pointer mode");
  if (!s->ptr_mode)
    return DV_STACK_ERR_INVALID;

  if (s->top == 0u) {
    *out = NULL;
    return DV_STACK_ERR_EMPTY;
  }

  --s->top;
  *out = ((void **)(void *)s->buffer)[s->top];
  return DV_STACK_OK;
}

static inline dv_stack_status_t dv_stack_peek_ptr(const dv_stack_t *s,
                                                  void **out) {
  if (!s || !out)
    return DV_STACK_ERR_INVALID;

  DV_STACK_ASSERT(s->ptr_mode && "stack is not in pointer mode");
  if (!s->ptr_mode)
    return DV_STACK_ERR_INVALID;

  if (s->top == 0u) {
    *out = NULL;
    return DV_STACK_ERR_EMPTY;
  }

  *out = ((void **)(void *)s->buffer)[s->top - 1u];
  return DV_STACK_OK;
}

#endif // DV_STACK_H
