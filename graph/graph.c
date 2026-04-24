#include "graph.h"

typedef struct {
  uint32_t generation;
  uint32_t first_edge;
  uint32_t last_edge;
  uint32_t degree;
  bool active;
  union {
    uint32_t next_free;
  };
} vertex_data_t;

typedef struct {
  uint32_t generation;
  uint32_t dest_vertex_idx;
  uint32_t next_edge;
  float weight;
  bool active;
  union {
    uint32_t next_free;
  };
} edge_data_t;

struct graph_s {
  allocator_t allocator;
  graph_config_t config;

  vertex_data_t *vertices;
  uint32_t vertex_capacity;
  uint32_t vertex_count;
  uint32_t vertex_free_head;

  edge_data_t *edges;
  uint32_t edge_capacity;
  uint32_t edge_count;
  uint32_t edge_free_head;

  bool in_update_phase;
};

static void zero_mem(void *ptr, size_t size) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0; i < size; ++i) {
    p[i] = 0;
  }
}

graph_t *graph_init(const graph_config_t *config,
                    const allocator_t *allocator) {
  if (!config || !allocator || !allocator->alloc)
    return NULL;

  graph_t *graph = allocator->alloc(sizeof(graph_t), allocator->user_data);
  if (!graph)
    return NULL;

  graph->allocator = *allocator;
  graph->config = *config;
  graph->in_update_phase = false;

  size_t v_size = sizeof(vertex_data_t) * config->initial_vertex_capacity;
  graph->vertices = allocator->alloc(v_size, allocator->user_data);
  zero_mem(graph->vertices, v_size);

  graph->vertex_capacity = config->initial_vertex_capacity;
  graph->vertex_count = 0;

  for (uint32_t i = 0; i < graph->vertex_capacity; ++i) {
    graph->vertices[i].next_free = i + 1;
    graph->vertices[i].active = false;
    graph->vertices[i].generation = 1;
  }
  graph->vertices[graph->vertex_capacity - 1].next_free = INVALID_INDEX;
  graph->vertex_free_head = 0;

  return graph;
}

bool graph_begin_update(graph_t *graph) {
  if (graph->in_update_phase)
    return false;
  graph->in_update_phase = true;
  return true;
}

void graph_commit_update(graph_t *graph) { graph->in_update_phase = false; }

vertex_t graph_add_vertex(graph_t *graph) {
  vertex_t invalid_handle = {INVALID_INDEX, 0};
  if (!graph->in_update_phase)
    return invalid_handle;

  if (graph->vertex_free_head == INVALID_INDEX) {
    if (graph->config.fixed_capacity) {
      return invalid_handle;
    }
  }

  uint32_t new_idx = graph->vertex_free_head;
  vertex_data_t *v_data = &graph->vertices[new_idx];

  graph->vertex_free_head = v_data->next_free;

  v_data->active = true;
  v_data->first_edge = INVALID_INDEX;
  v_data->last_edge = INVALID_INDEX;
  v_data->degree = 0;

  graph->vertex_count++;

  vertex_t handle = {new_idx, v_data->generation};
  return handle;
}

bool graph_is_vertex_valid(const graph_t *graph, vertex_t vertex) {
  if (vertex.index >= graph->vertex_capacity)
    return false;
  vertex_data_t *v_data = &graph->vertices[vertex.index];
  return v_data->active && (v_data->generation == vertex.generation);
}

bool graph_remove_vertex(graph_t *graph, vertex_t vertex) {
  if (!graph->in_update_phase || !graph_is_vertex_valid(graph, vertex)) {
    return false;
  }

  vertex_data_t *v_data = &graph->vertices[vertex.index];

  v_data->active = false;
  v_data->generation++;
  v_data->next_free = graph->vertex_free_head;
  graph->vertex_free_head = vertex.index;

  graph->vertex_count--;
  return true;
}