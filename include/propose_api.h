#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*follower_cb_t)(void *userdata, char *data, int data_len);

static void follower_nop_cb(void *userdata, char *data, int data_len) {
    (void)userdata;
    (void)data;
    (void)data_len;
}

void consensus_setup(follower_cb_t follower_cb, void *follower_cb_data);

void consensus_propose_noop();
void consensus_propose_test1();
void consensus_propose_test2();
void consensus_propose_test3();
void consensus_propose_leader_median();
void consensus_propose_test_rereg();
void consensus_shutdown();
void consensus_start_leader_election();
void consensus_stop_leader_election();
bool consensus_propose(uint8_t *buf, size_t len);
