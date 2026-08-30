// dv_heap.h - array-backed binary heap with explicit memory control.
//
// A contiguous 0-based binary heap: parent(i) = (i - 1) / 2, left(i) = 2i + 1,
// right(i) = 2i + 2. Push and pop are iterative sift loops that move a single
// carried element through a travelling hole, so a sift of depth d costs d + 1
// element moves rather than the 3d of a swap-based loop.
//
// Ordering is configured, not hard-coded. Built-in key types read a scalar at
// a fixed offset (optionally through the pointer stored in the slot) and are
// compared inline; DV_HEAP_KEY_CUSTOM falls back to a comparator callback.
// The built-ins exist because the comparison is the hot path: a binary heap
// performs O(log n) comparisons per push and twice that per pop, and an
// indirect call in that loop dominates everything else the heap does.
//
// The core is single-threaded and unsynchronized by design. A mutex-guarded
// wrapper lives in dv_heap_mt.h.

#ifndef DV_HEAP_H
#define DV_HEAP_H

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

// Smallest capacity a growable heap jumps to on its first growth. Growth is
// 1.5x thereafter, which keeps freed blocks reusable by the allocator.
#ifndef DV_HEAP_MIN_CAPACITY
#define DV_HEAP_MIN_CAPACITY 8u
#endif

#ifdef DV_HEAP_NO_ASSERT
#define DV_HEAP_ASSERT(expr) ((void)0)
#else
#define DV_HEAP_ASSERT(expr) assert(expr)
#endif

typedef enum {
  DV_HEAP_OK = 0,
  DV_HEAP_ERR_INVALID,  // NULL or contract-violating arguments
  DV_HEAP_ERR_EMPTY,    // pop/peek with no elements
  DV_HEAP_ERR_FULL,     // push past a fixed or maximum capacity
  DV_HEAP_ERR_NOMEM,    // allocator hook returned NULL
  DV_HEAP_ERR_OVERFLOW, // byte size of the request is not representable
} dv_heap_status_t;

typedef enum {
  DV_HEAP_MIN = 0, // smallest key at the root
  DV_HEAP_MAX,     // largest key at the root
} dv_heap_order_t;

// How the key is read out of an element. Every built-in loads through memcpy,
// so an unaligned key_offset is well defined and still compiles to one load.
typedef enum {
  DV_HEAP_KEY_U64 = 0,
  DV_HEAP_KEY_I64,
  DV_HEAP_KEY_U32,
  DV_HEAP_KEY_I32,
  DV_HEAP_KEY_F64,
  DV_HEAP_KEY_F32,
  DV_HEAP_KEY_PTR,   // compares the pointer value itself, as uintptr_t
  DV_HEAP_KEY_CUSTOM // defers to the comparator callback
} dv_heap_key_type_t;

// Receives the addresses of two element slots, not of two keys, and returns
// negative, zero or positive in the manner of memcmp. In pointer-payload mode
// each argument is the address of a void * slot, so a comparator dereferences
// twice to reach the object.
typedef int (*dv_heap_compare_fn)(const void *a, const void *b,
                                  void *user_data);

// Called whenever an element comes to rest at a new index, with the address of
// its slot. This is the index map hook: an owner that records the reported
// index inside its own object can later hand that index to dv_heap_update_at
// or dv_heap_remove_at. Leave NULL when indices are not tracked; the heap then
// pays a single well-predicted branch per move and no call.
typedef void (*dv_heap_move_fn)(void *slot, size_t index, void *user_data);

// See dv_stack.h for the same allocator contract: alloc and free are
// mandatory, realloc is optional, and every hook receives the exact byte count
// and the required alignment so arena allocators need no side table.
typedef struct {
  void *(*alloc)(size_t size, size_t align, void *user_data);
  void *(*realloc)(void *ptr, size_t old_size, size_t new_size, size_t align,
                   void *user_data);
  void (*free)(void *ptr, size_t size, size_t align, void *user_data);
  void *user_data;
} dv_heap_allocator_t;

typedef struct {
  size_t elem_size;        // bytes per element; must be non-zero
  size_t elem_align;       // 0 selects alignof(max_align_t); power of two
  size_t initial_capacity; // elements reserved by dv_heap_init
  size_t max_capacity;     // growth ceiling; 0 means unbounded
  bool growable;           // false pins capacity at initial_capacity
  bool cache_aligned;      // align the buffer to DV_CACHE_LINE_SIZE

  dv_heap_order_t order;
  dv_heap_key_type_t key_type;

  // Byte offset of the key. When key_indirect is false this is an offset into
  // the element itself and is bounds-checked against elem_size at init. When
  // true, the slot holds a void * and the offset applies inside the pointed-to
  // object, which the heap cannot bounds-check.
  size_t key_offset;
  bool key_indirect;

  dv_heap_compare_fn compare; // required when key_type is DV_HEAP_KEY_CUSTOM
  void *compare_user_data;

  dv_heap_move_fn on_move; // optional index map hook
  void *move_user_data;
} dv_heap_config_t;

// Field order is deliberate: everything the sift loops read on every step --
// the buffer, the bounds, and the whole key description -- shares the first
// cache line.
typedef struct {
  unsigned char *buffer;
  size_t size;
  size_t capacity;
  size_t elem_size;

  size_t key_offset;
  dv_heap_key_type_t key_type;
  dv_heap_order_t order;
  bool key_indirect;

  dv_heap_compare_fn compare;
  void *compare_user_data;

  dv_heap_move_fn on_move;
  void *move_user_data;

  size_t align;
  size_t initial_capacity;
  size_t max_capacity;
  dv_heap_allocator_t allocator;
  bool growable;
  bool ptr_mode;
} dv_heap_t;

// --- Lifecycle ---------------------------------------------------------

// Initializes `h` in place. On failure `*h` is left zeroed, which is a valid
// argument to dv_heap_destroy. `allocator` may be NULL to use malloc-backed
// default hooks; when supplied, its alloc and free members must be non-NULL.
//
// The buffer always carries one slot beyond `capacity`. That slot is the sift
// scratch: it holds the carried element while a hole travels, which is what
// lets pop and remove run without allocating and without a swap loop.
dv_heap_status_t dv_heap_init(dv_heap_t *h, const dv_heap_config_t *config,
                              const dv_heap_allocator_t *allocator);

// Drops all elements and returns any growth beyond the configured initial
// capacity to the allocator. The heap stays usable with the same config. The
// contents are cleared even if the resize itself fails.
dv_heap_status_t dv_heap_reset(dv_heap_t *h);

// Releases the buffer through the owning allocator and zeroes the control
// block. Idempotent, and safe on a zeroed or already-destroyed heap.
void dv_heap_destroy(dv_heap_t *h);

// --- Core operations ---------------------------------------------------

dv_heap_status_t dv_heap_push(dv_heap_t *h, const void *elem);

// Removes the root. `out` may be NULL to discard it.
dv_heap_status_t dv_heap_pop(dv_heap_t *h, void *out);

// Copies the root out without removing it.
dv_heap_status_t dv_heap_peek(const dv_heap_t *h, void *out);

// Replaces the root and re-sifts once, which is the top-k primitive: it costs
// one sift-down where a pop followed by a push costs a sift-down and a
// sift-up. `out` may be NULL to discard the displaced root.
dv_heap_status_t dv_heap_replace_top(dv_heap_t *h, const void *elem, void *out);

// --- Priority updates --------------------------------------------------

// Replaces the element at `index` and re-sifts it exactly once, in whichever
// direction the new key requires. This is decrease_key and increase_key both:
// the direction follows from the comparison rather than from the caller. Pair
// it with the on_move hook to know an element's current index.
dv_heap_status_t dv_heap_update_at(dv_heap_t *h, size_t index,
                                   const void *elem);

// Removes the element at `index`, restoring heap order. `out` may be NULL.
dv_heap_status_t dv_heap_remove_at(dv_heap_t *h, size_t index, void *out);

// --- Maintenance -------------------------------------------------------

// Restores the heap-order property over the current contents in O(n) via
// Floyd's bottom-up construction. Use after mutating elements in place.
dv_heap_status_t dv_heap_heapify(dv_heap_t *h);

// Replaces the contents with `count` elements read at the heap's own stride
// and heapifies them in O(n) -- cheaper than `count` pushes at O(n log n).
dv_heap_status_t dv_heap_build(dv_heap_t *h, const void *elems, size_t count);

dv_heap_status_t dv_heap_reserve(dv_heap_t *h, size_t capacity);
dv_heap_status_t dv_heap_shrink_to_fit(dv_heap_t *h);

// --- Bulk operations ---------------------------------------------------

// Pushes up to `count` elements. Returns the number pushed, short of `count`
// only when the heap cannot grow far enough. Appending a run at least as long
// as the existing contents heapifies once instead of sifting each element up.
size_t dv_heap_push_bulk(dv_heap_t *h, const void *elems, size_t count);

// Pops up to `count` elements into `out` in priority order, root first.
// `out` may be NULL to discard. Returns the number popped.
size_t dv_heap_pop_bulk(dv_heap_t *h, void *out, size_t count);

// --- Diagnostics -------------------------------------------------------

const char *dv_heap_status_str(dv_heap_status_t status);

// Verifies the heap-order property across every parent/child pair in O(n).
// Intended for tests and debug assertions, not for the hot path.
bool dv_heap_is_valid(const dv_heap_t *h);

// --- Inline accessors --------------------------------------------------
//
// The sift loops live out of line: they are O(log n) loops where a call costs
// nothing measurable, unlike the stack's three-instruction push.

static inline size_t dv_heap_size(const dv_heap_t *h) {
  return h ? h->size : 0u;
}

static inline size_t dv_heap_capacity(const dv_heap_t *h) {
  return h ? h->capacity : 0u;
}

static inline size_t dv_heap_elem_size(const dv_heap_t *h) {
  return h ? h->elem_size : 0u;
}

static inline bool dv_heap_is_empty(const dv_heap_t *h) {
  return !h || h->size == 0u;
}

// True when the next push must call the allocator or fail. A growable heap
// that has room to grow reports full only at its ceiling.
static inline bool dv_heap_is_full(const dv_heap_t *h) {
  if (!h)
    return true;
  if (h->size < h->capacity)
    return false;
  if (!h->growable)
    return true;
  return h->max_capacity != 0u && h->capacity >= h->max_capacity;
}

// Address of the element at `index`, or NULL when out of range. Index 0 is the
// root. Valid until the next mutating call.
//
// Writing a key through this pointer breaks the heap-order property; use
// dv_heap_update_at, or dv_heap_heapify after a bulk edit.
static inline void *dv_heap_at(dv_heap_t *h, size_t index) {
  if (!h || index >= h->size)
    return NULL;
  return h->buffer + (index * h->elem_size);
}

static inline const void *dv_heap_at_const(const dv_heap_t *h, size_t index) {
  if (!h || index >= h->size)
    return NULL;
  return h->buffer + (index * h->elem_size);
}

// Zero-copy view of the root, or NULL when empty.
static inline void *dv_heap_top(dv_heap_t *h) { return dv_heap_at(h, 0u); }

static inline const void *dv_heap_top_const(const dv_heap_t *h) {
  return dv_heap_at_const(h, 0u);
}

// Drops every element without touching the allocator. O(1).
static inline void dv_heap_clear(dv_heap_t *h) {
  if (h)
    h->size = 0u;
}

static inline bool dv_heap_is_ptr_mode(const dv_heap_t *h) {
  return h && h->ptr_mode;
}

#endif // DV_HEAP_H
