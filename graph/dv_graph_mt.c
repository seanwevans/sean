// dv_graph_mt.c - mutex-guarded graph wrapper.

#include "dv_graph_mt.h"

// A failed lock or unlock means the mutex is corrupt or the caller violated
// the ownership contract; neither is recoverable inside a container, so the
// result is checked in debug builds and deliberately ignored otherwise.
static void dv_graph_mt_lock(dv_graph_mt_t *m) {
  const int err = pthread_mutex_lock(&m->lock);
  DV_GRAPH_ASSERT(err == 0);
  (void)err;
}

static void dv_graph_mt_unlock(dv_graph_mt_t *m) {
  const int err = pthread_mutex_unlock(&m->lock);
  DV_GRAPH_ASSERT(err == 0);
  (void)err;
}

dv_graph_status_t dv_graph_mt_init(dv_graph_mt_t *m,
                                   const dv_graph_config_t *config,
                                   const dv_graph_allocator_t *allocator) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    memset(&m->graph, 0, sizeof(m->graph));
    return DV_GRAPH_ERR_NOMEM;
  }

  const dv_graph_status_t status = dv_graph_init(&m->graph, config, allocator);
  if (status != DV_GRAPH_OK)
    (void)pthread_mutex_destroy(&m->lock);

  return status;
}

void dv_graph_mt_destroy(dv_graph_mt_t *m) {
  if (!m)
    return;

  dv_graph_destroy(&m->graph);
  (void)pthread_mutex_destroy(&m->lock);
}

dv_graph_status_t dv_graph_mt_add_vertex(dv_graph_mt_t *m, dv_vertex_t *out) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status = dv_graph_add_vertex(&m->graph, out);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_remove_vertex(dv_graph_mt_t *m,
                                            dv_vertex_t vertex) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status = dv_graph_remove_vertex(&m->graph, vertex);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_add_edge(dv_graph_mt_t *m, dv_vertex_t src,
                                       dv_vertex_t dst, double weight,
                                       dv_edge_t *out) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status =
      dv_graph_add_edge(&m->graph, src, dst, weight, out);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_remove_edge(dv_graph_mt_t *m, dv_edge_t edge) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status = dv_graph_remove_edge(&m->graph, edge);
  dv_graph_mt_unlock(m);
  return status;
}

void dv_graph_mt_clear(dv_graph_mt_t *m) {
  if (!m)
    return;

  dv_graph_mt_lock(m);
  dv_graph_clear(&m->graph);
  dv_graph_mt_unlock(m);
}

uint32_t dv_graph_mt_vertex_count(dv_graph_mt_t *m) {
  if (!m)
    return 0u;

  dv_graph_mt_lock(m);
  const uint32_t count = dv_graph_vertex_count(&m->graph);
  dv_graph_mt_unlock(m);
  return count;
}

uint32_t dv_graph_mt_edge_count(dv_graph_mt_t *m) {
  if (!m)
    return 0u;

  dv_graph_mt_lock(m);
  const uint32_t count = dv_graph_edge_count(&m->graph);
  dv_graph_mt_unlock(m);
  return count;
}

uint32_t dv_graph_mt_degree(dv_graph_mt_t *m, dv_vertex_t vertex) {
  if (!m)
    return 0u;

  dv_graph_mt_lock(m);
  const uint32_t degree = dv_graph_degree(&m->graph, vertex);
  dv_graph_mt_unlock(m);
  return degree;
}

bool dv_graph_mt_has_edge(dv_graph_mt_t *m, dv_vertex_t src, dv_vertex_t dst) {
  if (!m)
    return false;

  dv_graph_mt_lock(m);
  const bool found = dv_graph_has_edge(&m->graph, src, dst);
  dv_graph_mt_unlock(m);
  return found;
}

dv_graph_status_t dv_graph_mt_find_edge(dv_graph_mt_t *m, dv_vertex_t src,
                                        dv_vertex_t dst, dv_edge_t *out) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status =
      dv_graph_find_edge(&m->graph, src, dst, out);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_reachable(dv_graph_mt_t *m, dv_vertex_t from,
                                        dv_vertex_t to, bool *out) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  // The walk writes the visited set and the frontier, so it holds the lock
  // for the whole traversal.
  dv_graph_mt_lock(m);
  const dv_graph_status_t status = dv_graph_reachable(&m->graph, from, to, out);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_bfs(dv_graph_mt_t *m, dv_vertex_t start,
                                  dv_graph_visit_fn visit, void *user_data) {
  if (!m)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  const dv_graph_status_t status =
      dv_graph_bfs(&m->graph, start, visit, user_data);
  dv_graph_mt_unlock(m);
  return status;
}

dv_graph_status_t dv_graph_mt_read(dv_graph_mt_t *m,
                                   void (*fn)(const dv_graph_t *g,
                                              void *user_data),
                                   void *user_data) {
  if (!m || !fn)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  fn(&m->graph, user_data);
  dv_graph_mt_unlock(m);
  return DV_GRAPH_OK;
}

dv_graph_status_t dv_graph_mt_write(dv_graph_mt_t *m,
                                    void (*fn)(dv_graph_t *g, void *user_data),
                                    void *user_data) {
  if (!m || !fn)
    return DV_GRAPH_ERR_INVALID;

  dv_graph_mt_lock(m);
  fn(&m->graph, user_data);
  dv_graph_mt_unlock(m);
  return DV_GRAPH_OK;
}
