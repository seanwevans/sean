// dv_heap.c - array-backed binary heap.

#include "dv_heap.h"

#include <stdlib.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

// The comparator contract check doubles every comparison, so it is compiled
// out alongside the asserts that report it.
#if defined(NDEBUG) || defined(DV_HEAP_NO_ASSERT)
#define DV_HEAP_CHECK_COMPARATOR 0
#else
#define DV_HEAP_CHECK_COMPARATOR 1
#endif

// --- Size and alignment arithmetic -------------------------------------

static bool dv_heap_is_pow2(size_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static bool dv_heap_align_up(size_t size, size_t align, size_t *out) {
  if (!out || !dv_heap_is_pow2(align))
    return false;

  if (size > SIZE_MAX - (align - 1u))
    return false;

  *out = (size + align - 1u) & ~(align - 1u);
  return true;
}

// Byte size of the allocation backing `capacity` elements. The buffer always
// carries one slot beyond the capacity to hold the element a sift carries, so
// pop and remove need no scratch allocation of their own.
//
// The capacity ceiling also keeps the child index arithmetic (2i + 2) inside
// size_t for every reachable index, which is why the sift loops can compute
// children without an overflow check on each step.
static dv_heap_status_t dv_heap_bytes_for(size_t capacity, size_t elem_size,
                                          size_t *out) {
  if (capacity == 0u) {
    *out = 0u;
    return DV_HEAP_OK;
  }

  if (capacity > (SIZE_MAX - 2u) / 2u)
    return DV_HEAP_ERR_OVERFLOW;

  const size_t slots = capacity + 1u;
  if (elem_size > SIZE_MAX / slots)
    return DV_HEAP_ERR_OVERFLOW;

  *out = slots * elem_size;
  return DV_HEAP_OK;
}

// --- Default allocator hooks -------------------------------------------

#define DV_HEAP_MALLOC_ALIGN (alignof(max_align_t))

static void *dv_heap_over_aligned_alloc(size_t size, size_t align) {
#if defined(_MSC_VER)
  return _aligned_malloc(size, align);
#else
  size_t padded = 0u;
  if (!dv_heap_align_up(size, align, &padded))
    return NULL;
  return aligned_alloc(align, padded);
#endif
}

static void dv_heap_over_aligned_free(void *ptr) {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static void *dv_heap_default_alloc(size_t size, size_t align, void *user_data) {
  (void)user_data;

  if (align <= DV_HEAP_MALLOC_ALIGN)
    return malloc(size);
  return dv_heap_over_aligned_alloc(size, align);
}

static void *dv_heap_default_realloc(void *ptr, size_t old_size,
                                     size_t new_size, size_t align,
                                     void *user_data) {
  (void)user_data;

  if (align <= DV_HEAP_MALLOC_ALIGN)
    return realloc(ptr, new_size);

  void *mem = dv_heap_over_aligned_alloc(new_size, align);
  if (!mem)
    return NULL;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);

  dv_heap_over_aligned_free(ptr);
  return mem;
}

static void dv_heap_default_free(void *ptr, size_t size, size_t align,
                                 void *user_data) {
  (void)size;
  (void)user_data;

  if (align <= DV_HEAP_MALLOC_ALIGN) {
    free(ptr);
    return;
  }
  dv_heap_over_aligned_free(ptr);
}

static dv_heap_allocator_t dv_heap_default_allocator(void) {
  const dv_heap_allocator_t hooks = {
      .alloc = dv_heap_default_alloc,
      .realloc = dv_heap_default_realloc,
      .free = dv_heap_default_free,
      .user_data = NULL,
  };
  return hooks;
}

// --- Slots and ordering ------------------------------------------------

static inline unsigned char *dv_heap_slot(const dv_heap_t *h, size_t index) {
  return h->buffer + (index * h->elem_size);
}

// The slot one past the capacity, reserved for the element a sift carries.
static inline unsigned char *dv_heap_scratch(const dv_heap_t *h) {
  return h->buffer + (h->capacity * h->elem_size);
}

static inline void dv_heap_notify(const dv_heap_t *h, size_t index) {
  if (h->on_move)
    h->on_move(dv_heap_slot(h, index), index, h->move_user_data);
}

static void dv_heap_notify_all(const dv_heap_t *h) {
  if (!h->on_move)
    return;

  for (size_t i = 0u; i < h->size; ++i)
    h->on_move(dv_heap_slot(h, i), i, h->move_user_data);
}

static const unsigned char *dv_heap_key_of(const dv_heap_t *h,
                                           const void *slot) {
  const unsigned char *base = (const unsigned char *)slot;

  if (h->key_indirect)
    base = (const unsigned char *)(*(void *const *)slot);

  return base + h->key_offset;
}

// Loads go through memcpy so an unaligned key_offset stays well defined; every
// compiler folds these into a single load.
static bool dv_heap_key_less(const dv_heap_t *h, const void *a, const void *b) {
  const unsigned char *ka = dv_heap_key_of(h, a);
  const unsigned char *kb = dv_heap_key_of(h, b);

  switch (h->key_type) {
  case DV_HEAP_KEY_U64: {
    uint64_t x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_I64: {
    int64_t x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_U32: {
    uint32_t x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_I32: {
    int32_t x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_F64: {
    double x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_F32: {
    float x, y;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return x < y;
  }
  case DV_HEAP_KEY_PTR: {
    void *x = NULL;
    void *y = NULL;
    memcpy(&x, ka, sizeof(x));
    memcpy(&y, kb, sizeof(y));
    return (uintptr_t)x < (uintptr_t)y;
  }
  case DV_HEAP_KEY_CUSTOM:
    break;
  }

  return h->compare(a, b, h->compare_user_data) < 0;
}

static inline bool dv_heap_before_raw(const dv_heap_t *h, const void *a,
                                      const void *b) {
  return h->order == DV_HEAP_MIN ? dv_heap_key_less(h, a, b)
                                 : dv_heap_key_less(h, b, a);
}

// "a belongs closer to the root than b". In debug builds the reverse question
// is asked as well: a comparator that answers yes both ways is not a strict
// ordering, and the heap built on it would be silently wrong rather than
// noisily broken.
static bool dv_heap_before(const dv_heap_t *h, const void *a, const void *b) {
#if DV_HEAP_CHECK_COMPARATOR
  const bool ab = dv_heap_before_raw(h, a, b);
  const bool ba = dv_heap_before_raw(h, b, a);
  DV_HEAP_ASSERT(!(ab && ba) && "comparator orders a before b and b before a");
  return ab;
#else
  return dv_heap_before_raw(h, a, b);
#endif
}

// --- Sift loops --------------------------------------------------------
//
// Both loops carry one element and move a hole, so a sift of depth d performs
// d + 1 element copies instead of the 3d a swap loop would.

static void dv_heap_sift_up(dv_heap_t *h, size_t index, const void *item) {
  const size_t elem_size = h->elem_size;

  while (index > 0u) {
    const size_t parent = (index - 1u) / 2u;
    unsigned char *parent_slot = dv_heap_slot(h, parent);

    if (!dv_heap_before(h, item, parent_slot))
      break;

    memcpy(dv_heap_slot(h, index), parent_slot, elem_size);
    dv_heap_notify(h, index);
    index = parent;
  }

  unsigned char *dst = dv_heap_slot(h, index);
  if (dst != item)
    memcpy(dst, item, elem_size);
  dv_heap_notify(h, index);
}

static void dv_heap_sift_down(dv_heap_t *h, size_t index, const void *item) {
  const size_t elem_size = h->elem_size;
  const size_t size = h->size;

  for (;;) {
    const size_t left = (2u * index) + 1u;
    if (left >= size)
      break;

    size_t child = left;
    const size_t right = left + 1u;
    if (right < size &&
        dv_heap_before(h, dv_heap_slot(h, right), dv_heap_slot(h, left)))
      child = right;

    unsigned char *child_slot = dv_heap_slot(h, child);
    if (!dv_heap_before(h, child_slot, item))
      break;

    memcpy(dv_heap_slot(h, index), child_slot, elem_size);
    dv_heap_notify(h, index);
    index = child;
  }

  unsigned char *dst = dv_heap_slot(h, index);
  if (dst != item)
    memcpy(dst, item, elem_size);
  dv_heap_notify(h, index);
}

// --- Buffer management -------------------------------------------------

static dv_heap_status_t dv_heap_set_capacity(dv_heap_t *h,
                                             size_t new_capacity) {
  if (new_capacity == h->capacity)
    return DV_HEAP_OK;
  if (new_capacity < h->size)
    return DV_HEAP_ERR_INVALID;

  size_t old_bytes = 0u;
  size_t new_bytes = 0u;

  dv_heap_status_t status =
      dv_heap_bytes_for(h->capacity, h->elem_size, &old_bytes);
  if (status != DV_HEAP_OK)
    return status;

  status = dv_heap_bytes_for(new_capacity, h->elem_size, &new_bytes);
  if (status != DV_HEAP_OK)
    return status;

  if (new_bytes == 0u) {
    if (h->buffer)
      h->allocator.free(h->buffer, old_bytes, h->align, h->allocator.user_data);
    h->buffer = NULL;
    h->capacity = 0u;
    return DV_HEAP_OK;
  }

  void *mem = NULL;
  if (!h->buffer) {
    mem = h->allocator.alloc(new_bytes, h->align, h->allocator.user_data);
  } else if (h->allocator.realloc) {
    mem = h->allocator.realloc(h->buffer, old_bytes, new_bytes, h->align,
                               h->allocator.user_data);
  } else {
    mem = h->allocator.alloc(new_bytes, h->align, h->allocator.user_data);
    if (mem) {
      const size_t live = h->size * h->elem_size;
      if (live > 0u)
        memcpy(mem, h->buffer, live);
      h->allocator.free(h->buffer, old_bytes, h->align, h->allocator.user_data);
    }
  }

  if (!mem)
    return DV_HEAP_ERR_NOMEM;

  h->buffer = (unsigned char *)mem;
  h->capacity = new_capacity;
  return DV_HEAP_OK;
}

static size_t dv_heap_next_capacity(size_t capacity) {
  if (capacity < DV_HEAP_MIN_CAPACITY)
    return DV_HEAP_MIN_CAPACITY;

  const size_t half = capacity / 2u;
  if (capacity > SIZE_MAX - half)
    return SIZE_MAX;

  return capacity + half;
}

static size_t dv_heap_clamp_capacity(const dv_heap_t *h, size_t capacity) {
  if (h->max_capacity != 0u && capacity > h->max_capacity)
    return h->max_capacity;
  return capacity;
}

static dv_heap_status_t dv_heap_grow_for_push(dv_heap_t *h) {
  if (h->size < h->capacity)
    return DV_HEAP_OK;
  if (!h->growable)
    return DV_HEAP_ERR_FULL;

  const size_t next =
      dv_heap_clamp_capacity(h, dv_heap_next_capacity(h->capacity));
  if (next <= h->capacity)
    return DV_HEAP_ERR_FULL;

  return dv_heap_set_capacity(h, next);
}

// --- Configuration validation ------------------------------------------

static bool dv_heap_key_type_valid(dv_heap_key_type_t type) {
  switch (type) {
  case DV_HEAP_KEY_U64:
  case DV_HEAP_KEY_I64:
  case DV_HEAP_KEY_U32:
  case DV_HEAP_KEY_I32:
  case DV_HEAP_KEY_F64:
  case DV_HEAP_KEY_F32:
  case DV_HEAP_KEY_PTR:
  case DV_HEAP_KEY_CUSTOM:
    return true;
  }

  return false;
}

static size_t dv_heap_key_width(dv_heap_key_type_t type) {
  switch (type) {
  case DV_HEAP_KEY_U64:
  case DV_HEAP_KEY_I64:
  case DV_HEAP_KEY_F64:
    return 8u;
  case DV_HEAP_KEY_U32:
  case DV_HEAP_KEY_I32:
  case DV_HEAP_KEY_F32:
    return 4u;
  case DV_HEAP_KEY_PTR:
    return sizeof(void *);
  case DV_HEAP_KEY_CUSTOM:
    return 0u;
  }

  return 0u;
}

// --- Lifecycle ---------------------------------------------------------

dv_heap_status_t dv_heap_init(dv_heap_t *h, const dv_heap_config_t *config,
                              const dv_heap_allocator_t *allocator) {
  if (!h)
    return DV_HEAP_ERR_INVALID;

  memset(h, 0, sizeof(*h));

  if (!config || config->elem_size == 0u)
    return DV_HEAP_ERR_INVALID;

  if (config->order != DV_HEAP_MIN && config->order != DV_HEAP_MAX)
    return DV_HEAP_ERR_INVALID;

  if (!dv_heap_key_type_valid(config->key_type))
    return DV_HEAP_ERR_INVALID;

  if (config->key_type == DV_HEAP_KEY_CUSTOM && !config->compare)
    return DV_HEAP_ERR_INVALID;

  size_t elem_align = config->elem_align;
  if (elem_align == 0u)
    elem_align = DV_HEAP_MALLOC_ALIGN;
  if (!dv_heap_is_pow2(elem_align))
    return DV_HEAP_ERR_INVALID;

  // A stride that is not a multiple of the element alignment misaligns every
  // element after the first.
  if ((config->elem_size & (elem_align - 1u)) != 0u)
    return DV_HEAP_ERR_INVALID;

  size_t align = elem_align;
  if (config->cache_aligned && align < DV_CACHE_LINE_SIZE)
    align = DV_CACHE_LINE_SIZE;

  if (config->key_indirect) {
    // The key lives inside the pointed-to object, so the slot must hold a
    // pointer and the offset cannot be bounds-checked from here.
    if (config->elem_size != sizeof(void *))
      return DV_HEAP_ERR_INVALID;
    if ((align & (alignof(void *) - 1u)) != 0u)
      return DV_HEAP_ERR_INVALID;
  } else if (config->key_type != DV_HEAP_KEY_CUSTOM) {
    // Ordered so the subtraction below cannot underflow.
    const size_t width = dv_heap_key_width(config->key_type);
    if (width > config->elem_size ||
        config->key_offset > config->elem_size - width)
      return DV_HEAP_ERR_INVALID;
  }

  dv_heap_allocator_t hooks;
  if (allocator) {
    if (!allocator->alloc || !allocator->free)
      return DV_HEAP_ERR_INVALID;
    hooks = *allocator;
  } else {
    hooks = dv_heap_default_allocator();
  }

  size_t max_capacity = config->max_capacity;
  if (!config->growable)
    max_capacity = config->initial_capacity;
  else if (max_capacity != 0u && max_capacity < config->initial_capacity)
    return DV_HEAP_ERR_INVALID;

  h->elem_size = config->elem_size;
  h->key_offset = config->key_offset;
  h->key_type = config->key_type;
  h->order = config->order;
  h->key_indirect = config->key_indirect;
  h->compare = config->compare;
  h->compare_user_data = config->compare_user_data;
  h->on_move = config->on_move;
  h->move_user_data = config->move_user_data;
  h->align = align;
  h->initial_capacity = config->initial_capacity;
  h->max_capacity = max_capacity;
  h->allocator = hooks;
  h->growable = config->growable;
  h->ptr_mode = (config->elem_size == sizeof(void *)) &&
                (align & (alignof(void *) - 1u)) == 0u;

  if (config->initial_capacity > 0u) {
    const dv_heap_status_t status =
        dv_heap_set_capacity(h, config->initial_capacity);
    if (status != DV_HEAP_OK) {
      memset(h, 0, sizeof(*h));
      return status;
    }
  }

  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_reset(dv_heap_t *h) {
  if (!h)
    return DV_HEAP_ERR_INVALID;

  h->size = 0u;
  return dv_heap_set_capacity(h, h->initial_capacity);
}

void dv_heap_destroy(dv_heap_t *h) {
  if (!h)
    return;

  if (h->buffer && h->allocator.free) {
    size_t bytes = 0u;
    if (dv_heap_bytes_for(h->capacity, h->elem_size, &bytes) != DV_HEAP_OK)
      bytes = 0u;
    h->allocator.free(h->buffer, bytes, h->align, h->allocator.user_data);
  }

  memset(h, 0, sizeof(*h));
}

// --- Capacity management -----------------------------------------------

dv_heap_status_t dv_heap_reserve(dv_heap_t *h, size_t capacity) {
  if (!h)
    return DV_HEAP_ERR_INVALID;
  if (capacity <= h->capacity)
    return DV_HEAP_OK;
  if (!h->growable)
    return DV_HEAP_ERR_FULL;
  if (h->max_capacity != 0u && capacity > h->max_capacity)
    return DV_HEAP_ERR_FULL;

  return dv_heap_set_capacity(h, capacity);
}

dv_heap_status_t dv_heap_shrink_to_fit(dv_heap_t *h) {
  if (!h)
    return DV_HEAP_ERR_INVALID;
  if (!h->growable || h->capacity == h->size)
    return DV_HEAP_OK;

  return dv_heap_set_capacity(h, h->size);
}

// --- Core operations ---------------------------------------------------

dv_heap_status_t dv_heap_push(dv_heap_t *h, const void *elem) {
  if (!h || !elem)
    return DV_HEAP_ERR_INVALID;

  if (h->size == h->capacity) {
    const dv_heap_status_t status = dv_heap_grow_for_push(h);
    if (status != DV_HEAP_OK)
      return status;
  }

  dv_heap_sift_up(h, h->size, elem);
  ++h->size;
  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_pop(dv_heap_t *h, void *out) {
  if (!h)
    return DV_HEAP_ERR_INVALID;
  if (h->size == 0u)
    return DV_HEAP_ERR_EMPTY;

  if (out)
    memcpy(out, dv_heap_slot(h, 0u), h->elem_size);

  --h->size;
  if (h->size == 0u)
    return DV_HEAP_OK;

  // Carry the former last element down from the root through the scratch slot,
  // so the hole travels and nothing is swapped.
  unsigned char *scratch = dv_heap_scratch(h);
  memcpy(scratch, dv_heap_slot(h, h->size), h->elem_size);
  dv_heap_sift_down(h, 0u, scratch);
  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_peek(const dv_heap_t *h, void *out) {
  if (!h || !out)
    return DV_HEAP_ERR_INVALID;
  if (h->size == 0u)
    return DV_HEAP_ERR_EMPTY;

  memcpy(out, dv_heap_slot(h, 0u), h->elem_size);
  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_replace_top(dv_heap_t *h, const void *elem,
                                     void *out) {
  if (!h || !elem)
    return DV_HEAP_ERR_INVALID;
  if (h->size == 0u)
    return DV_HEAP_ERR_EMPTY;

  if (out)
    memcpy(out, dv_heap_slot(h, 0u), h->elem_size);

  dv_heap_sift_down(h, 0u, elem);
  return DV_HEAP_OK;
}

// --- Priority updates --------------------------------------------------

dv_heap_status_t dv_heap_update_at(dv_heap_t *h, size_t index,
                                   const void *elem) {
  if (!h || !elem)
    return DV_HEAP_ERR_INVALID;
  if (index >= h->size)
    return DV_HEAP_ERR_INVALID;

  // One comparison against the parent decides the direction, so the element is
  // re-sifted exactly once whether the key rose or fell.
  if (index > 0u) {
    const size_t parent = (index - 1u) / 2u;
    if (dv_heap_before(h, elem, dv_heap_slot(h, parent))) {
      dv_heap_sift_up(h, index, elem);
      return DV_HEAP_OK;
    }
  }

  dv_heap_sift_down(h, index, elem);
  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_remove_at(dv_heap_t *h, size_t index, void *out) {
  if (!h)
    return DV_HEAP_ERR_INVALID;
  if (index >= h->size)
    return DV_HEAP_ERR_INVALID;

  if (out)
    memcpy(out, dv_heap_slot(h, index), h->elem_size);

  --h->size;
  if (index == h->size)
    return DV_HEAP_OK;

  unsigned char *scratch = dv_heap_scratch(h);
  memcpy(scratch, dv_heap_slot(h, h->size), h->elem_size);

  // The element backfilled into the hole can belong in either direction.
  if (index > 0u &&
      dv_heap_before(h, scratch, dv_heap_slot(h, (index - 1u) / 2u)))
    dv_heap_sift_up(h, index, scratch);
  else
    dv_heap_sift_down(h, index, scratch);

  return DV_HEAP_OK;
}

// --- Maintenance -------------------------------------------------------

dv_heap_status_t dv_heap_heapify(dv_heap_t *h) {
  if (!h)
    return DV_HEAP_ERR_INVALID;

  if (h->size > 1u) {
    // Suppress per-move reporting for the duration: the bottom-up pass moves
    // elements repeatedly, and one final sweep leaves an index map consistent
    // with far fewer calls.
    dv_heap_move_fn on_move = h->on_move;
    h->on_move = NULL;

    unsigned char *scratch = dv_heap_scratch(h);
    for (size_t i = h->size / 2u; i-- > 0u;) {
      memcpy(scratch, dv_heap_slot(h, i), h->elem_size);
      dv_heap_sift_down(h, i, scratch);
    }

    h->on_move = on_move;
  }

  dv_heap_notify_all(h);
  return DV_HEAP_OK;
}

dv_heap_status_t dv_heap_build(dv_heap_t *h, const void *elems, size_t count) {
  if (!h)
    return DV_HEAP_ERR_INVALID;

  if (count == 0u) {
    h->size = 0u;
    return DV_HEAP_OK;
  }

  if (!elems)
    return DV_HEAP_ERR_INVALID;

  if (count > h->capacity) {
    const dv_heap_status_t status = dv_heap_reserve(h, count);
    if (status != DV_HEAP_OK)
      return status;
  }

  memcpy(h->buffer, elems, count * h->elem_size);
  h->size = count;
  return dv_heap_heapify(h);
}

// --- Bulk operations ---------------------------------------------------

static void dv_heap_grow_for_bulk(dv_heap_t *h, size_t needed) {
  const size_t curve = dv_heap_next_capacity(h->capacity);
  const size_t target =
      dv_heap_clamp_capacity(h, needed > curve ? needed : curve);

  if (target > h->capacity && dv_heap_set_capacity(h, target) == DV_HEAP_OK)
    return;

  const size_t exact = dv_heap_clamp_capacity(h, needed);
  if (exact > h->capacity && exact < target)
    (void)dv_heap_set_capacity(h, exact);
}

size_t dv_heap_push_bulk(dv_heap_t *h, const void *elems, size_t count) {
  if (!h || !elems || count == 0u)
    return 0u;

  size_t room = h->capacity - h->size;
  if (room < count && h->growable && count <= SIZE_MAX - h->size) {
    dv_heap_grow_for_bulk(h, h->size + count);
    room = h->capacity - h->size;
  }

  const size_t n = count < room ? count : room;
  if (n == 0u)
    return 0u;

  const size_t old_size = h->size;
  memcpy(dv_heap_slot(h, old_size), elems, n * h->elem_size);
  h->size = old_size + n;

  // Appending a run at least as long as what was already there is cheaper to
  // rebuild bottom-up, O(old + n), than to sift each new element up,
  // O(n log(old + n)).
  if (n >= old_size) {
    (void)dv_heap_heapify(h);
    return n;
  }

  unsigned char *scratch = dv_heap_scratch(h);
  for (size_t i = 0u; i < n; ++i) {
    const size_t index = old_size + i;
    memcpy(scratch, dv_heap_slot(h, index), h->elem_size);
    dv_heap_sift_up(h, index, scratch);
  }

  return n;
}

size_t dv_heap_pop_bulk(dv_heap_t *h, void *out, size_t count) {
  if (!h || count == 0u)
    return 0u;

  const size_t n = count < h->size ? count : h->size;
  if (n == 0u)
    return 0u;

  unsigned char *dst = (unsigned char *)out;
  for (size_t i = 0u; i < n; ++i)
    (void)dv_heap_pop(h, dst ? dst + (i * h->elem_size) : NULL);

  return n;
}

// --- Diagnostics -------------------------------------------------------

const char *dv_heap_status_str(dv_heap_status_t status) {
  switch (status) {
  case DV_HEAP_OK:
    return "DV_HEAP_OK";
  case DV_HEAP_ERR_INVALID:
    return "DV_HEAP_ERR_INVALID";
  case DV_HEAP_ERR_EMPTY:
    return "DV_HEAP_ERR_EMPTY";
  case DV_HEAP_ERR_FULL:
    return "DV_HEAP_ERR_FULL";
  case DV_HEAP_ERR_NOMEM:
    return "DV_HEAP_ERR_NOMEM";
  case DV_HEAP_ERR_OVERFLOW:
    return "DV_HEAP_ERR_OVERFLOW";
  }

  return "DV_HEAP_ERR_UNKNOWN";
}

bool dv_heap_is_valid(const dv_heap_t *h) {
  if (!h)
    return false;
  if (h->size > h->capacity)
    return false;
  if (h->size > 0u && !h->buffer)
    return false;

  for (size_t i = 1u; i < h->size; ++i) {
    const size_t parent = (i - 1u) / 2u;
    if (dv_heap_before(h, dv_heap_slot(h, i), dv_heap_slot(h, parent)))
      return false;
  }

  return true;
}
