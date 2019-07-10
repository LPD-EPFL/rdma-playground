#ifndef CONSENSUS_PROTOCOL_H
#define CONSENSUS_PROTOCOL_H

#include "utils.h"
#include "ibv_layer.h"

void outer_loop(log_t *log);
void inner_loop(log_t *log, uint64_t propNr);
int write_log_slot(log_t* log, uint64_t offset, uint64_t propNr, value_t* value);
int write_min_proposal(log_t* log, uint64_t propNr);
int read_min_proposals();
int copy_remote_logs(uint64_t offset, write_location_t type, uint64_t size);
void update_followers();
value_t* freshest_accepted_value(uint64_t offset);
void wait_for_majority();
void wait_for_all(); 
void rdma_write_to_all(log_t* log, uint64_t offset, write_location_t type, bool signaled);
bool min_proposal_ok(uint64_t propNr);

#endif // CONSENSUS_PROTOCOL_H