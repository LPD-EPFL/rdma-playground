// Based on rdma_bw.c program

#include "rdma-consensus.h"

extern struct global_context g_ctx;
extern struct global_context le_ctx;

void count_lines(char* filename, struct global_context *ctx) {
    
    FILE * fp;
    char * line = (char*)malloc(NI_MAXHOST * sizeof(char));
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

void parse_config(char* filename, struct global_context *ctx) {
    struct ifaddrs *ifaddr;
    

    TEST_NZ(getifaddrs(&ifaddr), "getifaddrs");

    /* Walk through linked list, maintaining head pointer so we
      can free list later */


    FILE * fp;
    char * line = (char*)malloc(NI_MAXHOST * sizeof(char));
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

bool
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
void tcp_client_connect()
{
    struct addrinfo *res, *t;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family        = AF_UNSPEC;
    hints.ai_socktype    = SOCK_STREAM;
    

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

        TEST_N(getaddrinfo(g_ctx.qps[i].ip_address, service, &hints, &res),
                "getaddrinfo threw error");

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
void tcp_server_listen() {
    struct addrinfo *res;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags        = AI_PASSIVE;
    hints.ai_family        = AF_UNSPEC;
    hints.ai_socktype    = SOCK_STREAM;   

    char *service;
    int sockfd = -1;
    int accepted_socket;
    int n;
    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(clientaddr);

    TEST_N(asprintf(&service, "%d", g_ctx.port),
            "Error writing port-number to port-string");


    TEST_N(n = getaddrinfo(NULL, service, &hints, &res),
            "getaddrinfo threw error");


    TEST_N(sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol),
                "Could not create server socket");
    
    int option = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    TEST_N(bind(sockfd,res->ai_addr, res->ai_addrlen),
            "Could not bind addr to socket"); 
    
    listen(sockfd, 1);


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

void init_buf_le(struct global_context* ctx) {
    // initializing the main buf in global_context
    ctx->buf.le_data = le_data_new(ctx->num_clients+1); // +1 because num_clients is the number of processes -1
    ctx->len = le_data_size(ctx->buf.le_data);


    // initializing the buf_copy's in each qp_context
    for (int i = 0; i < ctx->num_clients; i++) {
        ctx->qps[i].buf_copy.counter = (counter_t*)malloc(sizeof(counter_t));
        memset(ctx->qps[i].buf_copy.counter, 0, sizeof(counter_t));
    }
}

void init_buf_consensus(struct global_context* ctx) {

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
void init_ctx_common(struct global_context* ctx, bool is_le)
{
    
    // TEST_NZ(posix_memalign(&g_ctx.buf, page_size, g_ctx.size * 2),
                // "could not allocate working buffer g_ctx.buf");
    void *write_buf;
    void *read_buf;

    ctx->qps = (struct qp_context*)malloc(ctx->num_clients * sizeof(struct qp_context));
    memset(ctx->qps, 0, ctx->num_clients * sizeof(struct qp_context));    

    if (is_le) {
        init_buf_le(ctx);
    } else {
        init_buf_consensus(ctx);
    }

    ctx->completed_ops = (uint64_t*)malloc(ctx->num_clients * sizeof(uint64_t));
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

    if (!is_le) {
        ctx->cur_write_permission = 0; // initially only process 0 has write accesss
    }

    for (int i = 0; i < ctx->num_clients; i++) {
        if (is_le) {
            write_buf = (void*)ctx->buf.le_data;
            read_buf = (void*)ctx->qps[i].buf_copy.counter;
            // create the MR that we write from and others write into
            TEST_Z(ctx->qps[i].mr_write = ibv_reg_mr(ctx->pd, write_buf, ctx->len, 
                            IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE),
                        "Could not allocate mr_write, ibv_reg_mr. Do you have root access?");
        } else {
           write_buf = (void*)ctx->buf.log;
           read_buf = (void*)ctx->qps[i].buf_copy.log;

           // give read-write access to 0 and read-only access to everybody else (initially) 
           int flags = (i == 0) ? (IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE) 
                            : (IBV_ACCESS_REMOTE_READ  | IBV_ACCESS_LOCAL_WRITE);
            // create the MR that we write from and others write into
            TEST_Z(ctx->qps[i].mr_write = ibv_reg_mr(ctx->pd, write_buf, ctx->len, 
                            flags),
                        "Could not allocate mr_write, ibv_reg_mr. Do you have root access?");

        }

        // create the MR that we read into
        TEST_Z(ctx->qps[i].mr_read = ibv_reg_mr(ctx->pd, read_buf, ctx->len, 
                        IBV_ACCESS_LOCAL_WRITE),
                    "Could not allocate mr_read, ibv_reg_mr. Do you have root access?");

        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.send_cq = ctx->cq;
        qp_init_attr.recv_cq = ctx->cq;
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.cap.max_send_wr = ctx->tx_depth;
        qp_init_attr.cap.max_recv_wr = 1;
        qp_init_attr.cap.max_send_sge = 1;
        qp_init_attr.cap.max_recv_sge = 1;
        qp_init_attr.cap.max_inline_data = 0;

        TEST_Z(ctx->qps[i].qp = ibv_create_qp(ctx->pd, &qp_init_attr),
                "Could not create queue pair, ibv_create_qp");    
        
        qp_change_state_init(&ctx->qps[i], ctx->ib_port);        
    }
}

void destroy_ctx(struct global_context* ctx, bool is_le){
        
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
        le_data_free(ctx->buf.le_data);
    } else {
        log_free(ctx->buf.log);
    }
    free(ctx->completed_ops);
    
}

