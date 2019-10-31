#include "consensus-protocol.h"

extern int leader;

struct global_context g_ctx;
extern struct global_context le_ctx;

bool need_prepare_phase = true;
bool need_init = true;
uint64_t g_prop_nr = 0;
value_t *v = NULL;

bool propose(uint8_t *buf, size_t len) {
    int rc;
    bool done = false;

    if (g_prop_nr == 0) {
        g_prop_nr = g_ctx.my_index + g_ctx.num_clients + 1;
    }

    // printf("Proposing %lu \n", *(uint64_t*)buf);
    while (!done) {
        if (leader != g_ctx.my_index) {
            return false;
        }

        if (need_init) {
            // rdma_ask_permission(le_ctx.buf.le_data, le_ctx.my_index, true);
            // // should always succeed

            rc = update_followers();
            if (rc) continue;

            need_init = false;
        }

        done = propose_inner(buf, len);
    }

    return true;
}

bool propose_inner(uint8_t *buf, size_t len) {
    int rc;
    bool inner_done = false;
    uint64_t offset = 0;

    value_t *freshVal = NULL;

    while (!inner_done) {
        offset = g_ctx.buf.log->firstUndecidedOffset;
        // printf("Offset is %lu \n", offset);
        if (need_prepare_phase) {
            rc = read_min_proposals();  // should always succeed
            if (rc) {
                need_init = true;
                return false;
            }

            if (!min_proposal_ok()) {  // check if any of the read minProposals
                                       // are larger than our g_prop_nr
                g_prop_nr +=
                    g_ctx.num_clients + 1;  // increment proposal number
                continue;
            }

            write_min_proposal(g_ctx.buf.log);
            // issue the next instruction without waiting for completions

            // read slot at position "offset" from a majority of logs
            copy_remote_logs(offset, SLOT, DEFAULT_VALUE_SIZE);

            // value with highest accepted proposal among those read
            freshVal = freshest_accepted_value(offset);
        }

        if (freshVal != NULL && freshVal->len != 0) {
            // printf("Found accepted value: %lu\n", *(uint64_t*)freshVal->val);
            v = freshVal;
        } else {
            need_prepare_phase = false;
            // adopt my value
            // printf("About to adopt my value\n");
            v = new_value(buf, len);

        }

        // write v into slot at position "offset" at a majority of logs // if
        // fails, restart from permissions
        rc = write_log_slot(g_ctx.buf.log, offset, v);
        if (rc) {
            need_init = true;
            need_prepare_phase = true;
            return false;
        }

        if (memcmp(v->val, buf, len) == 0) { // I managed to replicate my
        // value printf("Inner propose is done\n");
            inner_done = true;
            free_value(v);
        } else {
            printf("Inner propose is not done\n");
        }
        // increment the firstUndecidedOffset
        log_increment_fuo(g_ctx.buf.log);
    }

    return true;
}

int update_followers() {
    void *local_address;
    uint64_t remote_addr;
    size_t req_size;
    uint64_t wrid = 0;
    int rc = 0;

    int nb_to_wait = (g_ctx.num_clients / 2) + 1;

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        //  copy all or a part of the remote log
        //  overwrite remote log from their firstUn.. to my firstUn...
        if (g_ctx.buf.log->firstUndecidedOffset <=
            g_ctx.qps[i].buf_copy.log->firstUndecidedOffset) {
            nb_to_wait--;
            continue;
        }
        WRID_SET_CONN(wrid, i);
        local_address = log_get_local_slot(
            g_ctx.buf.log, g_ctx.qps[i].buf_copy.log->firstUndecidedOffset);
        // here we are assuming that the logs agree up to
        // g_ctx.qps[i].buf_copy.log->firstUndecidedOffset
        req_size = g_ctx.buf.log->firstUndecidedOffset -
                   g_ctx.qps[i].buf_copy.log->firstUndecidedOffset;
        remote_addr = log_get_remote_address(
            g_ctx.buf.log, local_address,
            (log_t *)g_ctx.qps[i].rc_remote_connection.vaddr);
        post_send(g_ctx.qps[i].rc_qp, local_address, req_size,
                  g_ctx.qps[i].mr_write->lkey,
                  g_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_WRITE, wrid, false);

        //  update remote firstUndecidedOffset
        local_address = &g_ctx.buf.log->firstUndecidedOffset;
        req_size = sizeof(g_ctx.buf.log->firstUndecidedOffset);
        remote_addr = log_get_remote_address(
            g_ctx.buf.log, local_address,
            (log_t *)g_ctx.qps[i].rc_remote_connection.vaddr);
        post_send(g_ctx.qps[i].rc_qp, local_address, req_size,
                  g_ctx.qps[i].mr_write->lkey,
                  g_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_WRITE, wrid, true);
    }

    if (nb_to_wait > 0) {
        // array to store the work completions inside wait_for_n
        // we might want to place this in the global context later
        struct ibv_wc wc_array[g_ctx.num_clients];
        // currently we are polling at most num_clients WCs from the CQ at a
        // time we might want to change this number later
        rc = wait_for_n(nb_to_wait, g_ctx.round_nb, &g_ctx, g_ctx.num_clients,
                        wc_array, g_ctx.completed_ops);
    }

    return rc;
}

bool min_proposal_ok() {
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        if (g_ctx.qps[i].buf_copy.log->minProposal > g_prop_nr) {
            return false;
        }
    }

    return true;
}

int write_log_slot(log_t *log, uint64_t offset, value_t *value) {
    UNUSED(value);

    // printf("Value length %lu - last byte %u\n", value->len,
    // value->val[value->len-1]); if (value->len <= 8) {
    //     printf("Write log slot uint64 %lu, %lu\n", offset,
    //     *(uint64_t*)value->val); log_write_local_slot_uint64(log, offset,
    //     g_prop_nr, *(uint64_t*)value->val);
    // } else {
    //     printf("Write log slot string %lu, %s\n", offset, value->val);
    //     log_write_local_slot_string(log, offset, g_prop_nr,
    //     (char*)value->val);
    // }

    // log_write_local_slot_uint64(log, offset, g_prop_nr, 42);
    log_write_local_slot(log, offset, g_prop_nr, value);

    // post sends to everyone
    bool signaled = false;
#ifdef CONSENSUS_SIGNAL
    if (g_ctx.round_nb % BATCH_SIZE == 0) {
        signaled = true;
    }
#else
    signaled = true;
#endif

    rdma_write_to_all(log, offset, SLOT, signaled);

    if (signaled) {
        return wait_for_majority();
    }

    return 0;
}

void write_min_proposal(log_t *log) {
    log->minProposal = g_prop_nr;

    rdma_write_to_all(log, 0, MIN_PROPOSAL,
                      false);  // offset is ignored for MIN_PROPOSAL
}

int read_min_proposals() {
    copy_remote_logs(0, MIN_PROPOSAL,
                     0);  // size and offset are ignored for MIN_PROPOSAL
    return wait_for_majority();
}

void copy_remote_logs(uint64_t offset, write_location_t type, uint64_t size) {
    void *local_address = NULL;
    uint64_t remote_addr = 0;
    size_t req_size = 0;
    log_slot_t *slot = NULL;
    uint64_t wrid = 0;

    g_ctx.round_nb++;
    WRID_SET_SSN(wrid, g_ctx.round_nb);

    for (int i = 0; i < g_ctx.num_clients; ++i) {
        switch (type) {
            case SLOT:
                slot = log_get_local_slot(g_ctx.qps[i].buf_copy.log, offset);
                local_address = slot;
                // Igor: problem: we can't know ahead of time how big the slot
                // will be Idea: initially copy a default size (large enough to
                // include the length) and if not enough, copy again
                req_size = sizeof(log_slot_t) + size;
                break;
            case MIN_PROPOSAL:
                local_address = &g_ctx.qps[i].buf_copy.log->minProposal;
                req_size = sizeof(g_ctx.qps[i].buf_copy.log->minProposal);
                break;
        }

        WRID_SET_CONN(wrid, i);
        remote_addr = log_get_remote_address(
            g_ctx.qps[i].buf_copy.log, local_address,
            (log_t *)g_ctx.qps[i].rc_remote_connection.vaddr);
        post_send(g_ctx.qps[i].rc_qp, local_address, req_size,
                  g_ctx.qps[i].mr_read->lkey,
                  g_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_READ, wrid, true);
    }

    if (type != SLOT) return;

    // for each entry that was seen as completed by the most recent wait_for_n
    // check length and, if necessary, re-issue read
    // wait for "the right number" complete
    // re-check and potentially start over
    int majority = (g_ctx.num_clients / 2) + 1;
    int not_ok_slots = majority;
    struct ibv_wc wc_array[g_ctx.num_clients];
    uint64_t correct_sizes[g_ctx.num_clients];
    for (int i = 0; i < g_ctx.num_clients; ++i) {
        correct_sizes[i] = size;
    }

    while (not_ok_slots > 0) {
        wait_for_n(not_ok_slots, g_ctx.round_nb, &g_ctx, g_ctx.num_clients,
                   wc_array, g_ctx.completed_ops);

        not_ok_slots = 0;
        for (int i = 0; i < g_ctx.num_clients; ++i) {
            if (g_ctx.completed_ops[i] == g_ctx.round_nb) {
                slot = log_get_local_slot(g_ctx.qps[i].buf_copy.log, offset);
                if (slot->accValue.len > correct_sizes[i]) {
                    not_ok_slots++;
                    // increase length
                    // re-issue the copy for this specific slot
                    local_address = slot;
                    // Igor: problem: we can't know ahead of time how big the
                    // slot will be Idea: initially copy a default size (large
                    // enough to include the length) and if not enough, copy
                    // again
                    req_size = sizeof(log_slot_t) + slot->accValue.len;
                    correct_sizes[i] =
                        slot->accValue
                            .len;  // so that on the next loop iteration, we
                                   // compare against the right size
                    WRID_SET_CONN(wrid, i);
                    remote_addr = log_get_remote_address(
                        g_ctx.qps[i].buf_copy.log, local_address,
                        (log_t *)g_ctx.qps[i].rc_remote_connection.vaddr);
                    post_send(g_ctx.qps[i].rc_qp, local_address, req_size,
                              g_ctx.qps[i].mr_read->lkey,
                              g_ctx.qps[i].rc_remote_connection.rkey, remote_addr,
                              IBV_WR_RDMA_READ, wrid, true);
                }
            }
        }
    }
}

void rdma_write_to_all(log_t *log, uint64_t offset, write_location_t type,
                       bool signaled) {
    void *local_address = NULL;
    uint64_t remote_addr = 0;
    size_t req_size = 0;
    uint64_t wrid = 0;

    switch (type) {
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
        remote_addr = log_get_remote_address(
            log, local_address,
            ((log_t *)g_ctx.qps[i].uc_remote_connection.vaddr));
        post_send(g_ctx.qps[i].uc_qp, local_address, req_size,
                  g_ctx.qps[i].mr_write->lkey,
                  g_ctx.qps[i].uc_remote_connection.rkey, remote_addr,
                  IBV_WR_RDMA_WRITE, wrid, signaled);
    }
}

// Igor - potential problem: we only look at fresh items, but copy_remote_logs
// might have cleared and overwritten the completed_ops array several times, so
// we cannot know which are the fresh items Can solve this in wait_for_n: don't
// clear completed_ops, rather update it with the most recent round_nb for which
// we have reveiced a
value_t *freshest_accepted_value(uint64_t offset) {
    uint64_t max_acc_prop;
    value_t *freshest_value;

    // start with my accepted proposal and value for the given offset
    log_slot_t *my_slot = log_get_local_slot(g_ctx.buf.log, offset);
    max_acc_prop = my_slot->accProposal;
    freshest_value = &my_slot->accValue;

    // go only through "fresh" slots (those to which reads were completed in the
    // preceding wait_for_n)
    log_slot_t *remote_slot;
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

int wait_for_majority() {
    int majority = ((g_ctx.num_clients + 1) / 2);

    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    return wait_for_n(majority, g_ctx.round_nb, &g_ctx, g_ctx.num_clients,
                      wc_array, g_ctx.completed_ops);
}

int wait_for_all() {
    // array to store the work completions inside wait_for_n
    // we might want to place this in the global context later
    struct ibv_wc wc_array[g_ctx.num_clients];
    // currently we are polling at most num_clients WCs from the CQ at a time
    // we might want to change this number later
    return wait_for_n(g_ctx.num_clients, g_ctx.round_nb, &g_ctx,
                      g_ctx.num_clients, wc_array, g_ctx.completed_ops);
}
