// dv_graph_mt.h - opt-in mutex-guarded wrapper around dv_graph_t.
//
// The core graph is unsynchronized on purpose. This wrapper is the explicit
// opt-in for callers that need a shared graph, and it lives in its own
// translation unit so the single-threaded hot path never links pthreads.
//
// It guards with a mutex rather than the reader/writer lock the concurrency
// model would suggest, because pthread_rwlock_t is not visible to a strictly
// conforming C translation unit without a feature-test macro, and a public
// header is the wrong place to set one. Concurrent readers therefore remain
// what the design says they are: the caller's own coordination.
//
// Note that a traversal is not a reader here in any case. A breadth- or
// depth-first walk reads the topology but writes the scratch the graph owns,
// so two threads cannot traverse one graph even under a shared lock.
//
// As with the stack and heap wrappers, no interior pointers escape: an
// adjacency iterator is valid only until the next mutation, which cannot be
// honored once the lock is released, so iteration goes through
// dv_graph_mt_read and compound mutations through dv_graph_mt_write.

#ifndef DV_GRAPH_MT_H
#define DV_GRAPH_MT_H

#include "dv_graph.h"

#include <pthread.h>

typedef struct {
  alignas(DV_CACHE_LINE_SIZE) pthread_mutex_t lock;
  dv_graph_t graph;
} dv_graph_mt_t;

// On failure the wrapper is left uninitialized and must not be passed to
// dv_graph_mt_destroy; the lock is already released by the failing call.
dv_graph_status_t dv_graph_mt_init(dv_graph_mt_t *m,
                                   const dv_graph_config_t *config,
                                   const dv_graph_allocator_t *allocator);

void dv_graph_mt_destroy(dv_graph_mt_t *m);

// --- Mutations ---------------------------------------------------------

dv_graph_status_t dv_graph_mt_add_vertex(dv_graph_mt_t *m, dv_vertex_t *out);
dv_graph_status_t dv_graph_mt_remove_vertex(dv_graph_mt_t *m,
                                            dv_vertex_t vertex);
dv_graph_status_t dv_graph_mt_add_edge(dv_graph_mt_t *m, dv_vertex_t src,
                                       dv_vertex_t dst, double weight,
                                       dv_edge_t *out);
dv_graph_status_t dv_graph_mt_remove_edge(dv_graph_mt_t *m, dv_edge_t edge);
void dv_graph_mt_clear(dv_graph_mt_t *m);

// --- Queries -----------------------------------------------------------

uint32_t dv_graph_mt_vertex_count(dv_graph_mt_t *m);
uint32_t dv_graph_mt_edge_count(dv_graph_mt_t *m);
uint32_t dv_graph_mt_degree(dv_graph_mt_t *m, dv_vertex_t vertex);
bool dv_graph_mt_has_edge(dv_graph_mt_t *m, dv_vertex_t src, dv_vertex_t dst);
dv_graph_status_t dv_graph_mt_find_edge(dv_graph_mt_t *m, dv_vertex_t src,
                                        dv_vertex_t dst, dv_edge_t *out);

// Read-only in topology, but still a mutation of the scratch: see above.
dv_graph_status_t dv_graph_mt_reachable(dv_graph_mt_t *m, dv_vertex_t from,
                                        dv_vertex_t to, bool *out);
dv_graph_status_t dv_graph_mt_bfs(dv_graph_mt_t *m, dv_vertex_t start,
                                  dv_graph_visit_fn visit, void *user_data);

// --- Compound critical sections ----------------------------------------
//
// `fn` must not re-enter the wrapper. Handles obtained under one acquisition
// are already stale under the next, so anything that reads a handle and then
// acts on it belongs in one of these.

dv_graph_status_t dv_graph_mt_read(dv_graph_mt_t *m,
                                   void (*fn)(const dv_graph_t *g,
                                              void *user_data),
                                   void *user_data);
dv_graph_status_t dv_graph_mt_write(dv_graph_mt_t *m,
                                    void (*fn)(dv_graph_t *g, void *user_data),
                                    void *user_data);

#endif // DV_GRAPH_MT_H
