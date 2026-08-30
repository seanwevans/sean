// dv_stack.c - contiguous LIFO stack.

#include "dv_stack.h"

#include <stdlib.h>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

// --- Size and alignment arithmetic -------------------------------------

static bool dv_stack_is_pow2(size_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static bool dv_stack_align_up(size_t size, size_t align, size_t *out) {
  if (!out || !dv_stack_is_pow2(align))
    return false;

  if (size > SIZE_MAX - (align - 1u))
    return false;

  *out = (size + align - 1u) & ~(align - 1u);
  return true;
}

static dv_stack_status_t dv_stack_bytes_for(size_t capacity, size_t elem_size,
                                            size_t *out) {
  if (capacity != 0u && elem_size > SIZE_MAX / capacity)
    return DV_STACK_ERR_OVERFLOW;

  *out = capacity * elem_size;
  return DV_STACK_OK;
}

// --- Default allocator hooks -------------------------------------------
//
// Requests no stricter than max_align_t go straight to malloc/realloc so the
// common case keeps realloc's in-place growth. Over-aligned requests use the
// platform aligned allocator and pay for a copy on resize, because realloc
// only promises max_align_t.

#define DV_STACK_MALLOC_ALIGN (alignof(max_align_t))

static void *dv_stack_over_aligned_alloc(size_t size, size_t align) {
#if defined(_MSC_VER)
  return _aligned_malloc(size, align);
#else
  size_t padded = 0u;
  if (!dv_stack_align_up(size, align, &padded))
    return NULL;
  return aligned_alloc(align, padded);
#endif
}

static void dv_stack_over_aligned_free(void *ptr) {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

static void *dv_stack_default_alloc(size_t size, size_t align,
                                    void *user_data) {
  (void)user_data;

  if (align <= DV_STACK_MALLOC_ALIGN)
    return malloc(size);
  return dv_stack_over_aligned_alloc(size, align);
}

static void *dv_stack_default_realloc(void *ptr, size_t old_size,
                                      size_t new_size, size_t align,
                                      void *user_data) {
  (void)user_data;

  if (align <= DV_STACK_MALLOC_ALIGN)
    return realloc(ptr, new_size);

  void *mem = dv_stack_over_aligned_alloc(new_size, align);
  if (!mem)
    return NULL;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);

  dv_stack_over_aligned_free(ptr);
  return mem;
}

static void dv_stack_default_free(void *ptr, size_t size, size_t align,
                                  void *user_data) {
  (void)size;
  (void)user_data;

  if (align <= DV_STACK_MALLOC_ALIGN) {
    free(ptr);
    return;
  }
  dv_stack_over_aligned_free(ptr);
}

static dv_stack_allocator_t dv_stack_default_allocator(void) {
  const dv_stack_allocator_t hooks = {
      .alloc = dv_stack_default_alloc,
      .realloc = dv_stack_default_realloc,
      .free = dv_stack_default_free,
      .user_data = NULL,
  };
  return hooks;
}

// --- Buffer management -------------------------------------------------

// Moves the stack to exactly `new_capacity` slots. Only the live prefix is
// preserved, so a shrink that would drop live elements is refused rather than
// silently truncating.
static dv_stack_status_t dv_stack_set_capacity(dv_stack_t *s,
                                               size_t new_capacity) {
  if (new_capacity == s->capacity)
    return DV_STACK_OK;
  if (new_capacity < s->top)
    return DV_STACK_ERR_INVALID;

  size_t old_bytes = 0u;
  size_t new_bytes = 0u;

  dv_stack_status_t status =
      dv_stack_bytes_for(s->capacity, s->elem_size, &old_bytes);
  if (status != DV_STACK_OK)
    return status;

  status = dv_stack_bytes_for(new_capacity, s->elem_size, &new_bytes);
  if (status != DV_STACK_OK)
    return status;

  if (new_bytes == 0u) {
    if (s->buffer)
      s->allocator.free(s->buffer, old_bytes, s->align, s->allocator.user_data);
    s->buffer = NULL;
    s->capacity = 0u;
    return DV_STACK_OK;
  }

  void *mem = NULL;
  if (!s->buffer) {
    mem = s->allocator.alloc(new_bytes, s->align, s->allocator.user_data);
  } else if (s->allocator.realloc) {
    mem = s->allocator.realloc(s->buffer, old_bytes, new_bytes, s->align,
                               s->allocator.user_data);
  } else {
    mem = s->allocator.alloc(new_bytes, s->align, s->allocator.user_data);
    if (mem) {
      const size_t live = s->top * s->elem_size;
      if (live > 0u)
        memcpy(mem, s->buffer, live);
      s->allocator.free(s->buffer, old_bytes, s->align, s->allocator.user_data);
    }
  }

  if (!mem)
    return DV_STACK_ERR_NOMEM;

  s->buffer = (unsigned char *)mem;
  s->capacity = new_capacity;
  return DV_STACK_OK;
}

// 1.5x growth off a floor, saturating rather than wrapping.
static size_t dv_stack_next_capacity(size_t capacity) {
  if (capacity < DV_STACK_MIN_CAPACITY)
    return DV_STACK_MIN_CAPACITY;

  const size_t half = capacity / 2u;
  if (capacity > SIZE_MAX - half)
    return SIZE_MAX;

  return capacity + half;
}

// Clamps a desired capacity to the stack's ceiling, if it has one.
static size_t dv_stack_clamp_capacity(const dv_stack_t *s, size_t capacity) {
  if (s->max_capacity != 0u && capacity > s->max_capacity)
    return s->max_capacity;
  return capacity;
}

// --- Lifecycle ---------------------------------------------------------

dv_stack_status_t dv_stack_init(dv_stack_t *s, const dv_stack_config_t *config,
                                const dv_stack_allocator_t *allocator) {
  if (!s)
    return DV_STACK_ERR_INVALID;

  memset(s, 0, sizeof(*s));

  if (!config || config->elem_size == 0u)
    return DV_STACK_ERR_INVALID;

  size_t elem_align = config->elem_align;
  if (elem_align == 0u)
    elem_align = DV_STACK_MALLOC_ALIGN;
  if (!dv_stack_is_pow2(elem_align))
    return DV_STACK_ERR_INVALID;

  // A stride that is not a multiple of the element alignment misaligns every
  // element after the first. sizeof is always a multiple of alignof, so this
  // only rejects hand-rolled configurations.
  if ((config->elem_size & (elem_align - 1u)) != 0u)
    return DV_STACK_ERR_INVALID;

  dv_stack_allocator_t hooks;
  if (allocator) {
    if (!allocator->alloc || !allocator->free)
      return DV_STACK_ERR_INVALID;
    hooks = *allocator;
  } else {
    hooks = dv_stack_default_allocator();
  }

  size_t align = elem_align;
  if (config->cache_aligned && align < DV_CACHE_LINE_SIZE)
    align = DV_CACHE_LINE_SIZE;

  size_t max_capacity = config->max_capacity;
  if (!config->growable)
    max_capacity = config->initial_capacity;
  else if (max_capacity != 0u && max_capacity < config->initial_capacity)
    return DV_STACK_ERR_INVALID;

  s->elem_size = config->elem_size;
  s->align = align;
  s->initial_capacity = config->initial_capacity;
  s->max_capacity = max_capacity;
  s->allocator = hooks;
  s->growable = config->growable;

  // Slots are at multiples of elem_size from a buffer aligned to `align`, so
  // pointer-mode access is safe exactly when both are pointer-aligned.
  s->ptr_mode = (config->elem_size == sizeof(void *)) &&
                (align & (alignof(void *) - 1u)) == 0u;

  if (config->initial_capacity > 0u) {
    const dv_stack_status_t status =
        dv_stack_set_capacity(s, config->initial_capacity);
    if (status != DV_STACK_OK) {
      memset(s, 0, sizeof(*s));
      return status;
    }
  }

  return DV_STACK_OK;
}

dv_stack_status_t dv_stack_init_ptr(dv_stack_t *s, size_t initial_capacity,
                                    bool growable,
                                    const dv_stack_allocator_t *allocator) {
  const dv_stack_config_t config = {
      .elem_size = sizeof(void *),
      .elem_align = alignof(void *),
      .initial_capacity = initial_capacity,
      .max_capacity = 0u,
      .growable = growable,
      .cache_aligned = false,
  };

  return dv_stack_init(s, &config, allocator);
}

dv_stack_status_t dv_stack_reset(dv_stack_t *s) {
  if (!s)
    return DV_STACK_ERR_INVALID;

  s->top = 0u;
  return dv_stack_set_capacity(s, s->initial_capacity);
}

void dv_stack_destroy(dv_stack_t *s) {
  if (!s)
    return;

  if (s->buffer && s->allocator.free) {
    size_t bytes = 0u;
    if (dv_stack_bytes_for(s->capacity, s->elem_size, &bytes) != DV_STACK_OK)
      bytes = 0u;
    s->allocator.free(s->buffer, bytes, s->align, s->allocator.user_data);
  }

  memset(s, 0, sizeof(*s));
}

// --- Capacity management -----------------------------------------------

dv_stack_status_t dv_stack_reserve(dv_stack_t *s, size_t capacity) {
  if (!s)
    return DV_STACK_ERR_INVALID;
  if (capacity <= s->capacity)
    return DV_STACK_OK;
  if (!s->growable)
    return DV_STACK_ERR_FULL;
  if (s->max_capacity != 0u && capacity > s->max_capacity)
    return DV_STACK_ERR_FULL;

  return dv_stack_set_capacity(s, capacity);
}

dv_stack_status_t dv_stack_shrink_to_fit(dv_stack_t *s) {
  if (!s)
    return DV_STACK_ERR_INVALID;
  if (!s->growable || s->capacity == s->top)
    return DV_STACK_OK;

  return dv_stack_set_capacity(s, s->top);
}

dv_stack_status_t dv_stack_grow_for_push(dv_stack_t *s) {
  if (!s)
    return DV_STACK_ERR_INVALID;
  if (s->top < s->capacity)
    return DV_STACK_OK;
  if (!s->growable)
    return DV_STACK_ERR_FULL;

  const size_t next =
      dv_stack_clamp_capacity(s, dv_stack_next_capacity(s->capacity));
  if (next <= s->capacity)
    return DV_STACK_ERR_FULL;

  return dv_stack_set_capacity(s, next);
}

// --- Bulk operations ---------------------------------------------------

// Best-effort growth for a bulk push: aim for the whole request rounded up to
// the growth curve, fall back to exactly what was asked for, and let the
// caller settle for a partial push if neither allocation succeeds.
static void dv_stack_grow_for_bulk(dv_stack_t *s, size_t needed) {
  const size_t curve = dv_stack_next_capacity(s->capacity);
  const size_t target =
      dv_stack_clamp_capacity(s, needed > curve ? needed : curve);

  if (target > s->capacity && dv_stack_set_capacity(s, target) == DV_STACK_OK)
    return;

  const size_t exact = dv_stack_clamp_capacity(s, needed);
  if (exact > s->capacity && exact < target)
    (void)dv_stack_set_capacity(s, exact);
}

size_t dv_stack_push_bulk(dv_stack_t *s, const void *elems, size_t count) {
  if (!s || !elems || count == 0u)
    return 0u;

  size_t room = s->capacity - s->top;
  if (room < count && s->growable && count <= SIZE_MAX - s->top) {
    dv_stack_grow_for_bulk(s, s->top + count);
    room = s->capacity - s->top;
  }

  const size_t n = count < room ? count : room;
  if (n == 0u)
    return 0u;

  memcpy(s->buffer + (s->top * s->elem_size), elems, n * s->elem_size);
  s->top += n;
  return n;
}

size_t dv_stack_pop_bulk(dv_stack_t *s, void *out, size_t count) {
  if (!s || count == 0u)
    return 0u;

  const size_t n = count < s->top ? count : s->top;
  if (n == 0u)
    return 0u;

  if (out) {
    const size_t elem_size = s->elem_size;
    unsigned char *dst = (unsigned char *)out;

    // Successive-pop order: out[0] is the former top.
    for (size_t i = 0u; i < n; ++i)
      memcpy(dst + (i * elem_size), s->buffer + ((s->top - 1u - i) * elem_size),
             elem_size);
  }

  s->top -= n;
  return n;
}

// --- Diagnostics -------------------------------------------------------

const char *dv_stack_status_str(dv_stack_status_t status) {
  switch (status) {
  case DV_STACK_OK:
    return "DV_STACK_OK";
  case DV_STACK_ERR_INVALID:
    return "DV_STACK_ERR_INVALID";
  case DV_STACK_ERR_EMPTY:
    return "DV_STACK_ERR_EMPTY";
  case DV_STACK_ERR_FULL:
    return "DV_STACK_ERR_FULL";
  case DV_STACK_ERR_NOMEM:
    return "DV_STACK_ERR_NOMEM";
  case DV_STACK_ERR_OVERFLOW:
    return "DV_STACK_ERR_OVERFLOW";
  }

  return "DV_STACK_ERR_UNKNOWN";
}
