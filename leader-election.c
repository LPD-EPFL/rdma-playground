#include "leader-election.h"

struct global_context le_ctx = {
    .ib_dev             = NULL,
    .context            = NULL,
    .pd                 = NULL,
    .cq                 = NULL,
    .ch                 = NULL,
    .qps                = NULL,
    .round_nb           = 0,
    .num_clients        = 0,
    .port               = 18515,
    .ib_port            = 1,
    .tx_depth           = 100,
    .servername         = NULL,
    .buf.counter        = NULL,
    .len                = 0,
    .completed_ops      = NULL
};

extern struct global_context g_ctx;
extern char* config_file;
extern atomic_int leader;

void
spawn_leader_election_thread() {
    pthread_t le_thread;
    
    TEST_NZ(pthread_create(&le_thread, NULL, leader_election, NULL), "Could not create leader election thread");
}

void*
leader_election(void* arg) {
    // create & initialize a le context
    le_ctx.ib_dev             = g_ctx.ib_dev;
    le_ctx.context            = g_ctx.context;
    le_ctx.num_clients        = g_ctx.num_clients;
    le_ctx.port               = g_ctx.port;
    le_ctx.ib_port            = g_ctx.ib_port;
    le_ctx.tx_depth           = g_ctx.tx_depth;
    le_ctx.servername         = g_ctx.servername;
    le_ctx.sockfd             = g_ctx.sockfd;


    init_ctx_common(&le_ctx, true); // true = leader election thread

    parse_config(config_file, &le_ctx);

    // we don't need a log structure in the leader election thread
    // for now, zero it out and interpret it as a counter
    // memset(le_ctx.buf.log,0,log

    // use the tcp connections to exchange qp info
    set_local_ib_connection(&le_ctx, true); // true = leader election thread


    TEST_NZ(tcp_exch_ib_connection_info(&le_ctx),
            "Could not exchange connection, tcp_exch_ib_connection");

    // Print IB-connection details
    printf("Leader election connections:\n");
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        print_ib_connection("Local  Connection", &le_ctx.qps[i].local_connection);
        print_ib_connection("Remote Connection", &le_ctx.qps[i].remote_connection);    
    }


    // bring the qps to the right states
    // for (int i = 0; i < le_ctx.num_clients; ++i) {
    //     qp_change_state_rts(le_ctx.qps[i].qp, i);
    // }


    qp_change_state_rts(&le_ctx);

    printf("Going to sleep le\n");
    sleep(5);
        
    // start the leader election loop
    int i=0;
    while (i++ < 10) {
        // increment a local counter
        le_ctx.buf.counter->count_cur++;
        // read (RDMA) counters of everyone*
        rdma_read_all_counters();
        // figure out who is leader
        // and communicate the leader to the main thread
        leader = decide_leader();

        // sleep
        nanosleep((const struct timespec[]){{0, LE_SLEEP_DURATION_NS}}, NULL);
    }

    destroy_ctx(&le_ctx, true);
    pthread_exit(NULL);   
}

void
rdma_read_all_counters() {

    void* local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;

    le_ctx.round_nb++;
    WRID_SET_SSN(wrid, le_ctx.round_nb);

    // shift the counters
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        counter_t* counters = le_ctx.qps[i].buf_copy.counter;
        counters->count_oldest = counters->count_old;
        counters->count_old = counters->count_cur;
    }

    for (int i = 0; i < le_ctx.num_clients; ++i) {

        local_address = le_ctx.qps[i].buf_copy.counter;
        req_size = sizeof(uint64_t);

        WRID_SET_CONN(wrid, i);
        remote_addr = le_ctx.qps[i].remote_connection.vaddr;
        post_send(le_ctx.qps[i].qp, local_address, req_size, le_ctx.qps[i].mr_read->lkey, le_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_READ, wrid, true);
    }

    nanosleep((const struct timespec[]){{0, SHORT_SLEEP_DURATION_NS}}, NULL);

    struct ibv_wc wc_array[le_ctx.num_clients];

    wait_for_n(1, le_ctx.round_nb, le_ctx.cq, le_ctx.num_clients, wc_array, le_ctx.completed_ops);

}

int
decide_leader() {
    for (int i = 0; i < le_ctx.my_index; ++i) {

        counter_t* counters = le_ctx.qps[i].buf_copy.counter;
        if (counters->count_old != counters->count_oldest) { // this node has moved
            // return i;
            printf("Node %d is my leader\n", i);
            return i;
        }
        
        // if there is no concurrent read of the counters in progress, look at the most recent read counter as well
        if (le_ctx.completed_ops[i] == le_ctx.round_nb) {
            if (counters->count_cur != counters->count_old) {
                printf("Node %d is my leader\n", i);
                return i;
            }
        }
    }

    // return myself (no smaller id incremented their counter)
    printf("I am the leader\n");
    return le_ctx.my_index;
}