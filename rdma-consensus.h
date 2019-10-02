#ifndef RDMA_CONSENSUS_H
#define RDMA_CONSENSUS_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#define RDMA_WRID 3
#define SHORT_SLEEP_DURATION 1


#include "ibv_layer.h"
#include "utils.h"
#include "leader-election.h"


int die(const char *reason);

void tcp_client_connect();
void tcp_server_listen();
void count_lines(char* filename, struct global_context *ctx);
void parse_config(char* filename, struct global_context *ctx);
bool compare_to_self(struct ifaddrs *ifaddr, char *addr);

void init_ctx_common(struct global_context* ctx, bool is_le);
void init_buf_le(struct global_context* ctx);
void init_buf_consensus(struct global_context* ctx);
void destroy_ctx(struct global_context* ctx, bool is_le);

void consensus_shutdown();
void emergency_shutdown(const char *reason);

#ifdef __cplusplus
}
#endif

#endif // RDMA_CONSENSUS_H