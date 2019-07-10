#ifndef LEADER_ELECTION_H
#define LEADER_ELECTION_H

#include "utils.h"
#include "ibv_layer.h"
#include "rdma-consensus.h"

void spawn_leader_election_thread();
void* leader_election(void* arg);
void rdma_read_all_counters();
int decide_leader();

#endif // LEADER_ELECTION_H