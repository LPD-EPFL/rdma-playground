#include <assert.h>
#include <sys/time.h>
#include <stdlib.h>
#include "consensus-protocol.h"
#include "leader-election.h"
#include "parser.h"
#include "rdma-consensus.h"
#include "registry.h"
#include "timers.h"

static int page_size;
static int sl = 1;
static pid_t pid;

int leader = 0;

extern struct global_context g_ctx;
extern struct global_context le_ctx;
// extern volatile bool stop_le;

void ibv_devinfo(void) {
  int num_devices = 0, dev_i;
  struct ibv_device** dev_list;
  struct ibv_context* ctx;
  struct ibv_device_attr device_attr;

  printf("HRD: printing IB dev info\n");

  dev_list = ibv_get_device_list(&num_devices);
  if(!dev_list) printf("Failed to get IB devices list");

  for (dev_i = 0; dev_i < num_devices; dev_i++) {
    ctx = ibv_open_device(dev_list[dev_i]);
    if (!ctx) printf("Couldn't get context");

    memset(&device_attr, 0, sizeof(device_attr));
    if (ibv_query_device(ctx, &device_attr)) {
      printf("Could not query device: %d\n", dev_i);
      assert(false);
    }

    printf("IB device %d:\n", dev_i);
    printf("    Name: %s\n", dev_list[dev_i]->name);
    printf("    Device name: %s\n", dev_list[dev_i]->dev_name);
    printf("    GUID: %016llx\n",
           (unsigned long long)ibv_get_device_guid(dev_list[dev_i]));
    printf("    Node type: %d (-1: UNKNOWN, 1: CA, 4: RNIC)\n",
           dev_list[dev_i]->node_type);
    printf("    Transport type: %d (-1: UNKNOWN, 0: IB, 1: IWARP)\n",
           dev_list[dev_i]->transport_type);

    printf("    fw: %s\n", device_attr.fw_ver);
    printf("    max_qp: %d\n", device_attr.max_qp);
    printf("    max_cq: %d\n", device_attr.max_cq);
    printf("    max_mr: %d\n", device_attr.max_mr);
    printf("    max_pd: %d\n", device_attr.max_pd);
    printf("    max_ah: %d\n", device_attr.max_ah);
    printf("    phys_port_cnt: %hu\n", device_attr.phys_port_cnt);
  }
}


void consensus_setup(follower_cb_t follower_cb, void *follower_cb_data) {
    set_cpu(MAIN_THREAD_CPU);
    printf("Setup\n");
    ibv_devinfo();
    pid = getpid();

    g_ctx = create_ctx();
    g_ctx.follower_cb_data = follower_cb_data;
    g_ctx.follower_cb = follower_cb;

    assert(pid);
    assert(g_ctx.len == (uint64_t)0);
    assert(sl);

    printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
           pid, IP_PORT, IB_PORT, g_ctx.len, MAX_SEND_WR, sl);

    // Is later needed to create random number for psn
    srand48(pid * time(NULL));

    page_size = sysconf(_SC_PAGESIZE);

    // Parse the configuration
    const char *filename = toml_getenv("CONFIG");
    toml_table_t *conf = toml_load_conf(filename);

    // Prepare memcached server
    char *host;
    int64_t port;
    toml_parse_registry(conf, &host, &port);
    g_ctx.registry = dory_registry_create(host, port);

    // Prepare global context
    int64_t clients, id;
    toml_parse_general(conf, &clients, &id);
    g_ctx.num_clients = clients - 1;
    g_ctx.my_index = id;

    // End of parsing. Pointer data are now owned by the registry and
    // the global context.
    toml_free(conf);

    init_ctx_common(&g_ctx, false);  // false = consensus thread

    set_local_ib_connection(&g_ctx, false);  // false = consensus thread
    exchange_ib_connection_info(&g_ctx, "consensus");

    // Print IB-connection details
    printf("Consensus thread connections:\n");
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        print_ib_connection("Local  Connection",
                            &g_ctx.qps[i].rc_local_connection);
        print_ib_connection("Remote Connection",
                            &g_ctx.qps[i].rc_remote_connection);
    }

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        qp_change_state_rts(&g_ctx.qps[i]);
    }
    printf("Main thread QPs changed to RTS mode\n");
}

void consensus_start_leader_election() { start_leader_election(); }

void consensus_stop_leader_election() {
    stop_leader_election();
    shutdown_leader_election_thread();
}

bool consensus_propose(uint8_t *buf, size_t len) { return propose(buf, len); }

void consensus_propose_test1() {
    uint64_t val;

    start_leader_election();

    if (g_ctx.my_index == 0) {
        val = 42;
        propose((uint8_t *)&val, sizeof(val));
        val = 43;
        propose((uint8_t *)&val, sizeof(val));
        val = 44;
        propose((uint8_t *)&val, sizeof(val));
        sleep(1);
    } else {
        sleep(1);
        log_print(g_ctx.buf.log);
    }

    stop_leader_election();
    shutdown_leader_election_thread();
}

void consensus_propose_noop() {
    start_leader_election();

    while (true) {
        sleep(600);
    }

    stop_leader_election();
    shutdown_leader_election_thread();
}

void consensus_propose_test2() {
    TIMESTAMP_INIT
    uint64_t val;

    start_leader_election();

    if (g_ctx.my_index == 0) {
        propose((uint8_t *)&val, sizeof(val));

        TIMESTAMP_T start, end;
        GET_TIMESTAMP(start);
        val = 42;
        for (int i = 0; i < TEST_SIZE; ++i) {
            propose((uint8_t *)&val, sizeof(val));
        }
        GET_TIMESTAMP(end);
        uint64_t duration = ELAPSED_NSEC(start, end);

        double avg_latency = (1.0 * duration) / TEST_SIZE;
        printf("Average latency = %.2f\n", avg_latency);
    } else {
        sleep(5);
        log_print(g_ctx.buf.log);
    }

    stop_leader_election();
    shutdown_leader_election_thread();
}

void consensus_propose_test3() {
    TIMESTAMP_INIT
    struct ibv_wc wc_array[g_ctx.num_clients];
    TIMESTAMP_T start, end;

    if (g_ctx.my_index == 0) {
        bool signaled;
        signaled = true;

        GET_TIMESTAMP(start);
        for (int i = 0; i < TEST_SIZE; i++) {
            if (g_ctx.round_nb % 64 == 0) {
                signaled = true;
            } else {
                signaled = true;
            }
            rdma_write_to_all(g_ctx.buf.log, 0, SLOT, signaled);
            if (signaled) {
                wait_for_n(1, g_ctx.round_nb, &g_ctx, g_ctx.num_clients,
                           wc_array, g_ctx.completed_ops);
            }
        }
        GET_TIMESTAMP(end);
        uint64_t duration = ELAPSED_NSEC(start, end);
        double avg_latency = (1.0 * duration) / TEST_SIZE;
        printf("Average latency = %.2f\n", avg_latency);
    } else {
        sleep(5);
    }

    // stop_leader_election();
    // shutdown_leader_election_thread();
}

int cmp_func(const void *a, const void *b) {
    return (int) ( *(uint64_t*)a - *(uint64_t*)b ); // ascending
}

void consensus_propose_leader_median() {
    TIMESTAMP_INIT

    uint8_t *data = malloc(1024 * sizeof(*data));
    assert(data);

    TIMESTAMP_T *timestamps = malloc((MEDIAN_SAMPLE_SIZE+1) * sizeof(*timestamps));
    assert(timestamps);

    uint64_t *elapsed_times = malloc(MEDIAN_SAMPLE_SIZE * sizeof(*elapsed_times));
    assert(elapsed_times);

    uint64_t *elapsed_times_ordered = malloc(MEDIAN_SAMPLE_SIZE * sizeof(*elapsed_times_ordered));
    assert(elapsed_times_ordered);


    printf("Sample size = %d\n", MEDIAN_SAMPLE_SIZE);

    start_leader_election();

    if (g_ctx.my_index == 0) {
        // Warm-up
        propose(data, 8);


        for (int sz = 128; sz < 1024; sz += 128) {
            for (int i = 0; i < MEDIAN_SAMPLE_SIZE; ++i) {
                GET_TIMESTAMP(timestamps[i]);
                propose(data, sz);
                // printf("Proposed %d\n", i);
            }
            GET_TIMESTAMP(timestamps[MEDIAN_SAMPLE_SIZE]);

            // post-processing

            for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
                elapsed_times[i] = ELAPSED_NSEC(timestamps[i], timestamps[i+1]);
                elapsed_times_ordered[i] = elapsed_times[i];

            }
            qsort(elapsed_times_ordered, MEDIAN_SAMPLE_SIZE, sizeof(uint64_t), cmp_func);

            uint64_t highest[3] = {elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-3],
                                elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-2],
                                elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-1]};
            uint64_t moments[3];
            for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
                for (int j = 0; j < 3; j++) {
                    if (elapsed_times[i] == highest[j]) {
                        moments[j] = i;
                    }
                }
            }

            double average = 0;
            for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
                average += (double) elapsed_times[i]/MEDIAN_SAMPLE_SIZE;
            }
            double median = elapsed_times_ordered[(int)(0.5 * MEDIAN_SAMPLE_SIZE)];
            double percentile_98 = elapsed_times_ordered[(int)(0.98 * MEDIAN_SAMPLE_SIZE)];
            double percentile_02 = elapsed_times_ordered[(int)(0.02 * MEDIAN_SAMPLE_SIZE)];

            printf("Sample size = %d\n", MEDIAN_SAMPLE_SIZE);
            printf("Average: %.2f\n", average);
            printf("Min = %lu ns\n", elapsed_times_ordered[0]);
            printf("02th percentile = %.2f ns\n", percentile_02);
            printf("Median = %.2f ns\n", median);
            printf("98th percentile = %.2f ns\n", percentile_98);
            printf("TOP 3 = %luth proposal - %lu ns, %luth proposal - %lu ns, %luth proposal - %lu ns\n",
                    moments[0]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-3],
                    moments[1]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-2],
                    moments[2]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-1]);
            printf("\n");
        }

    } else {
        sleep(60);
        log_print(g_ctx.buf.log);
    }

    stop_leader_election();
    shutdown_leader_election_thread();

    free(data);
    free(timestamps);
    free(elapsed_times);
    free(elapsed_times_ordered);
}

void consensus_propose_test_herd() {
    // WARNING: Do not forget to increase the MAX_INLINE_DATA constant for
    // best performance.
    start_leader_election();

    if (g_ctx.my_index == 0) {

        // Warm-up
        uint64_t val = 0xdeadbeef;
        propose((uint8_t *)&val, sizeof(val));
        sleep(5);

        struct timeval start, end;
        gettimeofday(&start, NULL);
        uint8_t get[17];
        uint8_t set[51];
        for (int i = 0; i < TEST_SIZE; ++i) {
            if (i % 2 == 0) {
                propose(get, 17);
            } else {
                propose(set, 51);
            }
        }
        gettimeofday(&end, NULL);
        uint64_t duration = (end.tv_sec * 1000000 + end.tv_usec) -
                            (start.tv_sec * 1000000 + start.tv_usec);
        double avg_latency = (1.0 * duration) / TEST_SIZE;
        printf("Average latency = %.2f\n", avg_latency);
    } else {
        sleep(30);
        log_print(g_ctx.buf.log);
    }

    while (true) {
        sleep(600);
    }

    stop_leader_election();
    shutdown_leader_election_thread();
}
