#include <iterator>
#include <stdint.h>
#include <map>

#include "handle_map.h"

struct handle_map {
    uint32_t next;
    std::map<uint32_t, void*> map;
};

struct handle_map_iter {
    handle_map *map;
    std::map<uint32_t, void*>::iterator it;
};

extern "C" handle_map *handle_map_new() {
    handle_map *map = new handle_map({1, {}});
    return map;
}

extern "C" void handle_map_destroy(handle_map *map) {
    delete map;
}

extern "C" uint32_t handle_map_add(handle_map *map, void *value) {
    uint32_t key = map->next++;
    map->map.insert({key, value});
    return key;
}

extern "C" bool handle_map_remove(handle_map *map, uint32_t handle) {
    auto entry = map->map.find(handle);
    if (entry == map->map.end()) {
        return false;
    } else {
        map->map.erase(entry);
        return true;
    }
}

extern "C" bool handle_map_get(handle_map *map, uint32_t handle, void **out) {
    auto entry = map->map.find(handle);
    if (entry == map->map.end()) {
        return false;
    } else {
        *out = entry->second;
        return true;
    }
}

extern "C" handle_map_iter *handle_map_iter_new(handle_map *map) {
    handle_map_iter *iter = new handle_map_iter();
    iter->map = map;
    iter->it = map->map.begin();
    return iter;
}

extern "C" void handle_map_iter_destroy(handle_map_iter *iter) {
    delete iter;
}

extern "C" bool handle_map_iter_next(handle_map_iter *iter, uint32_t *handle_out, void **value_out) {
    if (iter->it == iter->map->map.end()) {
        return false;
    }
    if (handle_out != NULL) {
        *handle_out = iter->it->first;
    }
    if (value_out != NULL) {
        *value_out = iter->it->second;
    }
    ++iter->it;
    return true;
}