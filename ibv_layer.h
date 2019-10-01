#ifndef IBV_LAYER_H
#define IBV_LAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "utils.h"

void set_local_ib_connection(struct global_context* ctx, bool is_le);
void print_ib_connection(char *conn_name, struct ib_connection *conn);

int tcp_exch_ib_connection_info(struct global_context* ctx);

int qp_change_state_reset( struct qp_context *qpc );
int qp_change_state_init(struct qp_context *qpc, int ib_port);
int qp_change_state_rtr(struct qp_context *qpc, int ib_port);
int qp_change_state_rts(struct qp_context *qpc, int ib_port);
void rc_qp_destroy( struct ibv_qp *qp, struct ibv_cq *cq );
int qp_restart( struct qp_context *qpc, int ib_port);

void rdma_write(int id);
void rdma_read(int id);
int permission_switch(struct ibv_mr* old_mr, struct ibv_mr* new_mr, struct ibv_pd* pd, void* addr, size_t length, int old_new_flags, int new_new_flags);


/**
 * Handle the completion status of a WC 
 */
static int
handle_work_completion( struct ibv_wc *wc )
{
    int rc;
    // uint64_t wr_id = wc->wr_id;
    // uint8_t wr_idx = WRID_GET_CONN(wr_id);
    // dare_ib_ep_t *ep = (dare_ib_ep_t*)SRV_DATA->config.servers[wr_idx].ep;

    /* Verify completion status */
    switch(wc->status) {
        case IBV_WC_SUCCESS:
            /* IBV_WC_SUCCESS: Operation completed successfully */
            rc = WC_SUCCESS;
            break;
        case IBV_WC_REM_ACCESS_ERR: //  Remote Access Error
            rc = WC_EXPECTED_ERROR;
            fprintf(stderr, "Expected error: WC has status %s (%d) \n", ibv_wc_status_str(wc->status), wc->status);
            break;

        case IBV_WC_LOC_LEN_ERR:    //  Local Length Error
        case IBV_WC_LOC_QP_OP_ERR:  //  Local QP Operation Error
        case IBV_WC_LOC_EEC_OP_ERR: //  Local EE Context Operation Error
        case IBV_WC_LOC_PROT_ERR:   //  Local Protection Error   
        case IBV_WC_MW_BIND_ERR:    //  Memory Window Binding Error
        case IBV_WC_LOC_ACCESS_ERR: //  Local Access Error
        case IBV_WC_RNR_RETRY_EXC_ERR:  // RNR Retry Counter Exceeded
        case IBV_WC_LOC_RDD_VIOL_ERR:   // Local RDD Violation Error
        case IBV_WC_REM_INV_RD_REQ_ERR: // Remote Invalid RD Request
        case IBV_WC_REM_ABORT_ERR:  // Remote Aborted Error
        case IBV_WC_INV_EECN_ERR:   // Invalid EE Context Number
        case IBV_WC_INV_EEC_STATE_ERR:  // Invalid EE Context State Error
        case IBV_WC_WR_FLUSH_ERR:
            /* Work Request Flushed Error: A Work Request was in 
            process or outstanding when the QP transitioned into the 
            Error State. */
        case IBV_WC_BAD_RESP_ERR:
            /* Bad Response Error - an unexpected transport layer 
            opcode was returned by the responder. */
        case IBV_WC_REM_INV_REQ_ERR:
            /* Remote Invalid Request Error: The responder detected an 
            invalid message on the channel. Possible causes include the 
            operation is not supported by this receive queue, insufficient 
            buffering to receive a new RDMA or Atomic Operation request, 
            or the length specified in an RDMA request is greater than 
            2^{31} bytes. Relevant for RC QPs. */
        case IBV_WC_REM_OP_ERR:
            /* Remote Operation Error: the operation could not be 
            completed successfully by the responder. Possible causes 
            include a responder QP related error that prevented the 
            responder from completing the request or a malformed WQE on 
            the Receive Queue. Relevant for RC QPs. */
        case IBV_WC_RETRY_EXC_ERR:
            /* Transport Retry Counter Exceeded: The local transport 
            timeout retry counter was exceeded while trying to send this 
            message. This means that the remote side didn’t send any Ack 
            or Nack. If this happens when sending the first message, 
            usually this mean that the connection attributes are wrong or 
            the remote side isn’t in a state that it can respond to messages. 
            If this happens after sending the first message, usually it 
            means that the remote QP isn’t available anymore. */
            /* REMOTE SIDE IS DOWN */
        case IBV_WC_FATAL_ERR:
            /* Fatal Error - WTF */
        case IBV_WC_RESP_TIMEOUT_ERR:
            /* Response Timeout Error */
        case IBV_WC_GENERAL_ERR:
            /* General Error: other error which isn’t one of the above errors. */

            rc = WC_UNEXPECTED_ERROR;
            fprintf(stderr, "Unexpected error: WC has status %s (%d) \n", ibv_wc_status_str(wc->status), wc->status);
            break;

    }

    return rc;
}

// Waits until n send requests complete for round number round_nb
// Parameters:
// n = the number of work completions to wait for
// round_nb = the round number (SSN) that we expect to find inside the work completions (wr_id)
// cq = the completion queue to poll from
// num_entries = maximum number of entries to poll from cq
// wc_array = a pre-allocated array to store the polled work completions 
// Returns: 0 on success, 1 on non-fatal failure, -1 on fatal failure
static int wait_for_n_inner(  int n, 
                        uint64_t round_nb, 
                        struct global_context* ctx, 
                        int num_entries, 
                        struct ibv_wc *wc_array, 
                        uint64_t* completed_ops ) {
    int success_count = 0;
    int completion_count = 0;
    int ne = 0;
    int i;
    int ret;
    uint64_t wr_id;
    int cid;

    while (completion_count < n) {
        // poll
        ne = ibv_poll_cq(ctx->cq, num_entries, wc_array);

        TEST_N(ne, "Unable to poll from CQ");
        // check what was polled
        for (i = 0; i < ne; i++) {
            wr_id = wc_array[i].wr_id;

            if (WRID_GET_SSN(wr_id) == round_nb) {
                completion_count++;
            }

            ret = handle_work_completion(&wc_array[i]);
            if (ret == WC_SUCCESS) {
                if (WRID_GET_SSN(wr_id) == round_nb) {
                    success_count++;
                    cid = WRID_GET_CONN(wr_id);
                    completed_ops[cid] = round_nb;
                }

            } else if (ret == WC_EXPECTED_ERROR) {
                if (wc_array[i].opcode == IBV_WC_RDMA_READ) {
                    // didn't manage to read due to permissions -> problem
                    return -1;
                }
                cid = WRID_GET_CONN(wr_id);
                qp_restart(&ctx->qps[cid], ctx->ib_port);
            } else { // unexpected error
                return -1;
            }
        }
    }

    if (success_count >= n) {
        return 0; // success
    } else {
        return 1;
    }
}

static int wait_for_n(  int n, 
                        uint64_t round_nb, 
                        struct global_context* ctx, 
                        int num_entries, 
                        struct ibv_wc *wc_array, 
                        uint64_t* completed_ops ) {

    int rc = wait_for_n_inner(n, round_nb, ctx, num_entries, wc_array, completed_ops);
    
    if (rc < 0) { // unexpected error, shutdown
        // die
    }

    return rc;
}


static int
post_send_inner(  struct ibv_qp* qp,
            void* buf,
            uint32_t len,
            uint32_t lkey,
            uint32_t rkey,
            uint64_t remote_addr,
            enum ibv_wr_opcode opcode,
            uint64_t wrid,
            bool signaled   ) {

    struct ibv_sge sg;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr;

    memset(&sg, 0, sizeof(sg));
    sg.addr   = (uint64_t)buf;
    sg.length = len;
    sg.lkey   = lkey;    

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wrid;
    wr.sg_list    = &sg;
    wr.num_sge    = 1;
    wr.opcode     = opcode;
    if (signaled) {
        wr.send_flags = IBV_SEND_SIGNALED;
    }
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    int rc = ibv_post_send(qp, &wr, &bad_wr);

    switch (rc) {
        case EINVAL: 
            printf("EINVAL\n");
            break;
        case ENOMEM:
            printf("ENOMEM\n");
            break;
        case EFAULT:
            printf("EFAULT\n");
            break;
        default:
            break;
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
        // die
    }
}

#ifdef __cplusplus
}
#endif

#endif // IBV_LAYER_H