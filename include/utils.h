#ifndef UTILS_H
#define UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <linux/if_link.h>
#include <netinet/in.h>

#include <infiniband/verbs.h>

#include "log.h"

#include "constants.h"
#include "registry.h"
#include "propose_api.h"

#define UNUSED(x) (void)x;

typedef enum {SLOT, MIN_PROPOSAL} write_location_t;

// if x is NON-ZERO, error is printed
#define TEST_NZ(x,y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x,y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x,y) do { if ((x)<0) die(y); } while (0)

// if x is NULL, error is printed
#define TEST_NULL(x,y) do { if ((x)==NULL) die(y); } while (0)

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

static int die(const char *reason){
    fprintf(stderr, "Err: %s - %s\n ", strerror(errno), reason);
    exit(EXIT_FAILURE);
    return -1;
}


struct ib_connection {
    int                 lid;
    int                 qpn;
    int                 psn;
    unsigned            rkey;
    unsigned long long  vaddr;
};

struct qp_context {
    struct ibv_qp               *rc_qp;
    struct ibv_qp               *uc_qp;
    // MR for reading from a remote log into my local copy of that log
    // Note: different mr_read MRs refer to different phyisical locations, and they have minimal permissions
    struct ibv_mr               *mr_read;
    // MR for writing from my local log to another node's log
    // Note: all mr_write MRs refer to the same physical location, but may have different permissions
    struct ibv_mr               *mr_write;

    struct ib_connection        rc_local_connection;
    struct ib_connection        rc_remote_connection;
    struct ib_connection        uc_local_connection;
    struct ib_connection        uc_remote_connection;
    // char                        *servername; // Igor: should we store this per-connection?
    // char                        ip_address[NI_MAXHOST];

    union {
        log_t *log;
        counter_t *counter;
    } buf_copy;
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
    char                        *servername;
    // char                        my_ip_address[NI_MAXHOST];
    int                         my_index; // my_index = i iff my_ip_address is the i-th in the config file

    union {
        log_t *log;
        le_data_t *le_data;
    } buf;

    int                         cur_write_permission; // index of process who currently has write permission on my memory
    size_t                      len; // length of buf in bytes (used when registering MRs)
    uint64_t                    *completed_ops;

    struct dory_registry *registry;
    void *follower_cb_data;
    follower_cb_t follower_cb;
};

static void
set_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_t thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset)) {
        fprintf(stderr, "Error setting cpu affinity\n");
    }
}

struct global_context create_ctx();

#ifdef __cplusplus
}
#endif

#endif // UTILS_H