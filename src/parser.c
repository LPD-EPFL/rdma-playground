#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "parser.h"
#include "registry.h"

static bool is_valid_ip(char *ip_address) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    return inet_pton(AF_INET, ip_address, &(sa.sin_addr));
}

// Return the environment variable @name if it is set. Exit if not.
char* toml_getenv(const char* name) {
    char *env = getenv(name);
    CPE(env == NULL, -1, "Environment variable %s not set", name);
    return env;
}

toml_table_t *toml_load_conf(char const *filename) {
    FILE *fp;
    toml_table_t *conf;

    char errbuf[200];

    // open file and parse
    if (0 == (fp = fopen(filename, "r"))) {
        perror("fopen");
	    exit(1);
    }

    conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if(0 == conf) {
        fprintf(stderr, "ERROR: %s\n", errbuf);
	    exit(1);
    }

    return conf;
}

void toml_parse_registry(toml_table_t *conf, char **host, int64_t *port) {
    toml_table_t *registry;
    const char* raw;

    // Locate the [registry] table
    if (0 == (registry = toml_table_in(conf, "registry"))) {
        fprintf(stderr, "ERROR: missing [registry]\n");
        toml_free(conf);
        exit(1);
    }

    // Extract host config value
    if (0 == (raw = toml_raw_in(registry, "host"))) {
        fprintf(stderr, "ERROR: missing 'host' in [registry]\n");
        toml_free(conf);
        exit(1);
    }
    if (toml_rtos(raw, host) || !is_valid_ip(*host)) {
        fprintf(stderr, "ERROR: bad value in 'host'\n");
        toml_free(conf);
        exit(1);
    }

    // Extract port config value
    if (0 == (raw = toml_raw_in(registry, "port"))) {
        fprintf(stderr, "ERROR: missing 'port' in [registry]\n");
        free(host);
        toml_free(conf);
        exit(1);
    }

    if (toml_rtoi(raw, port)) {
        fprintf(stderr, "ERROR: bad value in 'port'\n");
        free(host);
        toml_free(conf);
        exit(1);
    }
}

void toml_parse_general(toml_table_t *conf, int64_t *clients, int64_t *id) {
    toml_table_t *general;
    const char* raw;

    // Locate the [general] table
    if (0 == (general = toml_table_in(conf, "general"))) {
        fprintf(stderr, "ERROR: missing [general]\n");
        toml_free(conf);
        exit(1);
    }

    // Extract clients config value
    if (0 == (raw = toml_raw_in(general, "clients"))) {
        fprintf(stderr, "ERROR: missing 'clients' in [general]\n");
        toml_free(conf);
        exit(1);
    }

    if (toml_rtoi(raw, clients) || *clients <= 0) {
        fprintf(stderr, "ERROR: bad value in 'clients'\n");
        toml_free(conf);
        exit(1);
    }

    // Extract id config value
    if (0 == (raw = toml_raw_in(general, "id"))) {
        fprintf(stderr, "ERROR: missing 'id' in [general]\n");
        toml_free(conf);
        exit(1);
    }

    if (toml_rtoi(raw, id) || *id < 0 || *id >= *clients ) {
        fprintf(stderr, "ERROR: bad value in 'id'\n");
        toml_free(conf);
        exit(1);
    }
}

void toml_parse_ips(toml_table_t *conf, char ***ips, int *len) {
    toml_array_t *ip_arr;
    const char* raw;

    // Locate the [general] table
    if (0 == (ip_arr = toml_array_in(conf, "ips"))) {
        fprintf(stderr, "ERROR: missing `ips`\n");
        toml_free(conf);
        exit(1);
    }

    int ips_cnt = *len = toml_array_nelem(ip_arr);


    *ips = malloc(ips_cnt * sizeof(char *));
    if (*ips == NULL) {
        fprintf(stderr, "ERROR: could not allocate memory\n");
        toml_free(conf);
        exit(1);
    }

    memset(*ips, 0, ips_cnt * sizeof(char *));

    for (int i = 0; i < ips_cnt; i++) {
        if (0 == (raw = toml_raw_at(ip_arr, i))) {
            fprintf(stderr, "ERROR: could not parse `ip` at index %d\n", i);

            for (int j = 0; i < ips_cnt; j++) {
                free((*ips)[i]);
            }

            free(*ips);

            toml_free(conf);
            exit(1);
        }

        char *host = NULL;
        if (toml_rtos(raw, &host) || !is_valid_ip(host)) {
            fprintf(stderr, "ERROR: bad value in 'host'\n");

            for (int j = 0; i < ips_cnt; j++) {
                free((*ips)[i]);
            }

            free(*ips);

            toml_free(conf);
            exit(1);
        }

        (*ips)[i] = host;
    }
}