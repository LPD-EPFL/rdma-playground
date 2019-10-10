#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "registry.h"

struct dory_registry *dory_registry_create(char *ip, int port) {
    struct dory_registry *registry = malloc(sizeof(*registry));
    CPE(!registry, -1, "Couldn't allocate memory for the registry");

    registry->ip = ip;
    registry->port = port;

    memcached_server_st* servers = NULL;
    memcached_st* memc = memcached_create(NULL);
    memcached_return rc;

    memc = memcached_create(NULL);
    servers = memcached_server_list_append(servers, registry->ip,
                                              registry->port, &rc);
    rc = memcached_server_push(memc, servers);
    CPE(rc != MEMCACHED_SUCCESS, rc, "Couldn't add memcached server");

    registry->memc = memc;

    return registry;
}

struct dory_registry *dory_registry_create_from(struct dory_registry *reg) {
    char *ip = malloc((strlen(reg->ip) + 1) * sizeof(char));
    strcpy(ip, reg->ip);

    return dory_registry_create(ip, reg->port);
}

void dory_registry_free(struct dory_registry *registry) {
    free(registry->ip);
    memcached_free(registry->memc);
    free(registry);
    registry = NULL;
}

void dory_registry_publish(const struct dory_registry *registry, const char* key, void* value, int len) {
    CPE(key == NULL || value == NULL || len == 0, -1, "Invalid call to dory_publish");
    memcached_return rc;

    rc = memcached_set(registry->memc, key, strlen(key), (const char*)value,
                        len, (time_t)0, (uint32_t)0);

    if (rc != MEMCACHED_SUCCESS) {
        CPE(true, rc, "Failed to publish key %s to "
                      "registry with IP = %s. Error %s.",
                      key, registry->ip, memcached_strerror(registry->memc, rc));
    }
}

/*
 * Get the value associated with "key" into "value", and return the length
 * of the value. If the key is not found, return NULL and len -1. For all
 * other errors, terminate.
 *
 * This function sometimes gets called in a polling loop - ensure that there
 * are no memory leaks or unterminated memcached connections! We don't need
 * to free() the resul of getenv() since it points to a string in the process
 * environment.
 */
int dory_registry_get_published(const struct dory_registry *registry, const char* key, void** value) {
    CPE(key == NULL, -1, "Invalid call to dory_get_published");

    memcached_return rc;
    size_t value_length;
    uint32_t flags;

    *value = memcached_get(registry->memc, key, strlen(key), &value_length, &flags, &rc);

    if (rc == MEMCACHED_SUCCESS) {
        return (int)value_length;
    } else if (rc == MEMCACHED_NOTFOUND) {
        return -1;
    } else {
        if (rc != MEMCACHED_SUCCESS) {
            CPE(true, rc, "Failed to find value for the key %s in "
                        "registry with IP = %s. Error %s.",
                        key, registry->ip, memcached_strerror(registry->memc, rc));
        }
    }

    CPE(true, -1, "Unreachable");
}

/*
 * To advertise a queue pair with name qp_name as ready, we publish this
 * key-value mapping: "HRD_RESERVED_NAME_PREFIX-qp_name" -> "hrd_ready". This
 * requires that a qp_name never starts with HRD_RESERVED_NAME_PREFIX.
 *
 * This avoids overwriting the memcached entry for qp_name which might still
 * be needed by the remote peer.
 */
void dory_registry_publish_ready(const struct dory_registry *registry, const char* qp_name) {
    char value[ sizeof("dory_ready") ];
    sprintf(value, "%s", "dory_ready");
    dory_registry_publish(registry, qp_name, value, strlen(value));
}

/*
 * To check if a queue pair with name qp_name is ready, we check if this
 * key-value mapping exists: "HRD_RESERVED_NAME_PREFIX-qp_name" -> "hrd_ready".
 */
void dory_registry_wait_till_ready(const struct dory_registry *registry, const char* qp_name) {
    char exp_value[ sizeof("dory_ready") ];
    sprintf(exp_value, "%s", "dory_ready");

    int tries = 0;
    while (true) {
        char *value;

        int ret = dory_registry_get_published(registry, qp_name, (void**)&value);
        tries++;
        if (ret > 0) {
            if (strcmp(value, exp_value) == 0) {
                free(value);
                return;
            }
        }

        usleep(200000);

        if (tries > 100) {
            fprintf(stderr, "Waiting for QPs %s to be ready\n", qp_name);
            tries = 0;
        }
    }
}
