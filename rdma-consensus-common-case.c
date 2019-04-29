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

#include <netdb.h>

#include <infiniband/verbs.h>

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
#define WRID_GET_TAG(wrid) ((wrid) & (1 << 8))
#define WRID_SET_TAG(wrid) (wrid) |= 1 << 8
#define WRID_UNSET_TAG(wrid) (wrid) &= ~(1 << 8)
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
static int num_clients;

struct app_context{
    struct ibv_context          *context;
    struct ibv_pd               *pd;
    struct ibv_mr               **mr;
    struct ibv_cq               *cq;
    struct ibv_qp               **qp;
    struct ibv_comp_channel     *ch;
    void                        *buf; // local memory to use with RDMA; will get rid of later, here just for testing
    unsigned                    size; // size of local buffer above
    int                         tx_depth;
    struct ibv_sge              sge_list; // will get rid of later, here just for testing
    struct ibv_send_wr          wr; // will get rid of later, here just for testing
};

struct ib_connection {
    int                 lid;
    int                 qpn;
    int                 psn;
    unsigned            rkey;
    unsigned long long  vaddr;
};

struct app_data {
    int                         port;
    int                         ib_port;
    unsigned                    size;
    int                         tx_depth;
    int                         *sockfd;
    char                        *servername;
    struct ib_connection        *local_connection;
    struct ib_connection        *remote_connection;
    struct ibv_device           *ib_dev;

};

struct app_context *ctx = NULL;

struct app_data          data = {
    .port                = 18515,
    .ib_port            = 1,
    .size               = 65536,
    .tx_depth           = 100,
    .servername         = NULL,
    .remote_connection  = NULL,
    .local_connection   = NULL,
    .ib_dev             = NULL
    
};


static int die(const char *reason);

static int tcp_client_connect(struct app_data *data);
static void tcp_server_listen(struct app_data *data);

static struct app_context *init_ctx(struct app_data *data);
static void destroy_ctx(struct app_context *ctx);

static void set_local_ib_connection(struct app_context *ctx, struct app_data *data);
static void print_ib_connection(char *conn_name, struct ib_connection *conn);

static int tcp_exch_ib_connection_info(struct app_data *data);

static int qp_change_state_init(struct ibv_qp *qp, struct app_data *data);
static int qp_change_state_rtr(struct ibv_qp *qp, struct app_data *data, int id);
static int qp_change_state_rts(struct ibv_qp *qp, struct app_data *data, int id);
static void rc_qp_destroy( struct ibv_qp *qp, struct ibv_cq *cq );

static void rdma_write(struct app_context *ctx, struct app_data *data, int id);
static void rdma_read(struct app_context *ctx, struct app_data *data, int id);
static int permission_switch(struct ibv_mr* old_mr, struct ibv_mr* new_mr, struct ibv_pd* pd, void* addr, size_t length, int old_new_flags, int new_new_flags);
static int wait_for_n(int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array);
static int handle_work_completion( struct ibv_wc *wc );
static int post_send(struct ibv_qp *qp, void* buf, uint32_t len, uint32_t lkey, uint32_t rkey, uint64_t remote_addr, enum ibv_wr_opcode opcode, uint64_t round_nb);

int main(int argc, char *argv[])
{

    if(argc == 2){
        num_clients = atoi(argv[1]);
        if (num_clients == 0) {
            num_clients = 1;
            data.servername = argv[1];
            printf("I am a client. The server is %s\n", data.servername);
        } else {
            printf("I am a server. I expect %d clients\n", num_clients);
        }
    } else { // (argc != 2)
        die("*Error* Usage: rdma <server/nb_clients>\n");
    }

    pid = getpid();

    if(!data.servername){
        // Print app parameters. This is basically from rdma_bw app. Most of them are not used atm
        printf("PID=%d | port=%d | ib_port=%d | size=%d | tx_depth=%d | sl=%d |\n",
            pid, data.port, data.ib_port, data.size, data.tx_depth, sl);
    }

    // Is later needed to create random number for psn
    srand48(pid * time(NULL));
    
    page_size = sysconf(_SC_PAGESIZE);
    
    TEST_Z(ctx = init_ctx(&data),
          "Could not create ctx, init_ctx");

    set_local_ib_connection(ctx, &data);
    
    data.sockfd = malloc(num_clients * sizeof(data.sockfd));
    if(data.servername) { // I am a client
        data.sockfd[0] = tcp_client_connect(&data);
    } else { // I am the server
        tcp_server_listen(&data);
    }

    TEST_NZ(tcp_exch_ib_connection_info(&data),
            "Could not exchange connection, tcp_exch_ib_connection");

    // Print IB-connection details
    for (int i = 0; i < num_clients; ++i) {
        print_ib_connection("Local  Connection", &data.local_connection[i]);
        print_ib_connection("Remote Connection", &data.remote_connection[i]);    
    }

    if(data.servername){ // I am a client
        qp_change_state_rtr(ctx->qp[0], &data, 0);
    } else { // I am the server
        for (int i = 0; i < num_clients; ++i) {
            qp_change_state_rts(ctx->qp[i], &data, i);
        }
    }    

    if(!data.servername){
        /* Server - RDMA WRITE */

        printf("Press ENTER to continue\n");
        getchar();
        // For now, the message to be written into the clients buffer can be edited here
        char *chPtr = ctx->buf;
        strcpy(chPtr,"Saluton Teewurst. UiUi");

        // printf("Client. Writing to Server\n");
        for (int i = 0; i < num_clients; ++i) {
            rdma_write(ctx, &data, i);
        }

        struct ibv_wc wc;
        //int n, uint64_t round_nb, struct ibv_cq *cq, int num_entries, struct ibv_wc *wc_array);
        wait_for_n(num_clients, 42, ctx->cq, 1, &wc);
        
        // printf("Server. Done with write. Reading from client\n");

        // sleep(1);
        // rdma_read(ctx, &data, 0);
        // printf("Printing local buffer: %s\n" ,chPtr);
        
    } else { // Client

        // permission_switch(ctx->mr[1], ctx->mr[0], ctx->pd, ctx->buf, ctx->size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
        // permission_switch(ctx->mr[0], ctx->mr[1], ctx->pd, ctx->buf, ctx->size*2, IBV_ACCESS_LOCAL_WRITE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        // ibv_rereg_mr(ctx->mr[0], IBV_REREG_MR_CHANGE_ACCESS, ctx->pd, ctx->buf, ctx->size * 2, IBV_ACCESS_LOCAL_WRITE);
        // ibv_rereg_mr(ctx->mr[1], IBV_REREG_MR_CHANGE_ACCESS, ctx->pd, ctx->buf, ctx->size * 2, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
       
        /* Server - Read local buffer */
        printf("Client. Reading Local-Buffer (Buffer that was registered with MR)\n");
        
        char *chPtr = (char *)data.local_connection[0].vaddr;
            
        while(1){
            if(strlen(chPtr) > 0){
                break;
            }
        }

        printf("Printing local buffer: %s\n" ,chPtr);
        
        printf("Press ENTER to continue\n");
        getchar();
    }
    
    printf("Destroying IB context\n");
    destroy_ctx(ctx);
    
    printf("Closing socket\n");
    for (int i = 0; i < num_clients; ++i) {
        close(data.sockfd[i]);
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
static int tcp_client_connect(struct app_data *data)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family        = AF_UNSPEC,
        .ai_socktype    = SOCK_STREAM
    };

    char *service;
    int sockfd = -1;

    TEST_N(asprintf(&service, "%d", data->port),
            "Error writing port-number to port-string");

    TEST_N(getaddrinfo(data->servername, service, &hints, &res),
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
static void tcp_server_listen(struct app_data *data) {
    struct addrinfo *res;
    struct addrinfo hints = {
        .ai_flags        = AI_PASSIVE,
        .ai_family        = AF_UNSPEC,
        .ai_socktype    = SOCK_STREAM    
    };

    char *service;
    int sockfd = -1;
    int n;

    TEST_N(asprintf(&service, "%d", data->port),
            "Error writing port-number to port-string");

    TEST_N(n = getaddrinfo(NULL, service, &hints, &res),
            "getaddrinfo threw error");

    TEST_N(sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol),
                "Could not create server socket");
    
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

    TEST_N(bind(sockfd,res->ai_addr, res->ai_addrlen),
            "Could not bind addr to socket"); 
    
    listen(sockfd, 1);

    for (int i = 0; i < num_clients; ++i) {
        TEST_N(data->sockfd[i] = accept(sockfd, NULL, 0),
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
static struct app_context *init_ctx(struct app_data *data)
{
    struct app_context *ctx;

    ctx = malloc(sizeof *ctx);
    memset(ctx, 0, sizeof *ctx);
    
    ctx->size = data->size;
    ctx->tx_depth = data->tx_depth;
    
    TEST_NZ(posix_memalign(&ctx->buf, page_size, ctx->size * 2),
                "could not allocate working buffer ctx->buf");

    memset(ctx->buf, 0, ctx->size * 2);

    struct ibv_device **dev_list;

    TEST_Z(dev_list = ibv_get_device_list(NULL),
            "No IB-device available. get_device_list returned NULL");

    TEST_Z(data->ib_dev = dev_list[0],
            "IB-device could not be assigned. Maybe dev_list array is empty");

    TEST_Z(ctx->context = ibv_open_device(data->ib_dev),
            "Could not create context, ibv_open_device");
    
    TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context),
        "Could not allocate protection domain, ibv_alloc_pd");

    /* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
     */

    
    TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context),
            "Could not create completion channel, ibv_create_comp_channel");

    ctx->qp  = malloc(num_clients * sizeof(struct ibv_qp*));
    ctx->mr  = malloc(num_clients * sizeof(struct ibv_mr*));
    TEST_Z(ctx->cq = ibv_create_cq(ctx->context,ctx->tx_depth, ctx, ctx->ch, 0),
                "Could not create completion queue, ibv_create_cq"); 

    for (int i = num_clients; i >= 0; i--) {
        TEST_Z(ctx->mr[i] = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size * 2, 
                        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE),
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

        TEST_Z(ctx->qp[i] = ibv_create_qp(ctx->pd, &qp_init_attr),
                "Could not create queue pair, ibv_create_qp");    
        
        qp_change_state_init(ctx->qp[i], data);        
    }
    
    return ctx;
}

static void destroy_ctx(struct app_context *ctx){
        
    for (int i = 0; i < num_clients; i++) {
        rc_qp_destroy( ctx->qp[i], ctx->cq );
    }
        
    TEST_NZ(ibv_destroy_cq(ctx->cq),
            "Could not destroy completion queue, ibv_destroy_cq");

    TEST_NZ(ibv_destroy_comp_channel(ctx->ch),
        "Could not destory completion channel, ibv_destroy_comp_channel");

    for (int i = 0; i < num_clients; ++i) {
        TEST_NZ(ibv_dereg_mr(ctx->mr[i]),
                "Could not de-register memory region, ibv_dereg_mr");
    }

    TEST_NZ(ibv_dealloc_pd(ctx->pd),
            "Could not deallocate protection domain, ibv_dealloc_pd");    
    
    free(ctx->buf);
    free(ctx);
    
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
static void set_local_ib_connection(struct app_context *ctx, struct app_data *data){

    // First get local lid
    struct ibv_port_attr attr;
    TEST_NZ(ibv_query_port(ctx->context,data->ib_port,&attr),
        "Could not get port attributes, ibv_query_port");

    data->local_connection = malloc(num_clients * sizeof(struct ib_connection));

    for (int i = 0; i < num_clients; ++i) {
        data->local_connection[i].qpn = ctx->qp[i]->qp_num;
        data->local_connection[i].rkey = ctx->mr[i]->rkey;
        data->local_connection[i].lid = attr.lid;
        data->local_connection[i].psn = lrand48() & 0xffffff;
        data->local_connection[i].vaddr = (uintptr_t)ctx->buf + ctx->size;
    }

}

static void print_ib_connection(char *conn_name, struct ib_connection *conn){
    
    printf("%s: LID %#04x, QPN %#06x, PSN %#06x RKey %#08x VAddr %#016Lx\n", 
            conn_name, conn->lid, conn->qpn, conn->psn, conn->rkey, conn->vaddr);

}

static int tcp_exch_ib_connection_info(struct app_data *data){

    char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
    int parsed;

    struct ib_connection *local;

    TEST_Z(data->remote_connection = malloc(num_clients * sizeof(struct ib_connection)),
        "Could not allocate memory for remote_connection connection");
    memset(data->remote_connection, 0, num_clients * sizeof(struct ib_connection));
    
    for (int i = 0; i < num_clients; ++i) {
        local = &data->local_connection[i]; 
        sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", 
                local->lid, local->qpn, local->psn, local->rkey, local->vaddr);
        if(write(data->sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not send connection_details to peer");
            return -1;
        }    

        if(read(data->sockfd[i], msg, sizeof msg) != sizeof msg){
            perror("Could not receive connection_details to peer");
            return -1;
        }
        struct ib_connection *remote = &data->remote_connection[i];
        parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", 
                            &remote->lid, &remote->qpn, &remote->psn, &remote->rkey, &remote->vaddr);
        
        if(parsed != 5){
            fprintf(stderr, "Could not parse message from peer");
            free(data->remote_connection);
        }
    }

    return 0;
}

/*
 *  qp_change_state_init
 * **********************
 *    Changes Queue Pair status to INIT
 */
static int qp_change_state_init(struct ibv_qp *qp, struct app_data *data){
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state            = IBV_QPS_INIT;
    attr->pkey_index          = 0;
    attr->port_num            = data->ib_port;
    attr->qp_access_flags     = IBV_ACCESS_REMOTE_WRITE;

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
static int qp_change_state_rtr(struct ibv_qp *qp, struct app_data *data, int id){
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTR;
    attr->path_mtu              = IBV_MTU_2048;
    attr->dest_qp_num           = data->remote_connection[id].qpn;
    attr->rq_psn                = data->remote_connection[id].psn;
    attr->max_dest_rd_atomic    = 1;
    attr->min_rnr_timer         = 12;
    attr->ah_attr.is_global     = 0;
    attr->ah_attr.dlid          = data->remote_connection[id].lid;
    attr->ah_attr.sl            = sl;
    attr->ah_attr.src_path_bits = 0;
    attr->ah_attr.port_num      = data->ib_port;

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
static int qp_change_state_rts(struct ibv_qp *qp, struct app_data *data, int id){

    qp_change_state_rtr(qp, data, id); 
    
    struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTS;
    attr->timeout               = 14;
    attr->retry_cnt             = 7;
    attr->rnr_retry             = 7;    /* infinite retry */
    attr->sq_psn                = data->local_connection[id].psn;
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
static void rdma_write(struct app_context *ctx, struct app_data *data, int id){

    post_send(ctx->qp[id], ctx->buf, ctx->size, ctx->mr[id]->lkey, data->remote_connection[id].rkey, data->remote_connection[id].vaddr, IBV_WR_RDMA_WRITE, 42);

}    


/*
 *  rdma_write
 * **********************
 *  Writes 'ctx-buf' into buffer of peer
 */
static void rdma_read(struct app_context *ctx, struct app_data *data, int id){
    
    ctx->sge_list.addr      = (uintptr_t)ctx->buf;
    ctx->sge_list.length    = ctx->size;
    ctx->sge_list.lkey      = ctx->mr[id]->lkey;

    ctx->wr.wr.rdma.remote_addr = data->remote_connection[id].vaddr;
    ctx->wr.wr.rdma.rkey        = data->remote_connection[id].rkey;
    ctx->wr.wr_id       = RDMA_WRID;
    ctx->wr.sg_list     = &ctx->sge_list;
    ctx->wr.num_sge     = 1;
    ctx->wr.opcode      = IBV_WR_RDMA_READ;
    ctx->wr.send_flags  = IBV_SEND_SIGNALED;
    ctx->wr.next        = NULL;

    struct ibv_send_wr *bad_wr;

    int rc = ibv_post_send(ctx->qp[id],&ctx->wr,&bad_wr);

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

    // Conrols if message was competely sent. But fails if client destroys his context to early. This would have to
    // be timed by the server telling the client that the rdma_write has been completed.
    int ne;
    struct ibv_wc wc;

    do {
        ne = ibv_poll_cq(ctx->cq,1,&wc);
    } while(ne == 0);

    if (ne < 0) {
        fprintf(stderr, "%s: poll CQ failed %d\n",
            __func__, ne);
    }

    if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "%d:%s: Completion with error at %s:\n",
                pid, __func__, data->servername ? "client" : "server");
            fprintf(stderr, "%d:%s: Failed status %d: wr_id %d\n",
                pid, __func__, wc.status, (int) wc.wr_id);
        }

    if (wc.status == IBV_WC_SUCCESS) {
        printf("wrid: %i successfull\n",(int)wc.wr_id);
        printf("%i bytes transfered\n",(int)wc.byte_len);
    }

}

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
                }
            } else if (ret == WC_EXPECTED_ERROR) {
                // TODO handle the error
            } else { // unexpected error
                die("Unexpeted error while polling");
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

// static void
// outer_loop(log_t *log) {
//     uint64_t propNr;
//     while (true) {
//         // wait until I am leader
//         // get permissions
//         // bring followers up to date with me
//         propNr = 1; // choose number higher than any proposal number seen before
//         inner_loop(log);
//     }
// }

// static void
// inner_loop(log_t *log) {
//     uint64_t index;
//     uint64_t v;

//     bool needPreparePhase = true;

//     while (true) {
//         index = log->firstUndecidedIndex;
//         if (needPreparePhase) {
//             // write propNr into minProposal at a majority of logs // if fails, goto outerLoop
//             // read slot at position "index" from a majority of logs // if fails, abort
//             if (none of the slots read have an accepted value) {
//                 needPreparePhase = false;
//                 v = myValue;
//             } else {
//                 v = value with highest accepted proposal among those read
//             }
//         }
//         // write v, propNr into slot at position "index" at a majority of logs // if fails, goto outerLoop
//         log->firstUndecidedIndex += 1    
//     }
// }

static int
write_log_slot(log_t* log, size_t index) {
    log_slot_t* slot = get_slot(log, index);

    slot->accValue = v;
    slot->accProposal = propNr;

    // post sends to everyone
    rdma_write_to_all(slot);

    // wait_for_majority
    wait_for_n();
}

static void
rdma_write_to_all(log_slot_t* slot) {
    for (int i = 0; i < num_clients; ++i) {
        post_send(ctx->qp[i], slot, sizeof(log_slot_t), ctx->mr[i]->lkey, data->remote_connection[i].rkey, data->remote_connection[i].vaddr, IBV_WR_RDMA_WRITE, 42);
    }
}

static int
post_send(  struct ibv_qp* qp,
            void* buf,
            uint32_t len,
            uint32_t lkey,
            uint32_t rkey,
            uint64_t remote_addr,
            enum ibv_wr_opcode opcode,
            uint64_t round_nb   ) {

    struct ibv_sge sg;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr;

    memset(&sg, 0, sizeof(sg));
    sg.addr   = (uint64_t)buf;
    sg.length = len;
    sg.lkey   = lkey;    

    memset(&wr, 0, sizeof(wr));
    WRID_SET_SSN(wr.wr_id, round_nb);
    wr.sg_list    = &sg;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
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
