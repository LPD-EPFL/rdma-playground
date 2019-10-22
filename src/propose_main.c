#include <stdio.h>
#include <stdlib.h>

#include "propose_api.h"

int main() {
    consensus_setup();
//    consensus_propose_test2();
    consensus_propose_leader_median();
    getchar();
    consensus_shutdown();

    return 0;
}
