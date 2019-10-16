#include <stdio.h>
#include <stdlib.h>

#include "propose_api.h"


int main() {
    consensus_setup(follower_nop_cb, NULL);
    consensus_propose_noop();
    getchar();
    consensus_shutdown();

    return 0;
}
