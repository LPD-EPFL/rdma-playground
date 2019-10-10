#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void consensus_setup();
void consensus_propose_test1();
void consensus_propose_test2();
void consensus_propose_test3();
void consensus_shutdown();
void consensus_start_leader_election();
void consensus_stop_leader_election();
bool consensus_propose(uint8_t *buf, size_t len);
