#pragma once

#include <libmemcached/memcached.h>

// Compare, print, and exit
#define CPE(val, err_code, format, ...)                                             \
    if (val) {                                                                      \
        fprintf(stderr, "%s(%u): " format "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(err_code);                                                             \
    }

struct dory_registry {
    memcached_st* memc;
    char *ip;
    int port;
};

struct dory_registry *dory_registry_create(char *ip, int port);
struct dory_registry *dory_registry_create_from(struct dory_registry *reg);
void dory_registry_free(struct dory_registry *registry);

void dory_registry_publish(const struct dory_registry *registry, const char* key, void* value, int len);
int dory_registry_get_published(const struct dory_registry *registry, const char* key, void** value);

void dory_registry_publish_ready(const struct dory_registry *registry, const char* qp_name);
void dory_registry_wait_till_ready(const struct dory_registry *registry, const char* qp_name);