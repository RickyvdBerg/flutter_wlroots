#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

struct handle_map;
struct handle_map_iter;

struct handle_map *handle_map_new();
void handle_map_destroy(struct handle_map *map);
uint32_t handle_map_add(struct handle_map *map, void *value);
bool handle_map_get(struct handle_map *map, uint32_t handle, void **out);
bool handle_map_remove(struct handle_map *map, uint32_t handle);

// Iterator for traversing handle_map entries
struct handle_map_iter *handle_map_iter_new(struct handle_map *map);
void handle_map_iter_destroy(struct handle_map_iter *iter);
bool handle_map_iter_next(struct handle_map_iter *iter, uint32_t *handle_out, void **value_out);

#ifdef __cplusplus
}
#endif