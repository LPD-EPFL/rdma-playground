#ifndef RDMA_CONSENSUS_H
#define RDMA_CONSENSUS_H

#ifdef __cplusplus
extern "C" {
#endif

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */


#include "utils.h"
#include "ibv_layer.h"
#include "leader-election.h"


int die(const char *reason);

void tcp_client_connect();
void tcp_server_listen();
void count_lines(const char* filename, struct global_context *ctx);
void parse_config(const char* filename, struct global_context *ctx);
bool compare_to_self(struct ifaddrs *ifaddr, char *addr);

void init_ctx_common(struct global_context* ctx, bool is_le);
void init_buf_le(struct global_context* ctx);
void init_buf_consensus(struct global_context* ctx);
void destroy_ctx(struct global_context* ctx, bool is_le);

void consensus_shutdown();
void emergency_shutdown(const char *reason);

static int wait_for_n(  int n, 
                        uint64_t round_nb, 
                        struct global_context* ctx, 
                        int num_entries, 
                        struct ibv_wc *wc_array, 
                        uint64_t* completed_ops ) {

    int rc = wait_for_n_inner(n, round_nb, ctx, num_entries, wc_array, completed_ops);
    
    if (rc < 0) { // unexpected error, shutdown
        emergency_shutdown("Problem in wait_for_n");
    }

    return rc;
}

static void
post_send(  struct ibv_qp* qp,
            void* buf,
            uint32_t len,
            uint32_t lkey,
            uint32_t rkey,
            uint64_t remote_addr,
            enum ibv_wr_opcode opcode,
            uint64_t wrid,
            bool signaled   ) {
    
    int rc = post_send_inner(qp, buf, len, lkey, rkey, remote_addr, opcode, wrid, signaled);
    if (rc) { // there was a problem
        emergency_shutdown("Problem in post_send");
    }
}

#ifdef __cplusplus
}
#endif

#endif // RDMA_CONSENSUS_H