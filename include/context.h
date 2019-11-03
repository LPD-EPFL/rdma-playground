// context that is shared by everyone
struct dev_context {
    struct ibv_device           *ib_dev;
    struct ibv_context          *context;
    struct ibv_pd               *pd;    
}

struct remote_qp {
    int             lid;
    int             qpn;
    int             psn;     
}

struct remote_mr {
    unsigned            rkey;
    unsigned long long  vaddr;   
}

struct qp_context {
    struct ibv_qp       *qp;   
    struct remote_qp    rem_qp;   
}

// context that is shared by a set of qps
struct qp_plane {
    struct ibv_cq               *cq;
    struct ibv_comp_channel     *ch;
    struct qp_context           *qps; // array   
}

struct mr_context {
    struct ibv_mr               *mr_read;
    struct ibv_mr               *mr_write;  
    struct remote_mr            rem_mr;  
}


struct mr_plane {
   struct mr_context    *mrs; // array
}

struct tx_progress {
    uint64_t round_nb;
    uint64_t *completed_ops;
}

struct global_context {
    struct dev_context      dev;
    
    struct qp_plane         uc_plane;
    struct qp_plane         rc_plane1;
    struct qp_plane         rc_plane2;

    struct mr_plane         log_plane;
    struct mr_plane         bg_plane;

    log_t                   *log;
    le_data_t               *le_data;

    int                     num_clients;

    log_t                   **log_copies;
    le_data_t*              **le_data_copies;

    struct tx_progress      fg_progress;
    struct tx_progress      bg_progress;

    struct dory_registry    *registry;
}