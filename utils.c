#include "utils.h"

struct global_context
create_ctx() {
    struct global_context ctx;
    ctx.ib_dev             = NULL;
    ctx.context            = NULL;
    ctx.pd                 = NULL;
    ctx.cq                 = NULL;
    ctx.ch                 = NULL;
    ctx.qps                = NULL;
    ctx.round_nb           = 0;
    ctx.num_clients        = 0;
    ctx.port               = 18515;
    ctx.ib_port            = 1;
    ctx.tx_depth           = 100;
    ctx.servername         = NULL;
    ctx.buf.counter        = NULL;
    ctx.len                = 0;
    ctx.completed_ops      = NULL;
    return ctx;
}