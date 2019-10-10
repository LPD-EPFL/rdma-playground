#pragma once

#include <stdint.h>
#include "toml.h"

#ifdef __cplusplus
extern "C" {
#endif

toml_table_t *toml_load_conf(char const *filename);
void toml_parse_registry(toml_table_t *conf, char **host, int64_t *port);
void toml_parse_general(toml_table_t *conf, int64_t *clients, int64_t *id);

// Optional
void toml_parse_ips(toml_table_t *conf, char ***ips, int *len);
char *toml_getenv(const char *name);

#ifdef __cplusplus
}
#endif