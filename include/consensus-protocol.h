#ifndef CONSENSUS_PROTOCOL_H
#define CONSENSUS_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "utils.h"
#include "ibv_layer.h"
#include "leader-election.h"

bool propose(uint8_t* buf, size_t len);
bool propose_inner(uint8_t* buf, size_t len);
int update_followers();
bool min_proposal_ok();
int write_log_slot(log_t* log, uint64_t offset, value_t* value);
void write_min_proposal(log_t* log);
int read_min_proposals();
void copy_remote_logs(uint64_t offset, write_location_t type, uint64_t size);
void rdma_write_to_all(log_t* log, uint64_t offset, write_location_t type, bool signaled);
value_t* freshest_accepted_value(uint64_t offset);
int wait_for_majority();
int wait_for_all(); 

#ifdef __cplusplus
}
#endif

#endif // CONSENSUS_PROTOCOL_H