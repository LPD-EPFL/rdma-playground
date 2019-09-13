#include "utils.h"

struct global_context
create_ctx() {
    struct global_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port               = 18515;
    ctx.ib_port            = 1;
    ctx.tx_depth           = 100;
    return ctx;
}