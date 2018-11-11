#ifndef ALLONET_STATE_H
#define ALLONET_STATE_H
#include <sys/queue.h>

typedef struct {
    double zmovement; // 1 = maximum speed forwards
    double xmovement; // 1 = maximum speed strafe right
    double yaw; // rotation around x
    double pitch; // rotation around y
} allo_client_intent;

typedef struct {
    double x, y, z;
} allo_vector;

typedef struct allo_entity {
    const char *id;
    allo_vector position;
    allo_vector rotation;

    LIST_ENTRY(allo_entity) pointers;
} allo_entity;

allo_entity *entity_create(const char *id);
void entity_destroy(allo_entity);

typedef struct {
    long long revision;
    LIST_HEAD(allo_entity_list, allo_entity) entities;
} allo_state;

#endif
