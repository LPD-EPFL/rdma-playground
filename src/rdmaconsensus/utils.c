#include "utils.h"

struct global_context create_ctx() {
    struct global_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    return ctx;
}