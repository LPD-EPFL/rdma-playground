#include <stdio.h>
#include <stdlib.h>

#include "propose_api.h"

int main() {
        consensus_setup();
        consensus_propose_test2();
        getchar();
        consensus_shutdown();

    return 0;
}
