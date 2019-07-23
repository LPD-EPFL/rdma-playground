#include "consensus-protocol.h"

extern atomic_int leader;

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

void
outer_loop(log_t *log) {
    uint64_t propNr;
    // while (true) {
        // wait until I am leader
        while (leader != g_ctx.my_index) {
            nanosleep((const struct timespec[]){{0, SHORT_SLEEP_DURATION_NS}}, NULL);
        }
        // get permissions
        // bring followers up to date with me
        update_followers();
        propNr = 1; // choose number higher than any proposal number seen before
        inner_loop(log, propNr);
    // }
}

void
inner_loop(log_t *log, uint64_t propNr) {
    uint64_t offset = 0;
    value_t* v;

    bool needPreparePhase = true;

    while (offset < 80000) {
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

void
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
        wait_for_n(nb_to_wait, g_ctx.round_nb, g_ctx, g_ctx.num_clients, wc_array, g_ctx.completed_ops);        
    }
}

bool
min_proposal_ok(uint64_t propNr) {

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.qps[i].buf_copy.log->minProposal > propNr) {
            return false;
        }
    }

    return true;
}

int
write_log_slot(log_t* log, uint64_t offset, uint64_t propNr, value_t* value) {

    if (value->len <= 8) {
        // printf("Write log slot %lu, %lu, %lu\n", offset, propNr, *(uint64_t*)value->val);
        log_write_local_slot_uint64(log, offset, propNr, *(uint64_t*)value->val);
    } else {
        // printf("Write log slot %lu, %lu, %s\n", offset, propNr, value->val);
        log_write_local_slot_string(log, offset, propNr, (char*)value->val);        
    }


    // post sends to everyone
    rdma_write_to_all(log, offset, SLOT, true);
    
}

int
write_min_proposal(log_t* log, uint64_t propNr) {
    log->minProposal = propNr;

    rdma_write_to_all(log, 0, MIN_PROPOSAL, false); // offset is ignored for MIN_PROPOSAL
}


int
read_min_proposals() {

    copy_remote_logs(0, MIN_PROPOSAL, 0); // size and offset are ignored for MIN_PROPOSAL
}

int
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
        wait_for_n(not_ok_slots, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array, g_ctx.completed_ops);

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

void
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
value_t*
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




void
wait_for_majority() {
    int majority = (g_ctx.num_clients/2) + 1;

    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    wait_for_n(majority, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array, g_ctx.completed_ops);
}

void
wait_for_all() {
    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    wait_for_n(g_ctx.num_clients, g_ctx.round_nb, &g_ctx, g_ctx.num_clients, wc_array, g_ctx.completed_ops); 
}

