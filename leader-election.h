#ifndef LEADER_ELECTION_H
#define LEADER_ELECTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "utils.h"
#include "ibv_layer.h"
#include "rdma-consensus.h"

void spawn_leader_election_thread();
void* leader_election(void* arg);
void rdma_read_all_counters();
int decide_leader();
void rdma_ask_permission(le_data* le_data, uint64_t my_index, bool signaled);
void check_permission_requests();

void start_leader_election();
void stop_leader_election();
void shutdown_leader_election_thread();

#ifdef __cplusplus
}
#endif

#endif // LEADER_ELECTION_H