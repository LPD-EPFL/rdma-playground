#include <assert.h>
#include "rdma-consensus.h"
#include "consensus-protocol.h"
#include "leader-election.h"
#include "parser.h"
#include "registry.h"

static int page_size;
static int sl = 1;
static pid_t pid;

int leader = 0;

extern struct global_context g_ctx;
extern struct global_context le_ctx;
// extern volatile bool stop_le;

void consensus_setup() {
        printf("Setup\n");
        pid = getpid();

        g_ctx = create_ctx();

        assert(pid);
        assert(g_ctx.port);
        assert(g_ctx.ib_port);
        assert(g_ctx.len == (uint64_t)0);
        assert(g_ctx.tx_depth);
        assert(sl);

        printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
            pid, g_ctx.port, g_ctx.ib_port, g_ctx.len, g_ctx.tx_depth, sl);

        // Is later needed to create random number for psn
        srand48(pid * time(NULL));

        page_size = sysconf(_SC_PAGESIZE);

        // Parse the configuration
        const char * filename = toml_getenv("CONFIG");
        toml_table_t *conf = toml_load_conf(filename);

        // Prepare memcached server
        char* host;
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

        init_ctx_common(&g_ctx, false); // false = consensus thread

        set_local_ib_connection(&g_ctx, false); // false = consensus thread
        exchange_ib_connection_info(&g_ctx, "consensus");

        // Print IB-connection details
        printf("Consensus thread connections:\n");
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            print_ib_connection("Local  Connection", &g_ctx.qps[i].local_connection);
            print_ib_connection("Remote Connection", &g_ctx.qps[i].remote_connection);
        }

        for (int i = 0; i < g_ctx.num_clients; ++i) {
            qp_change_state_rts(&g_ctx.qps[i], g_ctx.ib_port);
        }
        printf("Main thread QPs changed to RTS mode\n");
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