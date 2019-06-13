// Based on rdma_bw.c program

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif


#include <pthread.h>

#include <infiniband/verbs.h>
#include "log.h"
#include "ibv_layer.h"
#include "utils.h"

#define RDMA_WRID 3
#define SHORT_SLEEP_DURATION 1

static int page_size;
static int sl = 1;
static pid_t pid;
static char* config_file;

struct global_context g_ctx = {
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
    .buf.log            = NULL,
    .len                = 0,
    .completed_ops      = NULL
};

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

typedef enum {SLOT, MIN_PROPOSAL} write_location_t;


static int die(const char *reason);

static void tcp_client_connect();
static void tcp_server_listen();
static void count_lines(char* filename, struct global_context *ctx);
static void parse_config(char* filename, struct global_context *ctx);
static bool compare_to_self(struct ifaddrs *ifaddr, char *addr);

static void init_ctx_common(struct global_context* ctx, bool is_le);
static void init_buf_le(struct global_context* ctx);
static void init_buf_consensus(struct global_context* ctx);
static void destroy_ctx(struct global_context* ctx, bool is_le);

static void spawn_leader_election_thread();
static void* leader_election(void* arg);
static void rdma_read_all_counters();
static void decide_leader();


static void outer_loop(log_t *log);
static void inner_loop(log_t *log, uint64_t propNr);
static int write_log_slot(log_t* log, uint64_t offset, uint64_t propNr, value_t* value);
static int write_min_proposal(log_t* log, uint64_t propNr);
static int read_min_proposals();
static int copy_remote_logs(uint64_t offset, write_location_t type, uint64_t size);
static void update_followers();
static value_t* freshest_accepted_value(uint64_t offset);
static void wait_for_majority();
static void wait_for_all(); 
static void rdma_write_to_all(log_t* log, uint64_t offset, write_location_t type, bool signaled);
static bool min_proposal_ok(uint64_t propNr);

int main(int argc, char *argv[])
{

    // if(argc == 2){
    //     g_ctx.num_clients = atoi(argv[1]);
    //     if (g_ctx.num_clients == 0) {
    //         g_ctx.num_clients = 1;
    //         g_ctx.servername = argv[1];
    //         printf("I am a client. The server is %s\n", g_ctx.servername);
    //     } else {
    //         printf("I am a server. I expect %d clients\n", g_ctx.num_clients);
    //     }
    // } else { // (argc != 2)
    //     die("*Error* Usage: rdma <server/nb_clients>\n");
    // }

    pid = getpid();


    if(!g_ctx.servername){
        // Print app parameters. This is basically from rdma_bw app. Most of them are not used atm
        printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
            pid, g_ctx.port, g_ctx.ib_port, g_ctx.len, g_ctx.tx_depth, sl);
    }

    // Is later needed to create random number for psn
    srand48(pid * time(NULL));
    
    page_size = sysconf(_SC_PAGESIZE);

    config_file = argv[1];
    count_lines(config_file, &g_ctx);
    
    init_ctx_common(&g_ctx, false); // false = consensus thread

    parse_config(config_file, &g_ctx);

    printf("Current ip addresses before set local ib connection %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

    set_local_ib_connection(&g_ctx, false); // false = consensus thread
    
    g_ctx.sockfd = malloc(g_ctx.num_clients * sizeof(g_ctx.sockfd));

    printf("Current ip addresses before server listen %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

    tcp_server_listen();

    printf("Current ip addresses after server listen %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

    // TODO maybe sleep here
    tcp_client_connect();

    // if(g_ctx.servername) { // I am a client
    //     g_ctx.sockfd[0] = tcp_client_connect();
    // } else { // I am the server
    //     tcp_server_listen();
    // }


    TEST_NZ(tcp_exch_ib_connection_info(&g_ctx),
            "Could not exchange connection, tcp_exch_ib_connection");

    // Print IB-connection details
    printf("Consensus thread connections:\n");
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        print_ib_connection("Local  Connection", &g_ctx.qps[i].local_connection);
        print_ib_connection("Remote Connection", &g_ctx.qps[i].remote_connection);    
    }
    
    spawn_leader_election_thread();

    if(g_ctx.servername){ // I am a client
        qp_change_state_rtr(&g_ctx);
    } else { // I am the server
        qp_change_state_rts(&g_ctx);
    }   
        
    printf("Going to sleep before consensus\n");
    sleep(5);


    if(!g_ctx.servername){
        /* Server - RDMA WRITE */


        g_ctx.buf.log->firstUndecidedOffset = 0;
        log_write_local_slot_string(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, "blablabla");
        log_increment_fuo(g_ctx.buf.log);
        log_write_local_slot_uint64(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, 5);
        log_increment_fuo(g_ctx.buf.log);
        log_write_local_slot_uint64(g_ctx.buf.log, g_ctx.buf.log->firstUndecidedOffset, 4, 5);

        outer_loop(g_ctx.buf.log);
        printf("Done with outer loop. Copying logs\n");

        copy_remote_logs(0, SLOT, 7);
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            log_print(g_ctx.qps[i].buf_copy.log);
        }

        // printf("Press ENTER to continue\n");
        // getchar();
        
        // For now, the message to be written into the clients buffer can be edited here
        //char *chPtr = &(g_ctx.buf.log->slots[0]);
        //strcpy(chPtr,"Saluton Teewurst. UiUi");

        // g_ctx.buf.log->minProposal = 70;
        // g_ctx.buf.log->slots[0].accValue = 42;
        // log_slot_t *slot = log_get_local_slot(g_ctx.buf.log, 4);
        // slot->accValue = 42;

        // // printf("Client. Writing to Server\n");
        // for (int i = 0; i < g_ctx.num_clients; ++i) {
        //     // rdma_write(i);
        //     uint64_t remote_addr = log_get_remote_address(g_ctx.buf.log, slot, ((log_t*)g_ctx.qps[i].remote_connection.vaddr));
        //     post_send(g_ctx.qps[i].qp, slot, sizeof(log_slot_t), g_ctx.qps[i].mr->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, 42);

        // }

        // struct ibv_wc wc;
        // //int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array);
        // wait_for_n(g_ctx.num_clients, 42, g_ctx.cq, 1, &wc);
        
        // printf("Server. Done with write. Reading from client\n");

        // sleep(1);
        // rdma_read(ctx, &data, 0);
        // printf("Printing local buffer: %s\n" ,chPtr);
        
    } else { // Client

        // permission_switch(g_ctx.mr[1], g_ctx.mr[0], g_ctx.pd, g_ctx.buf, g_ctx.size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
        // permission_switch(g_ctx.mr[0], g_ctx.mr[1], g_ctx.pd, g_ctx.buf, g_ctx.size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        // ibv_rereg_mr(g_ctx.mr[0], IBV_REREG_MR_CHANGE_ACCESS, g_ctx.pd, g_ctx.buf, g_ctx.size * 2, IBV_ACCESS_LOCAL_WRITE);
        // ibv_rereg_mr(g_ctx.mr[1], IBV_REREG_MR_CHANGE_ACCESS, g_ctx.pd, g_ctx.buf, g_ctx.size * 2, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
       

        printf("Client. Reading Local-Buffer (Buffer that was registered with MR)\n");
        
        // char *chPtr = (char *)g_ctx.qps[0].local_connection.vaddr;
            
        // while(1){
        //     if(strlen(chPtr) > 0){
        //         break;
        //     }
        // }



        log_print(g_ctx.buf.log);
        
    }

    printf("Going to sleep after consensus\n");
    sleep(15);
    
    printf("Destroying IB context\n");
    destroy_ctx(&g_ctx, false);
    
    printf("Closing socket\n");
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        close(g_ctx.sockfd[i]);
    }    
    return 0;
}

static void
spawn_leader_election_thread() {
    pthread_t le_thread;
    
    TEST_NZ(pthread_create(&le_thread, NULL, leader_election, NULL), "Could not create leader election thread");
}

static void*
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
        decide_leader();
        // communicate the leader to the main thread
        // sleep
        nanosleep((const struct timespec[]){{0, LE_SLEEP_DURATION_NS}}, NULL);
    }

    destroy_ctx(&le_ctx, true);
    pthread_exit(NULL);   
}

static void
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

// TODO need to fix ids / names first
static void
decide_leader() {
    for (int i = 0; i < le_ctx.num_clients; ++i) {

        counter_t* counters = le_ctx.qps[i].buf_copy.counter;
        if (counters->count_old != counters->count_oldest) {
            // return i;
            printf("Node %d is a valid leader\n", i);
        }
        
        if (le_ctx.completed_ops[i] == le_ctx.round_nb) {
            if (counters->count_cur != counters->count_old) {
                printf("Node %d is a valid leader\n", i);
            }
        }
    }

    // return myself no smaller id incremented their counter
}


static void count_lines(char* filename, struct global_context *ctx) {
    


    FILE * fp;
    char * line = malloc(NI_MAXHOST * sizeof(char));
    size_t len = NI_MAXHOST * sizeof(char);
    ssize_t read;

    TEST_NULL(fp = fopen(filename, "r"), "open config file failure");

    int i = 0;
    while ((read = getline(&line, &len, fp)) != -1) {
        i++;
    }

    ctx->num_clients = i-1;

    fclose(fp);
    if (line)
        free(line);


    // read the configuration file
    // for each line in the configuration file
    //  check if the ip address in this line is mine
    //  if yes, my id = the number of the current line
    //  else, id[next available qp_context] = the number of the current line
}

static void parse_config(char* filename, struct global_context *ctx) {
    struct ifaddrs *ifaddr;
    

    TEST_NZ(getifaddrs(&ifaddr), "getifaddrs");

    /* Walk through linked list, maintaining head pointer so we
      can free list later */


    FILE * fp;
    char * line = malloc(NI_MAXHOST * sizeof(char));
    size_t len = NI_MAXHOST * sizeof(char);
    ssize_t read;

    TEST_NULL(fp = fopen(filename, "r"), "open config file failure");

    int i = 0;
    while ((read = getline(&line, &len, fp)) != -1) {
        // strip the newline from the end of line
        if (line[read - 1] == '\n') {
            line[read - 1] = '\0';
            --read;
        }
        if (compare_to_self(ifaddr, line)) {
            printf("My id is %s\n", line);
            strcpy(ctx->my_ip_address, line);
            ctx->my_index = i;
        } else {
            strcpy(ctx->qps[i].ip_address, line);
            printf("The id of %d is %s\n", i, ctx->qps[i].ip_address);
            i++;
        }
    }

    fclose(fp);
    if (line)
        free(line);

    freeifaddrs(ifaddr);

    // read the configuration file
    // for each line in the configuration file
    //  check if the ip address in this line is mine
    //  if yes, my id = the number of the current line
    //  else, id[next available qp_context] = the number of the current line
}

static bool
compare_to_self(struct ifaddrs *ifaddr, char *addr) {
    struct ifaddrs *ifa;
    int family;
    char host[NI_MAXHOST];

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        /* For an AF_INET* interface address, display the address */

        if (family == AF_INET) {
            TEST_NZ(getnameinfo(ifa->ifa_addr,
                   sizeof(struct sockaddr_in),
                   host, NI_MAXHOST,
                   NULL, 0, NI_NUMERICHOST),
                   "getnameinfo() failed");

            printf("Comparing %s and %s = %d\n", host, addr, strcmp(host, addr));
            if (0 == strcmp(host, addr)) {
                return true;
            }
        }
    }

    return false;
}

/*
 *  tcp_client_connect
 * ********************
 *    Creates a connection to a TCP server 
 */
static void tcp_client_connect()
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family        = AF_UNSPEC,
        .ai_socktype    = SOCK_STREAM
    };

    char *service;
    // int sockfd = -1;


    // int n;
    // TEST_N(n = getaddrinfo(NULL, service, &hints, &res),
    //         "getaddrinfo threw error");

    // struct sockaddr_in* aux;
    // aux = (struct sockaddr_in *)res->ai_addr;
    // printf("My address is %s\n", inet_ntoa(aux->sin_addr));

    // struct sockaddr_in * clientaddr;
    // socklen_t clientlen = sizeof(clientaddr);

    
    TEST_N(asprintf(&service, "%d", g_ctx.port),
            "Error writing port-number to port-string");

    for (int i = 0; i < g_ctx.my_index; ++i) {

        printf("Current ip addresses before %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);
        TEST_N(getaddrinfo(g_ctx.qps[i].ip_address, service, &hints, &res),
                "getaddrinfo threw error");
        printf("Current ip addresses after %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);

        for(t = res; t; t = t->ai_next){
            // clientaddr = (struct sockaddr_in *)t->ai_addr;

            // printf("Server address is %s\n", inet_ntoa(clientaddr->sin_addr));

            TEST_N(g_ctx.sockfd[i] = socket(t->ai_family, t->ai_socktype, t->ai_protocol),
                    "Could not create client socket");

            TEST_N(connect(g_ctx.sockfd[i], t->ai_addr, t->ai_addrlen),
                    "Could not connect to server");    
        }

    }
    
    freeaddrinfo(res);
}

/*
 *  tcp_server_listen
 * *******************
 *  Creates a TCP server socket  which listens for incoming connections 
 */
static void tcp_server_listen() {
    struct addrinfo *res;
    struct addrinfo hints = {
        .ai_flags        = AI_PASSIVE,
        .ai_family        = AF_UNSPEC,
        .ai_socktype    = SOCK_STREAM    
    };

    char *service;
    int sockfd = -1;
    int accepted_socket;
    int n;
    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(clientaddr);

    TEST_N(asprintf(&service, "%d", g_ctx.port),
            "Error writing port-number to port-string");

    printf("Current ip addresses before getaddrinfo in listen %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);


    TEST_N(n = getaddrinfo(NULL, service, &hints, &res),
            "getaddrinfo threw error");

    printf("Current ip addresses after getaddrinfo in listen %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);
    printf("Some more prints %s\n", g_ctx.qps[1]);


    TEST_N(sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol),
                "Could not create server socket");
    
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

    TEST_N(bind(sockfd,res->ai_addr, res->ai_addrlen),
            "Could not bind addr to socket"); 
    
    listen(sockfd, 1);

    printf("Current ip addresses after started listening %s %s\n", g_ctx.qps[0].ip_address, g_ctx.qps[1].ip_address);


    for (int i = g_ctx.my_index; i < g_ctx.num_clients; ++i) {

        TEST_N(accepted_socket = accept(sockfd, (struct sockaddr *)&clientaddr, &clientlen),
            "server accept failed");
        char* client_ip_addr = inet_ntoa(clientaddr.sin_addr);

        for (int j = g_ctx.my_index; j < g_ctx.num_clients; ++j) {
            if (strcmp(client_ip_addr, g_ctx.qps[j].ip_address) == 0) {
                g_ctx.sockfd[j] = accepted_socket;
                printf("Client address is %s and its index is %d\n", inet_ntoa(clientaddr.sin_addr), j);
                break;
            }
        }
    }

    freeaddrinfo(res);

}

static void init_buf_le(struct global_context* ctx) {
    le_ctx.buf.counter = malloc(sizeof(counter_t));
    memset(le_ctx.buf.counter, 0, sizeof(counter_t));
    le_ctx.len = sizeof(counter_t);
    for (int i = 0; i < ctx->num_clients; i++) {
        ctx->qps[i].buf_copy.counter = malloc(sizeof(counter_t));
        memset(ctx->qps[i].buf_copy.counter, 0, sizeof(counter_t));
    }
}

static void init_buf_consensus(struct global_context* ctx) {

    g_ctx.buf.log = log_new();
    g_ctx.len = log_size(g_ctx.buf.log);
    for (int i = 0; i < ctx->num_clients; i++) {
        ctx->qps[i].buf_copy.log = log_new();
    }
}

/*
 *     init_ctx
 * **********
 *    This method initializes the Infiniband Context
 *     It creates structures for: ProtectionDomain, MemoryRegion, CompletionChannel, Completion Queues, Queue Pair
 */
static void init_ctx_common(struct global_context* ctx, bool is_le)
{
    
    // TEST_NZ(posix_memalign(&g_ctx.buf, page_size, g_ctx.size * 2),
                // "could not allocate working buffer g_ctx.buf");
    void *write_buf;
    void *read_buf;

    ctx->qps = malloc(ctx->num_clients * sizeof(struct qp_context));
    memset(ctx->qps, 0, ctx->num_clients * sizeof(struct qp_context));    

    if (is_le) {
        init_buf_le(ctx);
    } else {
        init_buf_consensus(ctx);
    }

    ctx->completed_ops = malloc(ctx->num_clients * sizeof(uint64_t));
    memset(ctx->completed_ops, 0, ctx->num_clients * sizeof(uint64_t));

    if (ctx->ib_dev == NULL) {
        struct ibv_device **dev_list;

        TEST_Z(dev_list = ibv_get_device_list(NULL),
                "No IB-device available. get_device_list returned NULL");

        TEST_Z(ctx->ib_dev = dev_list[0],
                "IB-device could not be assigned. Maybe dev_list array is empty");

        TEST_Z(ctx->context = ibv_open_device(ctx->ib_dev),
                "Could not create context, ibv_open_device");
    }
    
    TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context),
        "Could not allocate protection domain, ibv_alloc_pd");

    /* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
     */

    
    TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context),
            "Could not create completion channel, ibv_create_comp_channel");


    TEST_Z(ctx->cq = ibv_create_cq(ctx->context,ctx->tx_depth, NULL,  ctx->ch, 0),
                "Could not create completion queue, ibv_create_cq"); 

    for (int i = 0; i < ctx->num_clients; i++) {
        if (is_le) {
           write_buf = (void*)ctx->buf.counter;
           read_buf = (void*)ctx->qps[i].buf_copy.counter;
        } else {
           write_buf = (void*)ctx->buf.log;
           read_buf = (void*)ctx->qps[i].buf_copy.log;
        }
        TEST_Z(ctx->qps[i].mr_write = ibv_reg_mr(ctx->pd, write_buf, ctx->len, 
                        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE),
                    "Could not allocate mr, ibv_reg_mr. Do you have root access?");
        TEST_Z(ctx->qps[i].mr_read = ibv_reg_mr(ctx->pd, read_buf, ctx->len, 
                        IBV_ACCESS_LOCAL_WRITE),
                    "Could not allocate mr, ibv_reg_mr. Do you have root access?");

        struct ibv_qp_init_attr qp_init_attr = {
            .send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .qp_type = IBV_QPT_RC,
            .cap = {
                .max_send_wr = ctx->tx_depth,
                .max_recv_wr = 1,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 0
            }
        };

        TEST_Z(ctx->qps[i].qp = ibv_create_qp(ctx->pd, &qp_init_attr),
                "Could not create queue pair, ibv_create_qp");    
        
    }
    qp_change_state_init(ctx);        
}

static void destroy_ctx(struct global_context* ctx, bool is_le){
        
    for (int i = 0; i < ctx->num_clients; i++) {
        rc_qp_destroy( ctx->qps[i].qp, ctx->cq );
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

    TEST_NZ(ibv_dealloc_pd(ctx->pd),
            "Could not deallocate protection domain, ibv_dealloc_pd");    
    
    if (is_le) {
        free(ctx->buf.counter);
    } else {
        log_free(ctx->buf.log);
    }
    free(ctx->completed_ops);
    
}




static void
outer_loop(log_t *log) {
    uint64_t propNr;
    // while (true) {
        // wait until I am leader
        // get permissions
        // bring followers up to date with me
        update_followers();
        propNr = 1; // choose number higher than any proposal number seen before
        inner_loop(log, propNr);
    // }
}

static void
inner_loop(log_t *log, uint64_t propNr) {
    uint64_t offset = 0;
    value_t* v;

    bool needPreparePhase = true;

    while (offset < 100) {
        offset = log->firstUndecidedOffset;
        if (needPreparePhase) {
            read_min_proposals();
            wait_for_majority();
            if (!min_proposal_ok(propNr)) { // check if any of the read minProposals are larger than our propNr
                return;
            } 
            // write propNr into minProposal at a majority of logs // if fails, goto outerLoop
            write_min_proposal(log, propNr);
            // read slot at position "offset" from a majority of logs // if fails, abort
            copy_remote_logs(offset, SLOT, DEFAULT_VALUE_SIZE);
            // value with highest accepted proposal among those read
            value_t* freshVal = freshest_accepted_value(offset);
            if (freshVal->len != 0) {
                v = freshVal;
            } else {
                needPreparePhase = false;
                // v = get_my_value();
                v = malloc(sizeof(value_t) + sizeof(uint64_t));
                v->len = sizeof(uint64_t);
                memcpy(v->val, &offset, 8);
            }
        }
        // write v, propNr into slot at position "offset" at a majority of logs // if fails, goto outerLoop
        write_log_slot(log, offset, propNr, v);
        wait_for_majority();
        // increment the firstUndecidedOffset
        log_increment_fuo(log);    
    }
}

static void
update_followers() {
    void* local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;

    int nb_to_wait = (g_ctx.num_clients/2) + 1;

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);
    for (int i = 0; i < g_ctx.num_clients; ++i) {
    //  copy all or a part of the remote log
    //  overwrite remote log from their firstUn.. to my firstUn...
        if ( g_ctx.buf.log->firstUndecidedOffset <= g_ctx.qps[i].buf_copy.log->firstUndecidedOffset) {
            nb_to_wait--;
            continue;
        }
        WRID_SET_CONN(wrid, i);
        local_address = log_get_local_slot(g_ctx.buf.log, g_ctx.qps[i].buf_copy.log->firstUndecidedOffset);
        // here we are assuming that the logs agree up to g_ctx.qps[i].buf_copy.log->firstUndecidedOffset
        req_size = g_ctx.buf.log->firstUndecidedOffset - g_ctx.qps[i].buf_copy.log->firstUndecidedOffset;
        remote_addr = log_get_remote_address(g_ctx.buf.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_write->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, wrid, false);

        //  update remote firstUndecidedOffset
        local_address = &g_ctx.buf.log->firstUndecidedOffset;
        req_size = sizeof(g_ctx.buf.log->firstUndecidedOffset);
        remote_addr = log_get_remote_address(g_ctx.buf.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_write->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, wrid, true);        

    }

    if (nb_to_wait > 0) {
        // array to store the work completions inside wait_for_n
        // we might want to place this in the global context later
        struct ibv_wc wc_array[g_ctx.num_clients];
        // currently we are polling at most num_clients WCs from the CQ at a time
        // we might want to change this number later
        wait_for_n(nb_to_wait, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array, g_ctx.completed_ops);        
    }
}

static bool
min_proposal_ok(uint64_t propNr) {

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.qps[i].buf_copy.log->minProposal > propNr) {
            return false;
        }
    }

    return true;
}

static int
write_log_slot(log_t* log, uint64_t offset, uint64_t propNr, value_t* value) {

    if (value->len <= 8) {
        printf("Write log slot %lu, %lu, %lu\n", offset, propNr, *(uint64_t*)value->val);
        log_write_local_slot_uint64(log, offset, propNr, *(uint64_t*)value->val);
    } else {
        printf("Write log slot %lu, %lu, %s\n", offset, propNr, value->val);
        log_write_local_slot_string(log, offset, propNr, (char*)value->val);        
    }


    // post sends to everyone
    rdma_write_to_all(log, offset, SLOT, true);
    
}

static int
write_min_proposal(log_t* log, uint64_t propNr) {
    log->minProposal = propNr;

    rdma_write_to_all(log, 0, MIN_PROPOSAL, false); // offset is ignored for MIN_PROPOSAL
}


static int
read_min_proposals() {

    copy_remote_logs(0, MIN_PROPOSAL, 0); // size and offset are ignored for MIN_PROPOSAL
}

static int
copy_remote_logs(uint64_t offset, write_location_t type, uint64_t size) {

    void* local_address;
    uint64_t remote_addr;
    size_t req_size;
    log_slot_t *slot;
    uint64_t wrid = 0;

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);

    for (int i = 0; i < g_ctx.num_clients; ++i) {

        switch(type) {
            case SLOT:
                slot = log_get_local_slot(g_ctx.qps[i].buf_copy.log, offset);
                local_address = slot;
                // Igor: problem: we can't know ahead of time how big the slot will be
                // Idea: initially copy a default size (large enough to include the length) and if not enough, copy again
                req_size = sizeof(log_slot_t) + size;
                break;
            case MIN_PROPOSAL:
                local_address = &g_ctx.qps[i].buf_copy.log->minProposal;
                req_size = sizeof(g_ctx.qps[i].buf_copy.log->minProposal);
                break;
        }   

        WRID_SET_CONN(wrid, i);
        remote_addr = log_get_remote_address(g_ctx.qps[i].buf_copy.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_read->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_READ, wrid, true);
    }

    if (type != SLOT) return;

    // for each entry that was seen as completed by the most recent wait_for_n
    // check length and, if necessary, re-issue read
    // wait for "the right number" complete
    // re-check and potentially start over
    int majority = (g_ctx.num_clients/2) + 1;
    int not_ok_slots = majority;
    struct ibv_wc wc_array[g_ctx.num_clients];
    uint64_t correct_sizes[g_ctx.num_clients];
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        correct_sizes[i] = size;
    }


    while (not_ok_slots > 0) {
        wait_for_n(not_ok_slots, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array, g_ctx.completed_ops);

        not_ok_slots = 0;
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            if (g_ctx.completed_ops[i] == g_ctx.round_nb) {
                slot = log_get_local_slot(g_ctx.qps[i].buf_copy.log, offset);
                if (slot->accValue.len > correct_sizes[i]) {
                    not_ok_slots++;
                    // increase length
                    // re-issue the copy for this specific slot
                    local_address = slot;
                    // Igor: problem: we can't know ahead of time how big the slot will be
                    // Idea: initially copy a default size (large enough to include the length) and if not enough, copy again
                    req_size = sizeof(log_slot_t) + slot->accValue.len;
                    correct_sizes[i] = slot->accValue.len; // so that on the next loop iteration, we compare against the right size
                    WRID_SET_CONN(wrid, i);
                    remote_addr = log_get_remote_address(g_ctx.qps[i].buf_copy.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
                    post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_read->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_READ, wrid, true);
                }
            }
        }
    }
}

static void
rdma_write_to_all(log_t* log, uint64_t offset, write_location_t type, bool signaled) {

    void* local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;


    switch(type) {
        case SLOT:
            local_address = log_get_local_slot(log, offset);
            req_size = log_slot_size(g_ctx.buf.log, offset);
            break;
        case MIN_PROPOSAL:
            local_address = &log->minProposal;
            req_size = sizeof(log->minProposal);
            break;
    }

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);
    for (int i = 0; i < g_ctx.num_clients; ++i) {

        WRID_SET_CONN(wrid, i);
        remote_addr = log_get_remote_address(log, local_address, ((log_t*)g_ctx.qps[i].remote_connection.vaddr));
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_write->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, wrid, signaled);
    }
}

// Igor - potential problem: we only look at fresh items, but copy_remote_logs might have cleared and overwritten the completed_ops
// array several times, so we cannot know which are the fresh items
// Can solve this in wait_for_n: don't clear completed_ops, rather update it with the most recent round_nb for which we have reveiced a 
static value_t*
freshest_accepted_value(uint64_t offset) {
    uint64_t max_acc_prop;
    value_t* freshest_value;
    
    // start with my accepted proposal and value for the given offset
    log_slot_t* my_slot = log_get_local_slot(g_ctx.buf.log, offset);
    max_acc_prop = my_slot->accProposal;
    freshest_value = &my_slot->accValue;

    // go only through "fresh" slots (those to which reads were completed in the preceding wait_for_n)
    log_slot_t* remote_slot;
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.completed_ops[i] == g_ctx.round_nb) {
            remote_slot = log_get_local_slot(g_ctx.qps[i].buf_copy.log, offset);
            if (remote_slot->accProposal > max_acc_prop) {
                max_acc_prop = remote_slot->accProposal;
                freshest_value = &remote_slot->accValue;
            }            
        }
    }

    return freshest_value;
}




static void
wait_for_majority() {
    int majority = (g_ctx.num_clients/2) + 1;

    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    wait_for_n(majority, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array, g_ctx.completed_ops);
}

static void
wait_for_all() {
    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    wait_for_n(g_ctx.num_clients, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array, g_ctx.completed_ops); 
}

