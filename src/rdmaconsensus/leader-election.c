#include "leader-election.h"
#include "barrier.h"
#include "utils.h"

struct global_context le_ctx;
volatile bool stop_le;

extern struct global_context g_ctx;
extern char *config_file;
extern int leader;

// barriers to synchronize with the leader election thread
// entry_barrier syncs with the beginning of the leader election loop
// exit_barrier syncs with the exit from the leader election thread
barrier_t entry_barrier, exit_barrier;

#define TIMED_LOOP(duration)       \
    {                              \
        clock_t __begin = clock(); \
        while (((double)(clock() - __begin) / CLOCKS_PER_SEC) < (duration)) {
#define TIMED_LOOP_END() \
    }                    \
    }

void spawn_leader_election_thread() {
    pthread_t le_thread;

    TEST_NZ(pthread_create(&le_thread, NULL, leader_election, NULL),
            "Could not create leader election thread");
}

void *check_permission_loop(void *arg) {
    UNUSED(arg);

    set_cpu(PERM_THREAD_CPU);

    while (1) {
        check_permission_requests();
    }

    // Return value from thread
    return NULL;
}

void start_permission_checking() {
    // Added for testing purposes
    pthread_t threadId;

    // Create a thread that will funtion threadFunc()
    int err = pthread_create(&threadId, NULL, &check_permission_loop, NULL);
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

void *leader_election(void *arg) {
    UNUSED(arg);

    set_cpu(LE_THREAD_CPU);

    le_ctx = create_ctx();
    // create & initialize a le context
    le_ctx.my_index = g_ctx.my_index;
    le_ctx.ib_dev = g_ctx.ib_dev;
    le_ctx.context = g_ctx.context;
    le_ctx.pd = g_ctx.pd;
    le_ctx.num_clients = g_ctx.num_clients;
    le_ctx.servername = g_ctx.servername;
    le_ctx.registry = dory_registry_create_from(g_ctx.registry);

    init_ctx_common(&le_ctx, true);  // true = leader election thread

    // parse_config(config_file, &le_ctx);

    // we don't need a log structure in the leader election thread
    // for now, zero it out and interpret it as a counter
    // memset(le_ctx.buf.log,0,log

    // use the tcp connections to exchange qp info
    set_local_ib_connection(&le_ctx, true);  // true = leader election thread

    exchange_ib_connection_info(&le_ctx, "leader-election");

    // Print IB-connection details
    printf("Leader election connections:\n");
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        print_ib_connection("Local  Connection",
                            &le_ctx.qps[i].rc_local_connection);
        print_ib_connection("Remote Connection",
                            &le_ctx.qps[i].rc_remote_connection);
    }

    // bring the qps to the right states
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        qp_change_state_rts(&le_ctx.qps[i]);
    }

    barrier_cross(&entry_barrier);

    start_permission_checking();

    // start the leader election loop
    while (!stop_le) {
        // increment a local counter
        le_ctx.buf.le_data->counters.count_cur++;
        // read (RDMA) counters of everyone*
        rdma_read_all_counters();
        // figure out who is leader
        // and communicate the leader to the main thread
        leader = decide_leader();

        // while LE_SLEEP_DURATION_NS has not elapsed:
        // check and grant permission requests
        // TIMED_LOOP(LE_COUNTER_READ_PERIOD_SEC)
        // check_permission_requests();
        // TIMED_LOOP_END()
        // clock_t begin = clock();
        // while (((double)(clock() - begin) / CLOCKS_PER_SEC) <
        // LE_COUNTER_READ_PERIOD_SEC) {
        //     check_permission_requests();
        // }

        // sleep
        nanosleep((const struct timespec[]){{0, LE_SLEEP_DURATION_NS}}, NULL);
    }

    destroy_ctx(&le_ctx, true);

    barrier_cross(&exit_barrier);
    pthread_exit(NULL);
}

void rdma_read_all_counters() {
    void *local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;

    le_ctx.round_nb++;
    WRID_SET_SSN(wrid, le_ctx.round_nb);

    // shift the counters
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        counter_t *counters = le_ctx.qps[i].buf_copy.counter;
        counters->count_oldest = counters->count_old;
        counters->count_old = counters->count_cur;
    }

    for (int i = 0; i < le_ctx.num_clients; ++i) {
        // we are reading just the first counter (count_cur)
        local_address = le_ctx.qps[i].buf_copy.counter;
        req_size = sizeof(uint64_t);

        WRID_SET_CONN(wrid, i);
        remote_addr =
            le_ctx.qps[i]
                .rc_remote_connection.vaddr;  // remote offset = 0; we are reading
                                           // the first word = count_cur
        post_send(le_ctx.qps[i].rc_qp, local_address, req_size,
                  le_ctx.qps[i].mr_read->lkey,
                  le_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_READ, wrid, true);
    }

    nanosleep((const struct timespec[]){{0, SHORT_SLEEP_DURATION_NS}}, NULL);

    struct ibv_wc wc_array[le_ctx.num_clients];

    wait_for_n(1, le_ctx.round_nb, &le_ctx, le_ctx.num_clients, wc_array,
               le_ctx.completed_ops);
}

int decide_leader() {
    for (int i = 0; i < le_ctx.my_index; ++i) {
        counter_t *counters = le_ctx.qps[i].buf_copy.counter;
        if (counters->count_old !=
            counters->count_oldest) {  // this node has moved
            // return i;
            // printf("Node %d is my leader\n", i);
            return i;
        }

        // if there is no concurrent read of the counters in progress, look at
        // the most recent read counter as well
        if (le_ctx.completed_ops[i] == le_ctx.round_nb) {
            if (counters->count_cur != counters->count_old) {
                // printf("Node %d is my leader\n", i);
                return i;
            }
        }
    }

    // return myself (no smaller id incremented their counter)
    // printf("I am the leader\n");
    return le_ctx.my_index;
}

// wait for permission acks
// n = how many acks to wait for
static void wait_for_perm_ack(int n) {
    int acks = 0;
    uint64_t len = le_ctx.buf.le_data->len;

    while (acks < n) {
        for (uint64_t i = 0; i < len; ++i) {
            if (le_ctx.buf.le_data->perm_reqs_acks[i].ack != 0) {
                //printf("Got ack from %lu: %lu\n", i, le_ctx.buf.le_data->perm_reqs_acks[i].ack);
                g_ctx.qps[i].rc_remote_connection.rkey = le_ctx.buf.le_data->perm_reqs_acks[i].ack;
                g_ctx.qps[i].uc_remote_connection.rkey = g_ctx.qps[i].rc_remote_connection.rkey;
                acks += 1;
                le_ctx.buf.le_data->perm_reqs_acks[i].ack = 0;
            }
        }
    }
}

// TODO: we want this to also revoke the access of my current leader to my
// memory
void rdma_ask_permission(le_data_t *le_data, uint64_t my_index, bool signaled) {
    printf("Asking for permission\n");

    void *local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;

    le_data->perm_reqs_acks[my_index].req =
        1;  // I want to get permission for myself

    local_address = &le_data->perm_reqs_acks[my_index].req;
    req_size = sizeof(uint8_t);

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);
    for (int i = 0; i < le_ctx.num_clients; ++i) {
        WRID_SET_CONN(wrid, i);
        remote_addr = le_data_get_remote_address(
            le_data, local_address,
            ((le_data_t *)le_ctx.qps[i].rc_remote_connection.vaddr));
        post_send(g_ctx.qps[i].rc_qp, local_address, req_size,
                  le_ctx.qps[i].mr_write->lkey,
                  le_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_WRITE, wrid, signaled);
    }

    nanosleep((const struct timespec[]){{0, SHORT_SLEEP_DURATION_NS}}, NULL);

    struct ibv_wc wc_array[le_ctx.num_clients];

    // wait for at least one permission request to be successfully sent
    wait_for_n(1, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array,
               g_ctx.completed_ops);

    // wait for at least one permission ack to arrive
    wait_for_perm_ack(2);
}

static void send_perm_ack(int index) {

    void *local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;

    int my_index =
        (index < le_ctx.my_index)
            ? le_ctx.my_index - 1
            : le_ctx.my_index;  // my index in the other side's qps array
    le_ctx.buf.le_data->perm_reqs_acks[my_index].ack = g_ctx.qps[index].mr_write->rkey;
    local_address = &le_ctx.buf.le_data->perm_reqs_acks[my_index].ack;

    //printf("Sending ack to %d, my index in their array is %d, ack is %lu\n", index,
           //my_index, *(uint64_t*)local_address);

    req_size = sizeof(uint64_t);

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);
    WRID_SET_CONN(wrid, index);
    remote_addr = le_data_get_remote_address(
        le_ctx.buf.le_data, local_address,
        ((le_data_t *)le_ctx.qps[index].rc_remote_connection.vaddr));
    post_send(g_ctx.qps[index].rc_qp, local_address, req_size,
              le_ctx.qps[index].mr_write->lkey,
              le_ctx.qps[index].rc_remote_connection.rkey, remote_addr,
              IBV_WR_RDMA_WRITE, wrid, true);

    nanosleep((const struct timespec[]){{0, SHORT_SLEEP_DURATION_NS}}, NULL);

    struct ibv_wc wc_array[le_ctx.num_clients];

    // wait for at least one permission ack to be successfully sent
    wait_for_n(1, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array,
               g_ctx.completed_ops);
}

void check_permission_requests() {
    int j;
    // loop over local perm_reqs
    for (int i = 0; i < le_ctx.buf.le_data->len; ++i) {
        if (i == le_ctx.my_index) continue;

        if (le_ctx.buf.le_data->perm_reqs_acks[i].req ==
            1) {  // there is a request from i

            le_ctx.buf.le_data->perm_reqs_acks[i].req = 0;

            // change permission on g_ctx
            j = (i < le_ctx.my_index)
                    ? i
                    : i - 1;  // i indexes into perm_reqs (n entries total), j
                              // indexes into qps (n-1 entries total)
            //printf("Permission request from %d, my_index = %d, j = %d\n", i,
                   //le_ctx.my_index, j);
            //printf("rkeys before:%u, %u\n", g_ctx.qps[0].mr_write->rkey, g_ctx.qps[1].mr_write->rkey);
            // printf("vaddr before:%u, %u\n", g_ctx.qps[0].mr_write->vaddr, g_ctx.qps[1].mr_write->vaddr);
            permission_switch(
                g_ctx.qps[g_ctx.cur_write_permission]
                    .mr_write,          // mr losing permission
                g_ctx.qps[j].mr_write,  // mr gaining permission
                g_ctx.pd, g_ctx.buf.log, g_ctx.len,
                (IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE),
                (IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_LOCAL_WRITE));

            //printf("rkeys after:%u, %u\n", g_ctx.qps[0].mr_write->rkey, g_ctx.qps[1].mr_write->rkey);
            // printf("vaddr after:%u, %u\n", g_ctx.qps[0].mr_write->vaddr, g_ctx.qps[1].mr_write->vaddr);

            g_ctx.cur_write_permission = j;

            // send ack
            send_perm_ack(j);
        }
    }
}

void start_leader_election() {
    barrier_init(&entry_barrier, 2);
    barrier_init(&exit_barrier, 2);

    stop_le = false;
    spawn_leader_election_thread();

    barrier_cross(&entry_barrier);
}

void stop_leader_election() { stop_le = true; }

void shutdown_leader_election_thread() { barrier_cross(&exit_barrier); }
