#include <stdio.h>
#include <stdlib.h>

#include "propose_api.h"


int main() {
    consensus_setup(follower_nop_cb, NULL);

    // Used for barebones test of librdmaconsensus
    consensus_propose_leader_median();

    // // Used for testing of initialization
    // consensus_propose_noop();


    getchar();
    consensus_shutdown();

    return 0;
}
