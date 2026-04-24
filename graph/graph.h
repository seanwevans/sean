#ifndef GRAPH_H
#define GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INVALID_INDEX 0xFFFFFFFF

typedef struct {
  uint32_t index;
  uint32_t generation;
} vertex_t;

typedef struct {
  uint32_t index;
  uint32_t generation;
} edge_t;

typedef struct {
  void *(*alloc)(size_t size, void *user_data);
  void (*free)(void *ptr, void *user_data);
  void *user_data;
} allocator_t;

typedef struct {
  bool is_directed;
  bool is_weighted;
  uint32_t initial_vertex_capacity;
  uint32_t initial_edge_capacity;
  bool fixed_capacity;
} graph_config_t;

typedef struct graph_s graph_t;

graph_t *graph_init(const graph_config_t *config, const allocator_t *allocator);
void graph_reset(graph_t *graph);
void graph_destroy(graph_t *graph);

bool graph_begin_update(graph_t *graph);
void graph_commit_update(graph_t *graph);

vertex_t graph_add_vertex(graph_t *graph);
bool graph_remove_vertex(graph_t *graph, vertex_t vertex);

edge_t graph_add_edge(graph_t *graph, vertex_t src, vertex_t dst, float weight);
bool graph_remove_edge(graph_t *graph, edge_t edge);

bool graph_is_vertex_valid(const graph_t *graph, vertex_t vertex);
uint32_t graph_get_degree(const graph_t *graph, vertex_t vertex);

#endif  // GRAPH_H