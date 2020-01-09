// Based on rdma_bw.c program

#include "rdma-consensus.h"
#include <stdatomic.h>
#include <sys/socket.h>

extern struct global_context g_ctx;
extern struct global_context le_ctx;

void init_buf_le(struct global_context *ctx) {
    // initializing the main buf in global_context
    ctx->buf.le_data =
        le_data_new(ctx->num_clients +
                    1);  // +1 because num_clients is the number of processes -1
    ctx->len = le_data_size(ctx->buf.le_data);

    // initializing the buf_copy's in each qp_context
    for (int i = 0; i < ctx->num_clients; i++) {
        ctx->qps[i].buf_copy.counter = (counter_t *)malloc(sizeof(counter_t));
        memset(ctx->qps[i].buf_copy.counter, 0, sizeof(counter_t));
    }
}

 struct __attribute__((__packed__)) log_entry {
    uint64_t accProposal;
    uint32_t firstUndecidedOffset;
    uint32_t len;
};

void *threadFunc(void *arg) {

    printf("threadFunc id : %d\n",(int)getpid());
    struct global_context *ctx = (struct global_context *) arg;

    volatile uint8_t *log_slab = (volatile uint8_t *)(ctx->buf.log->slots);
    uint64_t last_committed_offset = 0, offset = 0;

    while (true) {
        volatile struct log_entry *header = (volatile struct log_entry *)(log_slab + offset);

        while (header->len == 0);

        while (last_committed_offset < header->firstUndecidedOffset) {
            struct log_entry *commit_header = (struct log_entry *)(log_slab + last_committed_offset);

            ctx->follower_cb(ctx->follower_cb_data, (char *)commit_header + sizeof(*commit_header), commit_header->len - 1);

            last_committed_offset +=
                (
                    (uint64_t)
                        (
                            sizeof(*commit_header) +
                            commit_header->len + 1
                        ) +
                        63
                )
                & (~63);

        }

        offset += (
                    (uint64_t)
                        (
                            sizeof(*header) +
                            header->len + 1
                        ) +
                        63
                )
                & (~63);

    }

    return NULL;
}

void init_buf_consensus(struct global_context *ctx) {
    g_ctx.buf.log = log_new();
    g_ctx.len = log_size(g_ctx.buf.log);
    for (int i = 0; i < ctx->num_clients; i++) {
        ctx->qps[i].buf_copy.log = log_new();
    }

    if (ctx->my_index != 0) {
        printf("Spawning the pump (detached thread)\n");
        // Added for testing purposes
        pthread_t threadId;

        // Create a thread that will funtion threadFunc()
        int err = pthread_create(&threadId, NULL, &threadFunc, (void *)&g_ctx);
        // Check if thread is created sucessfuly
        if (err) {
            emergency_shutdown("Could not create a detached thread");
        }

        err = pthread_detach(threadId);
        // Check if thread is created sucessfuly
        if (err) {
            emergency_shutdown("Could not create a detached thread");
        }
    }
}

/*
 *     init_ctx
 * **********
 *    This method initializes the Infiniband Context
 *     It creates structures for: ProtectionDomain, MemoryRegion,
 * CompletionChannel, Completion Queues, Queue Pair
 */
void init_ctx_common(struct global_context *ctx, bool is_le) {
    // TEST_NZ(posix_memalign(&g_ctx.buf, page_size, g_ctx.size * 2),
    // "could not allocate working buffer g_ctx.buf");
    void *write_buf;
    void *read_buf;

    ctx->qps = (struct qp_context *)malloc(ctx->num_clients *
                                           sizeof(struct qp_context));
    memset(ctx->qps, 0, ctx->num_clients * sizeof(struct qp_context));

    if (is_le) {
        init_buf_le(ctx);
    } else {
        init_buf_consensus(ctx);
    }

    ctx->completed_ops =
        (uint64_t *)malloc(ctx->num_clients * sizeof(uint64_t));
    memset(ctx->completed_ops, 0, ctx->num_clients * sizeof(uint64_t));

    if (ctx->ib_dev ==
        NULL) {  // we only do this once & share this stuff among all contexts
        struct ibv_device **dev_list;

	int num_dev = 0;
        TEST_Z(dev_list = ibv_get_device_list(&num_dev),
               "No IB-device available. get_device_list returned NULL");

        TEST_Z(
            ctx->ib_dev = dev_list[num_dev - 1],
            "IB-device could not be assigned. Maybe dev_list array is empty");

        TEST_Z(ctx->context = ibv_open_device(ctx->ib_dev),
               "Could not create context, ibv_open_device");

        TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context),
               "Could not allocate protection domain, ibv_alloc_pd");
    }

    TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context),
           "Could not create completion channel, ibv_create_comp_channel");

    TEST_Z(ctx->cq = ibv_create_cq(ctx->context, MAX_SEND_WR, NULL, ctx->ch,
                                   COMP_VECTOR),
           "Could not create completion queue, ibv_create_cq");

    if (!is_le) {
        ctx->cur_write_permission =
            0;  // initially only process 0 has write accesss
    }

    for (int i = 0; i < ctx->num_clients; i++) {
        if (is_le) {
            write_buf = (void *)ctx->buf.le_data;
            read_buf = (void *)ctx->qps[i].buf_copy.counter;
            // create the MR that we write from and others write into
            TEST_Z(ctx->qps[i].mr_write = ibv_reg_mr(
                       ctx->pd, write_buf, ctx->len,
                       IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_LOCAL_WRITE),
                   "Could not allocate mr_write, ibv_reg_mr. Do you have root "
                   "access?");
        } else {
            write_buf = (void *)ctx->buf.log;
            read_buf = (void *)ctx->qps[i].buf_copy.log;

            // give read-write access to 0 and read-only access to everybody
            // else (initially)
            int flags = (i == 0)
                            ? (IBV_ACCESS_REMOTE_READ |
                               IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE)
                            : (IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE);
            // create the MR that we write from and others write into
            TEST_Z(ctx->qps[i].mr_write =
                       ibv_reg_mr(ctx->pd, write_buf, ctx->len, flags),
                   "Could not allocate mr_write, ibv_reg_mr. Do you have root "
                   "access?");
        }

        // create the MR that we read into
        TEST_Z(
            ctx->qps[i].mr_read =
                ibv_reg_mr(ctx->pd, read_buf, ctx->len, IBV_ACCESS_LOCAL_WRITE),
            "Could not allocate mr_read, ibv_reg_mr. Do you have root access?");

        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.send_cq = ctx->cq;
        qp_init_attr.recv_cq = ctx->cq;
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.cap.max_send_wr = MAX_SEND_WR;
        qp_init_attr.cap.max_recv_wr = MAX_RECV_WR;
        qp_init_attr.cap.max_send_sge = MAX_SEND_SGE;
        qp_init_attr.cap.max_recv_sge = MAX_RECV_SGE;
        qp_init_attr.cap.max_inline_data = MAX_INLINE_DATA;

        TEST_Z(ctx->qps[i].rc_qp = ibv_create_qp(ctx->pd, &qp_init_attr),
               "Could not create queue pair, ibv_create_qp");

        qp_change_state_init(&ctx->qps[i]);

        if (!is_le) { // also initialize the UC QP
#ifdef CONSENSUS_UC
            memset(&qp_init_attr, 0, sizeof(qp_init_attr));
            qp_init_attr.send_cq = ctx->cq;
            qp_init_attr.recv_cq = ctx->cq;
            qp_init_attr.qp_type = IBV_QPT_UC;
            qp_init_attr.cap.max_send_wr = MAX_SEND_WR;
            qp_init_attr.cap.max_recv_wr = MAX_RECV_WR;
            qp_init_attr.cap.max_send_sge = MAX_SEND_SGE;
            qp_init_attr.cap.max_recv_sge = MAX_RECV_SGE;
            qp_init_attr.cap.max_inline_data = MAX_INLINE_DATA;

            TEST_Z(ctx->qps[i].uc_qp = ibv_create_qp(ctx->pd, &qp_init_attr),
                   "Could not create queue pair, ibv_create_qp");

            qp_change_state_init(&ctx->qps[i]);
#else
            ctx->qps[i].uc_qp = ctx->qps[i].rc_qp;   
#endif // CONSENSUS_UC
        }
    }
}

void destroy_ctx(struct global_context *ctx, bool is_le) {
    for (int i = 0; i < ctx->num_clients; i++) {
        rc_qp_destroy(ctx->qps[i].rc_qp, ctx->cq);
#ifdef CONSENSUS_UC        
        rc_qp_destroy(ctx->qps[i].uc_qp, ctx->cq);
#endif // CONSENSUS_UC        
    }

    TEST_NZ(ibv_destroy_cq(ctx->cq),
            "Could not destroy completion queue, ibv_destroy_cq");

    TEST_NZ(ibv_destroy_comp_channel(ctx->ch),
            "Could not destory completion channel, ibv_destroy_comp_channel");

    for (int i = 0; i < ctx->num_clients; ++i) {
        TEST_NZ(ibv_dereg_mr(ctx->qps[i].mr_write),
                "Could not de-register memory region, ibv_dereg_mr");
        TEST_NZ(ibv_dereg_mr(ctx->qps[i].mr_read),
                "Could not de-register memory region, ibv_dereg_mr");

        if (is_le) {
            free(ctx->qps[i].buf_copy.counter);
        } else {
            log_free(ctx->qps[i].buf_copy.log);
        }
    }

    if (!is_le) {  // only do this once because pd is shared
        TEST_NZ(ibv_dealloc_pd(ctx->pd),
                "Could not deallocate protection domain, ibv_dealloc_pd");
    }

    if (is_le) {
        le_data_free(ctx->buf.le_data);
    } else {
        log_free(ctx->buf.log);
    }
    free(ctx->completed_ops);
}

void consensus_shutdown() {
    printf("Destroying IB context\n");
    destroy_ctx(&g_ctx, false);
}

void emergency_shutdown(const char *reason) {
    stop_leader_election();
    shutdown_leader_election_thread();
    consensus_shutdown();
    die(reason);
}
