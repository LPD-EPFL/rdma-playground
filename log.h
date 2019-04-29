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
    uint8_t slots[0];
};
typedef struct log log_t;

/* ================================================================== */


// Allocates and initializes a log
// len = the size in bytes to allocated for log slots
static log_t* 
log_new(size_t len) {
    log_t *log = (log_t*) malloc(sizeof(dare_log_t) + len);
    if (NULL == log) {
        return NULL;
    }

    memset(log, 0, sizeof(dare_log_t) + len);
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

// Returns a pointer to a slot at a specified index in a log
// log = the log
// index = the index of the slot to retrieve
// Return value: a pointer to the specified slot, or NULL of index is too large
static log_slot_t*
get_slot(log_t log, size_t index) {
    if (index * sizeof(log_slot_t) >= log->len) {
        return NULL;
    }
    return (log_t*)(log->slots + index);
}

#endif