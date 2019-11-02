#include <stdio.h>
#include <stdlib.h>

#include "propose_api.h"


int main() {
    consensus_setup(follower_nop_cb, NULL);

    // Used for barebones test of librdmaconsensus
#ifdef LEADER_CHANGE
    consensus_propose_leader_change();
#else
    consensus_propose_leader_median();
#endif

    // // Used for testing of initialization
    // consensus_propose_noop();


    printf("Press enter to exit\n");
    getchar();
    consensus_shutdown();

    return 0;
}
