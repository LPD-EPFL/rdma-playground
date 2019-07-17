#include "ibv_layer.h"

extern struct global_context g_ctx;
static int sl = 1;



/*
 *  set_local_ib_connection
 * *************************
 *  Sets all relevant attributes needed for an IB connection. Those are then sent to the peer via TCP
 *     Information needed to exchange data over IB are: 
 *      lid - Local Identifier, 16 bit address assigned to end node by subnet manager 
 *      qpn - Queue Pair Number, identifies qpn within channel adapter (HCA)
 *      psn - Packet Sequence Number, used to verify correct delivery sequence of packages (similar to ACK)
 *      rkey - Remote Key, together with 'vaddr' identifies and grants access to memory region
 *      vaddr - Virtual Address, memory address that peer can later write to
 */
void set_local_ib_connection(struct global_context* ctx, bool is_le){

    // First get local lid
    struct ibv_port_attr attr;
    TEST_NZ(ibv_query_port(ctx->context,ctx->ib_port,&attr),
        "Could not get port attributes, ibv_query_port");

    for (int i = 0; i < ctx->num_clients; ++i) {
        ctx->qps[i].local_connection.qpn = ctx->qps[i].qp->qp_num;
        ctx->qps[i].local_connection.rkey = ctx->qps[i].mr_write->rkey;
        ctx->qps[i].local_connection.lid = attr.lid;
        ctx->qps[i].local_connection.psn = lrand48() & 0xffffff;
        if (is_le) {
            ctx->qps[i].local_connection.vaddr = (uintptr_t)ctx->buf.counter;
        } else {
            ctx->qps[i].local_connection.vaddr = (uintptr_t)ctx->buf.log;
        }
    }

}

void print_ib_connection(char *conn_name, struct ib_connection *conn){
    
    printf("%s: LID %#04x, QPN %#06x, PSN %#06x RKey %#08x VAddr %#016Lx\n", 
            conn_name, conn->lid, conn->qpn, conn->psn, conn->rkey, conn->vaddr);

}

int tcp_exch_ib_connection_info(struct global_context* ctx){

    char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
    int parsed;

    struct ib_connection *local;
    
    for (int i = 0; i < ctx->num_clients; ++i) {
        local = &ctx->qps[i].local_connection; 
        sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", 
                local->lid, local->qpn, local->psn, local->rkey, local->vaddr);
        if(write(ctx->sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not send connection_details to peer");
            return -1;
        }    

        if(read(ctx->sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not receive connection_details to peer");
            return -1;
        }
        struct ib_connection *remote = &ctx->qps[i].remote_connection;
        parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", 
                            &remote->lid, &remote->qpn, &remote->psn, &remote->rkey, &remote->vaddr);
        
        if(parsed != 5){
            fprintf(stderr, "Could not parse message from peer");
        }
    }

    return 0;
}

/**
 * Move a QP to the RESET state 
 */
int
qp_change_state_reset( struct qp_context *qpc )
{
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RESET; 

    TEST_NZ(ibv_modify_qp(qpc->qp, &attr, IBV_QP_STATE),
                "Could not modify QP to RESET, ibv_modify_qp");
    
    return 0;
}


/*
 *  qp_change_state_init
 * **********************
 *    Changes Queue Pair status to INIT
 */
int qp_change_state_init(struct qp_context *qpc, int ib_port){
    
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state            = IBV_QPS_INIT;
    attr.pkey_index          = 0;
    attr.port_num            = ib_port;
    attr.qp_access_flags     = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    TEST_NZ(ibv_modify_qp(qpc->qp, &attr,
                            IBV_QP_STATE        |
                            IBV_QP_PKEY_INDEX   |
                            IBV_QP_PORT         |
                            IBV_QP_ACCESS_FLAGS),
            "Could not modify QP to INIT, ibv_modify_qp");

    return 0;
}

/*
 *  qp_change_state_rtr
 * **********************
 *  Changes Queue Pair status to RTR (Ready to receive)
 */
int qp_change_state_rtr(struct qp_context *qpc, int ib_port){
    
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_2048;
    attr.dest_qp_num           = qpc->remote_connection.qpn;
    attr.rq_psn                = qpc->remote_connection.psn;
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12;
    attr.ah_attr.is_global     = 0;
    attr.ah_attr.dlid          = qpc->remote_connection.lid;
    attr.ah_attr.sl            = sl;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = ib_port;

    TEST_NZ(ibv_modify_qp(qpc->qp, &attr,
                IBV_QP_STATE                |
                IBV_QP_AV                   |
                IBV_QP_PATH_MTU             |
                IBV_QP_DEST_QPN             |
                IBV_QP_RQ_PSN               |
                IBV_QP_MAX_DEST_RD_ATOMIC   |
                IBV_QP_MIN_RNR_TIMER),
        "Could not modify QP to RTR state");
    
    return 0;
}


/*
 *  qp_change_state_rts
 * **********************
 *  Changes Queue Pair status to RTS (Ready to send)
 *    QP status has to be RTR before changing it to RTS
 */
int qp_change_state_rts(struct qp_context *qpc, int ib_port){

    qp_change_state_rtr(qpc, ib_port); 
    
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof attr);

    attr.qp_state              = IBV_QPS_RTS;
    attr.timeout               = 14;
    attr.retry_cnt             = 7;
    attr.rnr_retry             = 7;    /* infinite retry */
    attr.sq_psn                = qpc->local_connection.psn;
    attr.max_rd_atomic         = 1;

    TEST_NZ(ibv_modify_qp(qpc->qp, &attr,
                IBV_QP_STATE            |
                IBV_QP_TIMEOUT          |
                IBV_QP_RETRY_CNT        |
                IBV_QP_RNR_RETRY        |
                IBV_QP_SQ_PSN           |
                IBV_QP_MAX_QP_RD_ATOMIC),
        "Could not modify QP to RTS state");
    

    return 0;
}


/** 
 * Restarts a certain QP: *->RESET->INIT->RTR->RTS
 * used only in case of ERROR
 */ 
int 
qp_restart( struct qp_context *qpc, int ib_port) {    

    TEST_NZ(qp_change_state_reset(qpc),
        "Cannot move QP to reset state\n"); 

    TEST_NZ(qp_change_state_init(qpc, ib_port),
        "Cannot move QP to init state\n");

    TEST_NZ(qp_change_state_rts(qpc, ib_port),
        "Cannot move QP to RTS state\n");
    
    return 0;
}

void 
rc_qp_destroy( struct ibv_qp *qp, struct ibv_cq *cq ) {
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    struct ibv_wc wc;

    if (NULL == qp) return;
       
    ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr);
    if (attr.qp_state != IBV_QPS_RESET) {
        /* Move QP into the ERR state to cancel all outstanding WR */
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_ERR;
        TEST_NZ(ibv_modify_qp(qp, &attr, IBV_QP_STATE), "could not move qp to error state");

        /* Empty the corresponding CQ */
        while (ibv_poll_cq(cq, 1, &wc) > 0);// info(log_fp, "while...\n");
    }
   
    TEST_NZ(ibv_destroy_qp(qp), "could not destroy qp");
    
}

/*
 *  rdma_write
 * **********************
 *    Writes 'ctx-buf' into buffer of peer
 */
void rdma_write(int id){

    post_send(g_ctx.qps[id].qp, g_ctx.buf.log, log_size(g_ctx.buf.log), g_ctx.qps[id].mr_write->lkey, g_ctx.qps[id].remote_connection.rkey, g_ctx.qps[id].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 42, true);

}    


/*
 *  rdma_read
 * **********************
 */
// void rdma_read(int id){

//     post_send(g_ctx.qps[id].qp, g_ctx.buf, g_ctx.size, g_ctx._read->lkey, g_ctx.qps[id].remote_connection.rkey, g_ctx.qps[id].remote_connection.vaddr, IBV_WR_RDMA_READ, 42);

// }

int permission_switch(struct ibv_mr* old_mr, struct ibv_mr* new_mr, struct ibv_pd* pd, void* addr, size_t length, int old_new_flags, int new_new_flags) {

    // take away access from old mr
    TEST_NZ(ibv_rereg_mr(old_mr, // the memory region
        IBV_REREG_MR_CHANGE_ACCESS, // we want to change the access flags
        pd, // the protection domain
        addr, length, 
        old_new_flags),
        "ibv_rereg_mr: failed to take away permission");

    // give access to new mr
    TEST_NZ(ibv_rereg_mr(new_mr, // the memory region
        IBV_REREG_MR_CHANGE_ACCESS, // we want to change the access flags
        pd, // the protection domain
        addr, length, 
        new_new_flags),
        "ibv_rereg_mr: failed to give permission");

    return 0;
}
