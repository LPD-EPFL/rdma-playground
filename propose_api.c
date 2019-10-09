#include "rdma-consensus.h"
#include "consensus-protocol.h"
#include "leader-election.h"
#include <assert.h>
#include <sys/time.h>

static int page_size;
static int sl = 1;
static pid_t pid;

const char* config_file = CONFIG_FILE_NAME;

int leader = 0;

extern struct global_context g_ctx;
extern struct global_context le_ctx;
// extern volatile bool stop_le;

bool isValidIpAddress(char *ipAddress) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress, &(sa.sin_addr));
    return result != 0;
}

void consensus_setup() {

        set_cpu(MAIN_THREAD_CPU);
        printf("Setup\n");
        pid = getpid();

        g_ctx = create_ctx();

        assert(pid);
        assert(g_ctx.len == (uint64_t)0);
        assert(sl);

        printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
            pid, IP_PORT, IB_PORT, g_ctx.len, MAX_SEND_WR, sl);

        // Is later needed to create random number for psn
        srand48(pid * time(NULL));

        page_size = sysconf(_SC_PAGESIZE);

        count_lines(config_file, &g_ctx);
        assert(g_ctx.num_clients > 0);

        init_ctx_common(&g_ctx, false); // false = consensus thread

        parse_config(config_file, &g_ctx);

        assert(isValidIpAddress(g_ctx.qps[0].ip_address));
        assert(isValidIpAddress(g_ctx.qps[1].ip_address));
        // printf("Current ip addresses before set local ib connection %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

        set_local_ib_connection(&g_ctx, false); // false = consensus thread

        g_ctx.sockfd = (int*)malloc(g_ctx.num_clients * sizeof(g_ctx.sockfd));

        tcp_server_listen();

        // TODO maybe sleep here
        tcp_client_connect();

        assert(tcp_exch_ib_connection_info(&g_ctx) == 0 && 
                "Could not exchange connection, tcp_exch_ib_connection");

        // Print IB-connection details
        printf("Consensus thread connections:\n");
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            print_ib_connection("Local  Connection", &g_ctx.qps[i].local_connection);
            print_ib_connection("Remote Connection", &g_ctx.qps[i].remote_connection);
        }

        for (int i = 0; i < g_ctx.num_clients; ++i) {
            qp_change_state_rts(&g_ctx.qps[i]);
        }
        printf("Main thred QPs changed to RTS mode\n");
}

void consensus_start_leader_election() {
	start_leader_election();
}

void consensus_stop_leader_election() {
	stop_leader_election();
	shutdown_leader_election_thread();
}

bool consensus_propose(uint8_t *buf, size_t len) {
	return propose(buf, len);
}

void consensus_propose_test1() {
    uint64_t val;

    start_leader_election();

    if (g_ctx.my_index == 0) {
        val = 42;
        propose((uint8_t*)&val, sizeof(val));
        val = 43;
        propose((uint8_t*)&val, sizeof(val));
        val = 44;
        propose((uint8_t*)&val, sizeof(val));
        sleep(1);
    } else {
        sleep(1);
        log_print(g_ctx.buf.log);
    }
    

    stop_leader_election();
    shutdown_leader_election_thread();
}

void consensus_propose_test2() {
    uint64_t val;

    start_leader_election();       

    if (g_ctx.my_index == 0) {
        propose((uint8_t*)&val, sizeof(val)); 
        
        struct timeval start, end;
        gettimeofday(&start, NULL);
        val = 42;
        for (int i = 0; i < TEST_SIZE; ++i) {
            propose((uint8_t*)&val, sizeof(val));        
        }
        gettimeofday(&end, NULL);
        uint64_t duration = (end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec );
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
    uint64_t val;
    struct ibv_wc wc_array[g_ctx.num_clients];
    struct timeval start, end;


    if (g_ctx.my_index == 0) {

        bool signaled;
        signaled = true;

        gettimeofday(&start, NULL);
        for (int i = 0; i < TEST_SIZE; i++) {
            if (g_ctx.round_nb % 64 == 0) {
                signaled = true;
            } else {
                signaled = true;
            }
            rdma_write_to_all(g_ctx.buf.log, 0, SLOT, signaled);
            if (signaled) {
                wait_for_n(1, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array, g_ctx.completed_ops);
            }
        }
        gettimeofday(&end, NULL);
        uint64_t duration = (end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec );
        double avg_latency = (1.0 * duration) / TEST_SIZE;
        printf("Average latency = %.2f\n", avg_latency);
    } else {
        sleep(5);
    }

    // stop_leader_election();
    // shutdown_leader_election_thread();
}

