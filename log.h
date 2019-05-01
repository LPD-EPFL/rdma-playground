#ifndef LOG_H
#define LOG_H

struct log_slot {
    uint64_t accProposal;
    uint64_t accValue;
};
typedef struct log_slot log_slot_t;

struct log {
    uint64_t minProposal;
    uint64_t firstUndecidedIndex;
    uint64_t len;
    log_slot_t slots[0];
};
typedef struct log log_t;

/* ================================================================== */


// Allocates and initializes a log
// len = the number of log slots to allocate
static log_t* 
log_new(size_t len) {
    log_t *log = (log_t*) malloc(sizeof(log_t) + len * sizeof(log_slot_t));
    if (NULL == log) {
        return NULL;
    }

    memset(log, 0, sizeof(log_t) + len * sizeof(log_slot_t));
    log->len = len;

    return log;
}

// Frees a log
// log = the log to free
static void 
log_free( log_t* log )
{
    if (NULL != log) {
        free(log);
        log = NULL;
    }
}

// Returns the size of a log in bytes
static size_t
log_size( log_t* log) {
    return (sizeof(log_t) + log->len * sizeof(log_slot_t));
}

// Returns a pointer to a slot at a specified index in a log
// log = the log
// index = the index of the slot to retrieve
// Return value: a pointer to the specified slot, or NULL of index is too large
static log_slot_t*
log_get_local_slot(log_t* log, size_t index) {
    if (index >= log->len) {
        return NULL;
    }
    return &log->slots[index];
}

// get remote address corresponding to local slot
static uint64_t
log_get_remote_address(log_t* local_log, log_slot_t* local_slot, log_t* remote_log) {
    return (uint64_t)remote_log + ((uint64_t)local_slot - (uint64_t)local_log);
}



static void
log_print(log_t* log) {
    printf("{ minProposal = %lu, firstUndecidedIndex = %lu, len = %lu, ", log->minProposal, log->firstUndecidedIndex, log->len);
    log_slot_t *slot;
    for (int i = 0; i < log->len; ++i) {
        slot = log_get_local_slot(log, i);
        printf("[%lu, %lu] ", slot->accProposal, slot->accValue);
    }
    printf("}\n");
}



#endif