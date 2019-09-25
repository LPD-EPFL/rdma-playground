#include "rdma-consensus.h"
#include "leader-election.h"
#include "barrier.h"

#include "gtest/gtest.h"
#include <string>

#define NB_ITERATIONS 10000

static int page_size;
static int sl = 1;
static pid_t pid;

char* config_file;

int leader = 0;

extern struct global_context g_ctx;
extern struct global_context le_ctx;
extern volatile bool stop_le;

// barriers to synchronize with the leader election thread
// entry_barrier syncs with the beginning of the leader election loop
// exit_barrier syncs with the exit from the leader election thread
barrier_t entry_barrier, exit_barrier;


namespace {

std::string error_details(std::string reason) {
    return ((std::string)"Err: ") + strerror(errno) + " " + reason;
}

bool isValidIpAddress(char *ipAddress) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipAddress, &(sa.sin_addr));
    return result != 0;
}

class Environment : public ::testing::Environment {
    public:
    virtual ~Environment() {}

    // Override this to define how to set up the environment.
    void SetUp() override {

        printf("Setup\n");
        pid = getpid();

        g_ctx = create_ctx();

        EXPECT_NE(pid, 0);
        EXPECT_NE(g_ctx.port, 0);
        EXPECT_NE(g_ctx.ib_port, 0);
        EXPECT_EQ(g_ctx.len, 0);
        EXPECT_NE(g_ctx.tx_depth, 0);
        EXPECT_NE(sl, 0);

        printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
            pid, g_ctx.port, g_ctx.ib_port, g_ctx.len, g_ctx.tx_depth, sl);

        // Is later needed to create random number for psn
        srand48(pid * time(NULL));
        
        page_size = sysconf(_SC_PAGESIZE);

        config_file = "./config";
        count_lines(config_file, &g_ctx);
        EXPECT_GT(g_ctx.num_clients, 0);
        
        init_ctx_common(&g_ctx, false); // false = consensus thread

        parse_config(config_file, &g_ctx);

        EXPECT_TRUE(isValidIpAddress(g_ctx.qps[0].ip_address));
        EXPECT_TRUE(isValidIpAddress(g_ctx.qps[1].ip_address));
        // printf("Current ip addresses before set local ib connection %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

        set_local_ib_connection(&g_ctx, false); // false = consensus thread
        
        g_ctx.sockfd = (int*)malloc(g_ctx.num_clients * sizeof(g_ctx.sockfd));

        tcp_server_listen();

        // TODO maybe sleep here
        tcp_client_connect();

        ASSERT_EQ(tcp_exch_ib_connection_info(&g_ctx), 0) << error_details(
                "Could not exchange connection, tcp_exch_ib_connection");

        // Print IB-connection details
        printf("Consensus thread connections:\n");
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            print_ib_connection("Local  Connection", &g_ctx.qps[i].local_connection);
            print_ib_connection("Remote Connection", &g_ctx.qps[i].remote_connection);    
        }

        for (int i = 0; i < g_ctx.num_clients; ++i) {
            qp_change_state_rts(&g_ctx.qps[i], g_ctx.ib_port);
        }
        printf("Main thred QPs changed to RTS mode\n");  
    }

    // Override this to define how to tear down the environment.
    void TearDown() override {
        printf("Destroying IB context\n");
        destroy_ctx(&g_ctx, false);

        int maxrecvsize = 100;
        char buf[maxrecvsize];
        
        printf("Closing socket\n");
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            if (i < g_ctx.my_index) { // I initiated this connection
                // do nothing
            } else { // I accepted this connection
                // wait for client to disconnect
                while (recv(g_ctx.sockfd[i], buf, maxrecvsize, 0) > 0) {}
            }
            shutdown(g_ctx.sockfd[i], SHUT_RDWR);
            close(g_ctx.sockfd[i]);
        }
    }
};


TEST(RDMATest, HelloWorld) {
  //empty
}

TEST(RDMATest, LeaderElectionCheckPermissions) {
    barrier_init(&entry_barrier, 2);
    barrier_init(&exit_barrier, 2);

    stop_le = false;
    spawn_leader_election_thread();

    barrier_cross(&entry_barrier);
    sleep(5);
    stop_le = true;
    barrier_cross(&exit_barrier);
}

TEST(RDMATest, LeaderElectionAskPermission) {
    barrier_init(&entry_barrier, 2);
    barrier_init(&exit_barrier, 2);
    stop_le = false;
    spawn_leader_election_thread();

    barrier_cross(&entry_barrier);

    if (g_ctx.my_index == 1) {
        printf("Asking for permission...\n");
        rdma_ask_permission(le_ctx.buf.le_data, le_ctx.my_index, true);
        // sleep(1);
        printf("1 trying to write to 0 -> should succeed\n");
        post_send(g_ctx.qps[0].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[0].mr_write->lkey, g_ctx.qps[0].remote_connection.rkey, g_ctx.qps[0].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 42, true);
        // check the CQ
        sleep(1);
        int ne;
        struct ibv_wc wc;

        do {
            ne = ibv_poll_cq(g_ctx.cq, 1, &wc);

            if (ne > 0) {
                printf("Work completion with id %llu has status %s (%d) \n", wc.wr_id, ibv_wc_status_str(wc.status), wc.status);
                sleep(1);
            } else {
                printf("ne was %d\n", ne);
            }
        } while(ne > 0);
    } else if (g_ctx.my_index == 0) {
        sleep(2);
        printf("0 trying to write to 1 -> should succeed\n");
        post_send(g_ctx.qps[0].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[0].mr_write->lkey, g_ctx.qps[0].remote_connection.rkey, g_ctx.qps[0].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 42, true);
        printf("0 trying to write to 2 -> should not succeed\n");
        post_send(g_ctx.qps[1].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[1].mr_write->lkey, g_ctx.qps[1].remote_connection.rkey, g_ctx.qps[1].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 43, true);

        // check the CQ
        sleep(1);
        int ne;
        struct ibv_wc wc;

        do {
            ne = ibv_poll_cq(g_ctx.cq, 1, &wc);

            if (ne > 0) {
                printf("Work completion with id %llu has status %s (%d) \n", wc.wr_id, ibv_wc_status_str(wc.status), wc.status);
                sleep(1);
            } else {
                printf("ne was %d\n", ne);
            }
        } while(ne > 0);
    } else {
        sleep(5);
    }

    stop_le = true;
    barrier_cross(&exit_barrier);

}

TEST(RDMATest, DetectLeaderFailure) {
    barrier_init(&entry_barrier, 2);
    barrier_init(&exit_barrier, 2);

    stop_le = false;
    spawn_leader_election_thread();

    barrier_cross(&entry_barrier);
    sleep(1);
    if (g_ctx.my_index != 0) {
        sleep(3);
        stop_le = true;
    } else {
        stop_le = true;
        sleep(3);
    }
    barrier_cross(&exit_barrier);    
}

TEST(RDMATest, BigTest) {    
    // spawn_leader_election_thread(); 
        
    printf("Going to sleep before consensus\n");
    
    if (g_ctx.my_index == 2) {
        // remove permission for 0
        // take away access from old mr
        TEST_NZ(ibv_rereg_mr(g_ctx.qps[0].mr_write, // the memory region
            IBV_REREG_MR_CHANGE_ACCESS, // we want to change the access flags
            g_ctx.pd, // the protection domain
            (void*)g_ctx.buf.log, g_ctx.len, 
            IBV_ACCESS_LOCAL_WRITE),
            "ibv_rereg_mr: failed to take away permission");

    } else {
        // sleep
        sleep(2);
    }

    if (g_ctx.my_index == 0) {

        // write to 1 5 times
        for (int i = 0; i < 5; ++i) {
            /* code */
            post_send(g_ctx.qps[0].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[0].mr_write->lkey, g_ctx.qps[0].remote_connection.rkey, g_ctx.qps[0].remote_connection.vaddr, IBV_WR_RDMA_WRITE, i, true);
        }
        // write to 2 -> error
        for (int i = 0; i < 5; i++) {
            post_send(g_ctx.qps[1].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[1].mr_write->lkey, g_ctx.qps[1].remote_connection.rkey, g_ctx.qps[1].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 42+i, true);
        }
        // write to 1 5 times
        sleep(1);
        for (int i = 0; i < 5; ++i) {
            /* code */
            post_send(g_ctx.qps[0].qp, g_ctx.buf.log, sizeof(uint64_t), g_ctx.qps[0].mr_write->lkey, g_ctx.qps[0].remote_connection.rkey, g_ctx.qps[0].remote_connection.vaddr, IBV_WR_RDMA_WRITE, i+5, true);
        }
        // check the CQ
        sleep(3);
        int ne;
        struct ibv_wc wc;

        do {
            ne = ibv_poll_cq(g_ctx.cq, 1, &wc);

            if (ne > 0) {
                printf("Work completion with id %llu has status %s (%d) \n", wc.wr_id, ibv_wc_status_str(wc.status), wc.status);
                sleep(1);
                if (wc.status == IBV_WC_REM_ACCESS_ERR) {
                    printf("Restarting the QP\n");
                    // qp\_restart(&g_ctx.qps[1], g_ctx.ib_port);
                }
            } else {
                printf("ne was %d\n", ne);
            }
        } while(ne > 0);
    } else {
        sleep(5);
    }

    // if(leader == g_ctx.my_index){


    //     g_ctx.buf.log->firstUndecidedOffset = 0;
    //     log_write_local_slot_string(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, "blablabla");
    //     log_increment_fuo(g_ctx.buf.log);
    //     log_write_local_slot_uint64(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, 5);
    //     log_increment_fuo(g_ctx.buf.log);
    //     log_write_local_slot_uint64(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, 5);

    //     // start timer
    //     struct timeval start, end;
    //     gettimeofday(&start, NULL);
        
    //     outer_loop(g_ctx.buf.log);
        
    //     // stop timer
    //     gettimeofday(&end, NULL);
    //     // output latency
    //     uint64_t duration = (end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec );
    //     double avg_latency = (1.0 * duration) / NB_ITERATIONS;

    //     printf("Done with outer loop. Average latency = %.2f\n", avg_latency);

    //     copy_remote_logs(0, SLOT, 7);
    //     for (int i = 0; i < g_ctx.num_clients; ++i) {
    //         log_print(g_ctx.qps[i].buf_copy.log);
    //     }

    //     // printf("Press ENTER to continue\n");
    //     // getchar();
        
    //     // For now, the message to be written into the clients buffer can be edited here
    //     //char *chPtr = &(g_ctx.buf.log->slots[0]);
    //     //strcpy(chPtr,"Saluton Teewurst. UiUi");

    //     // g_ctx.buf.log->minProposal = 70;
    //     // g_ctx.buf.log->slots[0].accValue = 42;
    //     // log_slot_t *slot = log_get_local_slot(g_ctx.buf.log, 4);
    //     // slot->accValue = 42;

    //     // // printf("Client. Writing to Server\n");
    //     // for (int i = 0; i < g_ctx.num_clients; ++i) {
    //     //     // rdma_write(i);
    //     //     uint64_t remote_addr = log_get_remote_address(g_ctx.buf.log, slot, ((log_t*)g_ctx.qps[i].remote_connection.vaddr));
    //     //     post_send(g_ctx.qps[i].qp, slot, sizeof(log_slot_t), g_ctx.qps[i].mr->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, 42);

    //     // }

    //     // struct ibv_wc wc;
    //     // //int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array);
    //     // wait_for_n(g_ctx.num_clients, 42, g_ctx.cq, 1, &wc);
        
    //     // printf("Server. Done with write. Reading from client\n");

    //     // sleep(1);
    //     // rdma_read(ctx, &data, 0);
    //     // printf("Printing local buffer: %s\n" ,chPtr);
        
    // } else { // Client

    //     // permission_switch(g_ctx.mr[1], g_ctx.mr[0], g_ctx.pd, g_ctx.buf, g_ctx.size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    //     // permission_switch(g_ctx.mr[0], g_ctx.mr[1], g_ctx.pd, g_ctx.buf, g_ctx.size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    //     // ibv_rereg_mr(g_ctx.mr[0], IBV_REREG_MR_CHANGE_ACCESS, g_ctx.pd, g_ctx.buf, g_ctx.size * 2, IBV_ACCESS_LOCAL_WRITE);
    //     // ibv_rereg_mr(g_ctx.mr[1], IBV_REREG_MR_CHANGE_ACCESS, g_ctx.pd, g_ctx.buf, g_ctx.size * 2, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
       
    //     sleep(3);
    //     printf("Client. Reading Local-Buffer (Buffer that was registered with MR)\n");
        
    //     // char *chPtr = (char *)g_ctx.qps[0].local_connection.vaddr;
            
    //     // while(1){
    //     //     if(strlen(chPtr) > 0){
    //     //         break;
    //     //     }
    //     // }



    //     log_print(g_ctx.buf.log);
        
    // }

    printf("Going to sleep after consensus\n");
    sleep(15);
    
    
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    AddGlobalTestEnvironment(new Environment);
    return RUN_ALL_TESTS();
}