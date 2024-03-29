// Based on rdma_bw.c program

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

#include <netdb.h>

#include <infiniband/verbs.h>
#include "log.h"

#define RDMA_WRID 3

// if x is NON-ZERO, error is printed
#define TEST_NZ(x,y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x,y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x,y) do { if ((x)<0) die(y); } while (0)

/**
 * The WR Identifier (WRID)
 * the WRID is a 64-bit value [SSN|WA|TAG|CONN], where
    * SSN is the Send Sequence Number
    * WA is the Wrap-Around flag, set for log update WRs 
    * TAG is a flag set for special signaled WRs (to avoid QPs overflow)
    * CONN is a 8-bit index that identifies the connection (the remote server)
 */
/* The CONN consists of the 8 least significant bits (lsbs) */
#define WRID_GET_CONN(wrid) (uint8_t)((wrid) & (0xFF))
#define WRID_SET_CONN(wrid, conn) (wrid) = (conn | ((wrid >> 8) << 8))
/* The TAG flag is the 9th lsb */
#define WRID_IS_COPY_SLOT(wrid) ((wrid) & (1 << 8))
#define WRID_SET_COPY_SLOT(wrid) (wrid) |= 1 << 8
#define WRID_UNSET_COPY_SLOT(wrid) (wrid) &= ~(1 << 8)
/* The WA flag is the 10th lsb */
#define WRID_GET_WA(wrid) ((wrid) & (1 << 9))
#define WRID_SET_WA(wrid) (wrid) |= 1 << 9
#define WRID_UNSET_WA(wrid) (wrid) &= ~(1 << 9)
/* The SSN consists of the most significant 54 bits */
#define WRID_GET_SSN(wrid) ((wrid) >> 10)
#define WRID_SET_SSN(wrid, ssn) (wrid) = (((ssn) << 10) | ((wrid) & 0x3FF))

// Status code categories for work completions
#define WC_SUCCESS          0
#define WC_EXPECTED_ERROR   1
#define WC_UNEXPECTED_ERROR 2

static int page_size;
static int sl = 1;
static pid_t pid;

struct ib_connection {
    int                 lid;
    int                 qpn;
    int                 psn;
    unsigned            rkey;
    unsigned long long  vaddr;
};

struct qp_context {
    struct ibv_qp               *qp;
    // MR for reading from a remote log into my local copy of that log
    // Note: different mr_read MRs refer to different phyisical locations, and they have minimal permissions
    struct ibv_mr               *mr_read;
    // MR for writing from my local log to another node's log
    // Note: all mr_write MRs refer to the same physical location, but may have different permissions
    struct ibv_mr               *mr_write; 

    struct ib_connection        local_connection;
    struct ib_connection        remote_connection;
    char                        *servername; // Igor: should we store this per-connection?
    log_t                       *log_copy;
};

struct global_context {
    struct ibv_device           *ib_dev;
    struct ibv_context          *context;
    struct ibv_pd               *pd;
    struct ibv_cq               *cq;
    struct ibv_comp_channel     *ch;
    struct qp_context           *qps;
    uint64_t                    round_nb;
    int                         num_clients;
    int                         port;
    int                         ib_port;
    int                         tx_depth;
    int                         *sockfd;
    char                        *servername;
    log_t                       *log; 
    size_t                      len; 
    int                         *completed_ops;
};

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
    .log                = NULL,
    .len                = 1000,
    .completed_ops      = NULL
};

typedef enum {SLOT, MIN_PROPOSAL} write_location_t;


static int die(const char *reason);

static int tcp_client_connect();
static void tcp_server_listen();

static void init_ctx();
static void destroy_ctx();

static void set_local_ib_connection();
static void print_ib_connection(char *conn_name, struct ib_connection *conn);

static int tcp_exch_ib_connection_info();

static int qp_change_state_init(struct ibv_qp *qp);
static int qp_change_state_rtr(struct ibv_qp *qp, int id);
static int qp_change_state_rts(struct ibv_qp *qp, int id);
static void rc_qp_destroy( struct ibv_qp *qp, struct ibv_cq *cq );

static void rdma_write(int id);
static void rdma_read(int id);
static int permission_switch(struct ibv_mr* old_mr, struct ibv_mr* new_mr, struct ibv_pd* pd, void* addr, size_t length, int old_new_flags, int new_new_flags);
static int wait_for_n(int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array);
static int handle_work_completion( struct ibv_wc *wc );
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
static int post_send(struct ibv_qp *qp, void* buf, uint32_t len, uint32_t lkey, uint32_t rkey, uint64_t remote_addr, enum ibv_wr_opcode opcode, uint64_t wrid, bool signaled);
static bool min_proposal_ok(uint64_t propNr);

int main(int argc, char *argv[])
{

    if(argc == 2){
        g_ctx.num_clients = atoi(argv[1]);
        if (g_ctx.num_clients == 0) {
            g_ctx.num_clients = 1;
            g_ctx.servername = argv[1];
            printf("I am a client. The server is %s\n", g_ctx.servername);
        } else {
            printf("I am a server. I expect %d clients\n", g_ctx.num_clients);
        }
    } else { // (argc != 2)
        die("*Error* Usage: rdma <server/nb_clients>\n");
    }

    pid = getpid();

    if(!g_ctx.servername){
        // Print app parameters. This is basically from rdma_bw app. Most of them are not used atm
        printf("PID=%d | port=%d | ib_port=%d | size=%lu | tx_depth=%d | sl=%d |\n",
            pid, g_ctx.port, g_ctx.ib_port, g_ctx.len, g_ctx.tx_depth, sl);
    }

    // Is later needed to create random number for psn
    srand48(pid * time(NULL));
    
    page_size = sysconf(_SC_PAGESIZE);
    
    init_ctx();

    set_local_ib_connection();
    
    g_ctx.sockfd = malloc(g_ctx.num_clients * sizeof(g_ctx.sockfd));
    if(g_ctx.servername) { // I am a client
        g_ctx.sockfd[0] = tcp_client_connect();
    } else { // I am the server
        tcp_server_listen();
    }

    TEST_NZ(tcp_exch_ib_connection_info(),
            "Could not exchange connection, tcp_exch_ib_connection");

    // Print IB-connection details
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        print_ib_connection("Local  Connection", &g_ctx.qps[i].local_connection);
        print_ib_connection("Remote Connection", &g_ctx.qps[i].remote_connection);    
    }

    if(g_ctx.servername){ // I am a client
        qp_change_state_rtr(g_ctx.qps[0].qp, 0);
    } else { // I am the server
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            qp_change_state_rts(g_ctx.qps[i].qp, i);
        }
    }    

    if(!g_ctx.servername){
        /* Server - RDMA WRITE */

        printf("Press ENTER to start\n");
        getchar();

        g_ctx.log->firstUndecidedOffset = 0;
        log_write_local_slot_string(g_ctx.log, g_ctx.log->firstUndecidedOffset, 4, "blablabla");
        log_increment_fuo(g_ctx.log);
        log_write_local_slot_uint64(g_ctx.log, g_ctx.log->firstUndecidedOffset, 4, 5);
        log_increment_fuo(g_ctx.log);
        log_write_local_slot_uint64(g_ctx.log, g_ctx.log->firstUndecidedOffset, 4, 5);

        outer_loop(g_ctx.log);
        printf("Done with outer loop. Copying logs\n");

        copy_remote_logs(0, SLOT, 7);
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            log_print(g_ctx.qps[i].log_copy);
        }

        // printf("Press ENTER to continue\n");
        // getchar();
        
        // For now, the message to be written into the clients buffer can be edited here
        //char *chPtr = &(g_ctx.log->slots[0]);
        //strcpy(chPtr,"Saluton Teewurst. UiUi");

        // g_ctx.log->minProposal = 70;
        // g_ctx.log->slots[0].accValue = 42;
        // log_slot_t *slot = log_get_local_slot(g_ctx.log, 4);
        // slot->accValue = 42;

        // // printf("Client. Writing to Server\n");
        // for (int i = 0; i < g_ctx.num_clients; ++i) {
        //     // rdma_write(i);
        //     uint64_t remote_addr = log_get_remote_address(g_ctx.log, slot, ((log_t*)g_ctx.qps[i].remote_connection.vaddr));
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


        printf("Press ENTER to print log\n");
        getchar();

        log_print(g_ctx.log);
        
        printf("Press ENTER to exit\n");
        getchar();
    }
    
    printf("Destroying IB context\n");
    destroy_ctx();
    
    printf("Closing socket\n");
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        close(g_ctx.sockfd[i]);
    }    
    return 0;
}

static int die(const char *reason){
    fprintf(stderr, "Err: %s - %s\n ", strerror(errno), reason);
    exit(EXIT_FAILURE);
    return -1;
}

/*
 *  tcp_client_connect
 * ********************
 *    Creates a connection to a TCP server 
 */
static int tcp_client_connect()
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family        = AF_UNSPEC,
        .ai_socktype    = SOCK_STREAM
    };

    char *service;
    int sockfd = -1;

    TEST_N(asprintf(&service, "%d", g_ctx.port),
            "Error writing port-number to port-string");

    TEST_N(getaddrinfo(g_ctx.servername, service, &hints, &res),
            "getaddrinfo threw error");

    for(t = res; t; t = t->ai_next){
        TEST_N(sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol),
                "Could not create client socket");

        TEST_N(connect(sockfd,t->ai_addr, t->ai_addrlen),
                "Could not connect to server");    
    }
    
    freeaddrinfo(res);
    
    return sockfd;
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
    int n;

    TEST_N(asprintf(&service, "%d", g_ctx.port),
            "Error writing port-number to port-string");

    TEST_N(n = getaddrinfo(NULL, service, &hints, &res),
            "getaddrinfo threw error");

    TEST_N(sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol),
                "Could not create server socket");
    
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

    TEST_N(bind(sockfd,res->ai_addr, res->ai_addrlen),
            "Could not bind addr to socket"); 
    
    listen(sockfd, 1);

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        TEST_N(g_ctx.sockfd[i] = accept(sockfd, NULL, 0),
            "server accept failed");
    }

    freeaddrinfo(res);

}

/*
 *     init_ctx
 * **********
 *    This method initializes the Infiniband Context
 *     It creates structures for: ProtectionDomain, MemoryRegion, CompletionChannel, Completion Queues, Queue Pair
 */
static void init_ctx()
{
    
    // TEST_NZ(posix_memalign(&g_ctx.buf, page_size, g_ctx.size * 2),
                // "could not allocate working buffer g_ctx.buf");

    // memset(g_ctx.buf, 0, g_ctx.size * 2);

    g_ctx.log = log_new(g_ctx.len);

    g_ctx.completed_ops = malloc(g_ctx.num_clients * sizeof(int));
    memset(g_ctx.completed_ops, 0, g_ctx.num_clients * sizeof(int));

    struct ibv_device **dev_list;

    TEST_Z(dev_list = ibv_get_device_list(NULL),
            "No IB-device available. get_device_list returned NULL");

    TEST_Z(g_ctx.ib_dev = dev_list[0],
            "IB-device could not be assigned. Maybe dev_list array is empty");

    TEST_Z(g_ctx.context = ibv_open_device(g_ctx.ib_dev),
            "Could not create context, ibv_open_device");
    
    TEST_Z(g_ctx.pd = ibv_alloc_pd(g_ctx.context),
        "Could not allocate protection domain, ibv_alloc_pd");

    /* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
     */

    
    TEST_Z(g_ctx.ch = ibv_create_comp_channel(g_ctx.context),
            "Could not create completion channel, ibv_create_comp_channel");

    g_ctx.qps = malloc(g_ctx.num_clients * sizeof(struct qp_context));
    memset(g_ctx.qps, 0, g_ctx.num_clients * sizeof(struct qp_context));

    TEST_Z(g_ctx.cq = ibv_create_cq(g_ctx.context,g_ctx.tx_depth, NULL,  g_ctx.ch, 0),
                "Could not create completion queue, ibv_create_cq"); 

    for (int i = 0; i < g_ctx.num_clients; i++) {
        g_ctx.qps[i].log_copy = log_new(g_ctx.len);
        TEST_Z(g_ctx.qps[i].mr_write = ibv_reg_mr(g_ctx.pd, (void*)g_ctx.log, log_size(g_ctx.log), 
                        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE),
                    "Could not allocate mr, ibv_reg_mr. Do you have root access?");
        TEST_Z(g_ctx.qps[i].mr_read = ibv_reg_mr(g_ctx.pd, (void*)g_ctx.qps[i].log_copy, log_size(g_ctx.qps[i].log_copy), 
                        IBV_ACCESS_LOCAL_WRITE),
                    "Could not allocate mr, ibv_reg_mr. Do you have root access?");

        struct ibv_qp_init_attr qp_init_attr = {
            .send_cq = g_ctx.cq,
            .recv_cq = g_ctx.cq,
            .qp_type = IBV_QPT_RC,
            .cap = {
                .max_send_wr = g_ctx.tx_depth,
                .max_recv_wr = 1,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = 0
            }
        };

        TEST_Z(g_ctx.qps[i].qp = ibv_create_qp(g_ctx.pd, &qp_init_attr),
                "Could not create queue pair, ibv_create_qp");    
        
        qp_change_state_init(g_ctx.qps[i].qp);        
    }
}

static void destroy_ctx(){
        
    for (int i = 0; i < g_ctx.num_clients; i++) {
        rc_qp_destroy( g_ctx.qps[i].qp, g_ctx.cq );
    }
        
    TEST_NZ(ibv_destroy_cq(g_ctx.cq),
            "Could not destroy completion queue, ibv_destroy_cq");

    TEST_NZ(ibv_destroy_comp_channel(g_ctx.ch),
        "Could not destory completion channel, ibv_destroy_comp_channel");

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        TEST_NZ(ibv_dereg_mr(g_ctx.qps[i].mr_write),
                "Could not de-register memory region, ibv_dereg_mr");
        TEST_NZ(ibv_dereg_mr(g_ctx.qps[i].mr_read),
                "Could not de-register memory region, ibv_dereg_mr");
        log_free(g_ctx.qps[i].log_copy);
    }

    TEST_NZ(ibv_dealloc_pd(g_ctx.pd),
            "Could not deallocate protection domain, ibv_dealloc_pd");    
    
    log_free(g_ctx.log);
    free(g_ctx.completed_ops);
    
}

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
static void set_local_ib_connection(){

    // First get local lid
    struct ibv_port_attr attr;
    TEST_NZ(ibv_query_port(g_ctx.context,g_ctx.ib_port,&attr),
        "Could not get port attributes, ibv_query_port");

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        g_ctx.qps[i].local_connection.qpn = g_ctx.qps[i].qp->qp_num;
        g_ctx.qps[i].local_connection.rkey = g_ctx.qps[i].mr_write->rkey;
        g_ctx.qps[i].local_connection.lid = attr.lid;
        g_ctx.qps[i].local_connection.psn = lrand48() & 0xffffff;
        g_ctx.qps[i].local_connection.vaddr = (uintptr_t)g_ctx.log;
    }

}

static void print_ib_connection(char *conn_name, struct ib_connection *conn){
    
    printf("%s: LID %#04x, QPN %#06x, PSN %#06x RKey %#08x VAddr %#016Lx\n", 
            conn_name, conn->lid, conn->qpn, conn->psn, conn->rkey, conn->vaddr);

}

static int tcp_exch_ib_connection_info(){

    char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
    int parsed;

    struct ib_connection *local;
    
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        local = &g_ctx.qps[i].local_connection; 
        sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", 
                local->lid, local->qpn, local->psn, local->rkey, local->vaddr);
        if(write(g_ctx.sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not send connection_details to peer");
            return -1;
        }    

        if(read(g_ctx.sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not receive connection_details to peer");
            return -1;
        }
        struct ib_connection *remote = &g_ctx.qps[i].remote_connection;
        parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", 
                            &remote->lid, &remote->qpn, &remote->psn, &remote->rkey, &remote->vaddr);
        
        if(parsed != 5){
            fprintf(stderr, "Could not parse message from peer");
        }
    }

    return 0;
}

/*
 *  qp_change_state_init
 * **********************
 *    Changes Queue Pair status to INIT
 */
static int qp_change_state_init(struct ibv_qp *qp){
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state            = IBV_QPS_INIT;
    attr->pkey_index          = 0;
    attr->port_num            = g_ctx.ib_port;
    attr->qp_access_flags     = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

    TEST_NZ(ibv_modify_qp(qp, attr,
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
static int qp_change_state_rtr(struct ibv_qp *qp, int id){
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTR;
    attr->path_mtu              = IBV_MTU_2048;
    attr->dest_qp_num           = g_ctx.qps[id].remote_connection.qpn;
    attr->rq_psn                = g_ctx.qps[id].remote_connection.psn;
    attr->max_dest_rd_atomic    = 1;
    attr->min_rnr_timer         = 12;
    attr->ah_attr.is_global     = 0;
    attr->ah_attr.dlid          = g_ctx.qps[id].remote_connection.lid;
    attr->ah_attr.sl            = sl;
    attr->ah_attr.src_path_bits = 0;
    attr->ah_attr.port_num      = g_ctx.ib_port;

    TEST_NZ(ibv_modify_qp(qp, attr,
                IBV_QP_STATE                |
                IBV_QP_AV                   |
                IBV_QP_PATH_MTU             |
                IBV_QP_DEST_QPN             |
                IBV_QP_RQ_PSN               |
                IBV_QP_MAX_DEST_RD_ATOMIC   |
                IBV_QP_MIN_RNR_TIMER),
        "Could not modify QP to RTR state");

    free(attr);
    
    return 0;
}

/*
 *  qp_change_state_rts
 * **********************
 *  Changes Queue Pair status to RTS (Ready to send)
 *    QP status has to be RTR before changing it to RTS
 */
static int qp_change_state_rts(struct ibv_qp *qp, int id){

    qp_change_state_rtr(qp, id); 
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTS;
    attr->timeout               = 14;
    attr->retry_cnt             = 7;
    attr->rnr_retry             = 7;    /* infinite retry */
    attr->sq_psn                = g_ctx.qps[id].local_connection.psn;
    attr->max_rd_atomic         = 1;

    TEST_NZ(ibv_modify_qp(qp, attr,
                IBV_QP_STATE            |
                IBV_QP_TIMEOUT          |
                IBV_QP_RETRY_CNT        |
                IBV_QP_RNR_RETRY        |
                IBV_QP_SQ_PSN           |
                IBV_QP_MAX_QP_RD_ATOMIC),
        "Could not modify QP to RTS state");

    free(attr);

    return 0;
}

static void 
rc_qp_destroy( struct ibv_qp *qp, struct ibv_cq *cq )
{
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
static void rdma_write(int id){

    post_send(g_ctx.qps[id].qp, g_ctx.log, log_size(g_ctx.log), g_ctx.qps[id].mr_write->lkey, g_ctx.qps[id].remote_connection.rkey, g_ctx.qps[id].remote_connection.vaddr, IBV_WR_RDMA_WRITE, 42, true);

}    


/*
 *  rdma_read
 * **********************
 */
// static void rdma_read(int id){

//     post_send(g_ctx.qps[id].qp, g_ctx.buf, g_ctx.size, g_ctx._read->lkey, g_ctx.qps[id].remote_connection.rkey, g_ctx.qps[id].remote_connection.vaddr, IBV_WR_RDMA_READ, 42);

// }

static int permission_switch(struct ibv_mr* old_mr, struct ibv_mr* new_mr, struct ibv_pd* pd, void* addr, size_t length, int old_new_flags, int new_new_flags) {

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

// Waits until n send requests complete for round number round_nb
// Parameters:
// n = the number of work completions to wait for
// round_nb = the round number (SSN) that we expect to find inside the work completions (wr_id)
// cq = the completion queue to poll from
// num_entries = maximum number of entries to poll from cq
// wc_array = a pre-allocated array to store the polled work completions 
// Returns:
static int wait_for_n(int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array) {
    int success_count = 0;
    int ne = 0;
    int i;
    int ret;
    uint64_t wr_id;
    int cid;

    while (success_count < n) {
        // poll
        ne = ibv_poll_cq(cq, num_entries, wc_array);

        TEST_N(ne, "Unable to poll from CQ");
        // check what was polled
        for (i = 0; i < ne; i++) {
            wr_id = wc_array[i].wr_id;
            // split wr_id into relevant fields

            ret = handle_work_completion(&wc_array[i]);
            if (ret == WC_SUCCESS) {
                if (WRID_GET_SSN(wr_id) == round_nb) {
                    success_count++;
                    cid = WRID_GET_CONN(wr_id);
                    g_ctx.completed_ops[cid] = round_nb;
                }

            } else if (ret == WC_EXPECTED_ERROR) {
                // TODO handle the error
            } else { // unexpected error
                die("Unexpected error while polling");
            }
        }
    }

    return 0;
}

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
        if ( g_ctx.log->firstUndecidedOffset <= g_ctx.qps[i].log_copy->firstUndecidedOffset) {
            nb_to_wait--;
            continue;
        }
        WRID_SET_CONN(wrid, i);
        local_address = log_get_local_slot(g_ctx.log, g_ctx.qps[i].log_copy->firstUndecidedOffset);
        // here we are assuming that the logs agree up to g_ctx.qps[i].log_copy->firstUndecidedOffset
        req_size = g_ctx.log->firstUndecidedOffset - g_ctx.qps[i].log_copy->firstUndecidedOffset;
        remote_addr = log_get_remote_address(g_ctx.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_write->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, wrid, false);

        //  update remote firstUndecidedOffset
        local_address = &g_ctx.log->firstUndecidedOffset;
        req_size = sizeof(g_ctx.log->firstUndecidedOffset);
        remote_addr = log_get_remote_address(g_ctx.log, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
        post_send(g_ctx.qps[i].qp, local_address, req_size, g_ctx.qps[i].mr_write->lkey, g_ctx.qps[i].remote_connection.rkey, remote_addr, IBV_WR_RDMA_WRITE, wrid, true);        

    }

    if (nb_to_wait > 0) {
        // array to store the work completions inside wait_for_n
        // we might want to place this in the global context later
        struct ibv_wc wc_array[g_ctx.num_clients];
        // currently we are polling at most num_clients WCs from the CQ at a time
        // we might want to change this number later
        wait_for_n(nb_to_wait, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array);        
    }
}

static bool
min_proposal_ok(uint64_t propNr) {

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.qps[i].log_copy->minProposal > propNr) {
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
                slot = log_get_local_slot(g_ctx.qps[i].log_copy, offset);
                local_address = slot;
                // Igor: problem: we can't know ahead of time how big the slot will be
                // Idea: initially copy a default size (large enough to include the length) and if not enough, copy again
                req_size = sizeof(log_slot_t) + size;
                break;
            case MIN_PROPOSAL:
                local_address = &g_ctx.qps[i].log_copy->minProposal;
                req_size = sizeof(g_ctx.qps[i].log_copy->minProposal);
                break;
        }   

        WRID_SET_CONN(wrid, i);
        remote_addr = log_get_remote_address(g_ctx.qps[i].log_copy, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
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
        wait_for_n(not_ok_slots, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array);

        not_ok_slots = 0;
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            if (g_ctx.completed_ops[i] == g_ctx.round_nb) {
                slot = log_get_local_slot(g_ctx.qps[i].log_copy, offset);
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
                    remote_addr = log_get_remote_address(g_ctx.qps[i].log_copy, local_address, (log_t*)g_ctx.qps[i].remote_connection.vaddr);
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
            req_size = log_slot_size(g_ctx.log, offset);
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
    log_slot_t* my_slot = log_get_local_slot(g_ctx.log, offset);
    max_acc_prop = my_slot->accProposal;
    freshest_value = &my_slot->accValue;

    // go only through "fresh" slots (those to which reads were completed in the preceding wait_for_n)
    log_slot_t* remote_slot;
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.completed_ops[i] == g_ctx.round_nb) {
            remote_slot = log_get_local_slot(g_ctx.qps[i].log_copy, offset);
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
    wait_for_n(majority, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array);
}

static void
wait_for_all() {
    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    wait_for_n(g_ctx.num_clients, g_ctx.round_nb, g_ctx.cq, g_ctx.num_clients, wc_array); 
}

static int
post_send(  struct ibv_qp* qp,
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
