//
//  asset_store.c
//  allonet
//
//  Created by Patrik on 2020-11-27.
//

#include "asset_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <assert.h>

#define max(a,b) \
({ __typeof__ (a) _a = (a); \
__typeof__ (b) _b = (b); \
_a > _b ? _a : _b; })

#define min(a,b) \
({ __typeof__ (a) _a = (a); \
__typeof__ (b) _b = (b); \
_a < _b ? _a : _b; })

#define log(f_, ...) printf((f_), ##__VA_ARGS__)

assetstore *assetstore_open(const char *disk_path) {
    assetstore *store = cJSON_malloc(sizeof(assetstore));
    
    store->disk_path = strdup(disk_path);
    
    char *statefile = NULL;
    asprintf(&statefile, "%s/%s", disk_path, "state.json");
    FILE *f = fopen(statefile, "r");
    if (f) {
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            log("assetstore: Unable to seek state file\n");
            return NULL;
        }
        size_t size = ftell(f);
        if(size == 0) {
            log("assetstore: statefile is empty\n");
            store->state = cJSON_CreateObject();
            fclose(f);
            return store;
        }
        char *contents = (char *)malloc(size);
        if(contents == NULL) {
            log("assetstore: could not fit statefile in memory\n");
            store->state = cJSON_CreateObject();
            fclose(f);
            return store;
        }
        if (fread(contents, 1, size, f) != size) {
            log("assetstore: Failed to read the whole statefile\n");
            store->state = cJSON_CreateObject();
            free(contents);
            fclose(f);
            return store;
        }
        
        fclose(f);
        store->state = cJSON_Parse(contents);
        
        if (store->state == NULL) {
            log("assetstore: Failed to parse statefile\n");
            store->state = cJSON_CreateObject();
            free(contents);
            return store;
        }
        
        free(contents);
    } else {
        store->state = cJSON_CreateObject();
    }
}

void assetstore_close(assetstore *store) {
    free((void*)store->disk_path);
    cJSON_Delete(store->state);
    store->disk_path = NULL;
    store->state = NULL;
}

int assetstore_state(assetstore *store, const char *asset_id, int *out_exists, int *out_complete, size_t *out_regions_count) {
    assert(store);
    assert(asset_id);
    
    cJSON *state = cJSON_GetObjectItem(store->state, asset_id);
    // check existance
    if (state == NULL) {
        if (out_exists) *out_exists = 0;
        if (out_complete) *out_complete = 0;
        return 0;
    }
    assert(cJSON_IsObject(state));
    if (!cJSON_IsObject(state)) {
        cJSON_DeleteItemFromObject(store->state, asset_id);
        return 1;
    }
    
    if (out_complete) {
        cJSON *complete = cJSON_GetObjectItem(state, "complete");
        if (complete == NULL) {
            *out_complete = 0;
        } else {
            assert(cJSON_IsBool(complete));
            *out_complete = complete->valueint;
        }
    }
    
}

size_t assetstore_get_missing_ranges(assetstore *store, const char *asset_id, size_t start, size_t count, size_t **out_ranges) {
    assert(out_ranges);
    
    cJSON *state = cJSON_GetObjectItem(store->state, asset_id);
    // check existance
    if (state == NULL) {
        return 0;
    }
    
    cJSON *ranges = cJSON_GetObjectItem(state, "ranges");
    assert(cJSON_IsArray(ranges));
    int end = max(start + count, cJSON_GetArraySize(ranges));
    
    for (int i = start; i < end; i++) {
        cJSON *range = cJSON_GetArrayItem(ranges, i);
        assert(cJSON_IsArray(range));
        *out_ranges[i++] = (size_t)cJSON_GetArrayItem(range, 0)->valueint;
        *out_ranges[i++] = (size_t)cJSON_GetArrayItem(range, 1)->valueint;
    }
    
    return end - start;
}

char *_asset_path(assetstore *store, const char *id) {
    char *path = NULL;
    asprintf(&path, "%s/%s", store->disk_path, id);
    return path;
}

size_t _first(cJSON *range) {
    return (size_t)cJSON_GetArrayItem(range, 0)->valueint;
}

size_t _last(cJSON *range) {
    return (size_t)cJSON_GetArrayItem(range, 1)->valueint;
}

cJSON *_range(cJSON *ranges, int index) {
    return cJSON_GetArrayItem(ranges, index);
}

void _merge_range(cJSON *ranges, size_t start, size_t end) {
    int ranges_count = cJSON_GetArraySize(ranges);
    if (ranges_count == 0) {
        int numbers[2] = {(int)start, (int)end };
        cJSON *r = cJSON_CreateIntArray(numbers, 2);
        cJSON_AddItemToArray(ranges, r);
        return;
    }
    
    int i = 0, low = 0, high = 0;
    size_t value;
    cJSON *range;
    // find the index `i` where to insert the new range, based on start value
    while (low <= high) {
        i = (low + high) / 2;
        range = cJSON_GetArrayItem(ranges, i);
        assert(range);
        value = _first(range);
        if (value < start) {
            low = i + 1;
        } else if (value > start) {
            high = i - 1;
        } else {
            break;
        }
    }
    // We want to end at the range that is bigger, or at end of list
    if (start > value) {
        ++i;
    }
    
    // Merge all ranges that intersects to the right
    // Can be many as the length is unconstrained
    range = cJSON_GetArrayItem(ranges, i);
    while (range != NULL && (end >= _first(range) - 1 && start <= _last(range) + 1)) {
        start = min(start, _first(range));
        end = max(end, _last(range));
        cJSON_DeleteItemFromArray(ranges, i);
    }
    
    // Merge all intersecting to the left
    // Can be at most one as we sorted by start
    range = i > 0 ? _range(ranges, i-1) : NULL;
    if (range && start <= _last(range) + 1) {
        start = min(start, _first(range));
        end = max(end, _last(range));
        cJSON_DeleteItemFromArray(ranges, i-1);
        --i;
    }
    
    // Insert the new range
    int numbers[2] = {(int)start, (int)end };
    cJSON *r = cJSON_CreateIntArray(numbers, 2);
    cJSON_AddItemToArray(ranges, r);
    return;
}

int assetstore_write(assetstore *store, const char *asset_id, size_t offset, const u_int8_t *data, size_t length) {
    assert(asset_id);
    assert(data);
    
    char *fpath = _asset_path(store, asset_id);
    
    cJSON *state = cJSON_GetObjectItem(store->state, asset_id);
    cJSON *ranges = NULL;
    if (!state) {
        state = cJSON_CreateObject();
        ranges = cJSON_AddArrayToObject(state, "ranges");
    } else {
        ranges = cJSON_GetObjectItem(state, "ranges");
        // if there is a state then there is a "ranges"
        assert(ranges);
    }
    
    _merge_range(ranges, offset, offset + length);
}
