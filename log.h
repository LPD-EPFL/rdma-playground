#ifndef LOG_H
#define LOG_H

#define DEFAULT_VALUE_SIZE 8 // value size if it is uint64_t

struct value_t {
    uint64_t len;
    uint8_t val[0];
};
typedef struct value_t value_t;

struct log_slot {
    uint64_t accProposal;
    value_t accValue;
};
typedef struct log_slot log_slot_t;

struct log {
    uint64_t minProposal;
    uint64_t firstUndecidedOffset;
    uint64_t len;
    uint8_t slots[0];
};
typedef struct log log_t;

/* ================================================================== */


// Allocates and initializes a log
// len = the size in bytes to allocate for slots
static log_t* 
log_new(size_t len) {
    log_t *log = (log_t*) malloc(sizeof(log_t) + len);
    if (NULL == log) {
        return NULL;
    }

    memset(log, 0, sizeof(log_t) + len);
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
    return (sizeof(log_t) + log->len);
}

// Returns a pointer to a slot at a specified offset in a log
// log = the log
// ofsset = the offset of the slot to retrieve
// Return value: a pointer to the specified slot, or NULL of offset is too large
static log_slot_t*
log_get_local_slot(log_t* log, uint64_t offset) {
    if (offset >= log->len) {
        return NULL;
    }
    return (log_slot_t*)(log->slots + offset);
}

// returns the total size (headers + data) of a log slot at a given offset
static uint64_t
log_slot_size(log_t* log, uint64_t offset) {
    return sizeof(log_slot_t) + log_get_local_slot(log, offset)->accValue.len;
}

// get remote address corresponding to given offset in local log
// Note: unlike other log methods, the offset here is wrt the beginning of the 
// entire log (not the beginning of the slots)
static uint64_t
log_get_remote_address(log_t* local_log, void* local_offset, log_t* remote_log) {
    return (uint64_t)remote_log + ((uint64_t)local_offset - (uint64_t)local_log);
}

// increments the firstUndecidedOffset of a log
static void
log_increment_fuo(log_t *log) {
    log->firstUndecidedOffset += log_slot_size(log, log->firstUndecidedOffset);
}

static void
log_write_local_slot_uint64(log_t* log, uint64_t offset, uint64_t propNr, uint64_t val) {
    log_slot_t *slot = log_get_local_slot(log, offset);

    slot->accProposal = propNr;
    slot->accValue.len = sizeof(uint64_t);
    *(uint64_t *)slot->accValue.val = val;
}

static void
log_write_local_slot_string(log_t* log, uint64_t offset, uint64_t propNr, char* val) {
    log_slot_t *slot = log_get_local_slot(log, offset);

    slot->accProposal = propNr;
    slot->accValue.len = strlen(val);
    strcpy((char*)slot->accValue.val, val);
}

static void
log_print(log_t* log) {
    printf("{ minProposal = %lu, firstUndecidedOffset = %lu, len = %lu, ", log->minProposal, log->firstUndecidedOffset, log->len);
    uint64_t offset = 0;
    log_slot_t *slot = log_get_local_slot(log, offset);
    while (slot->accValue.len != 0) {
        printf("[%lu, %lu] ", slot->accProposal, *(uint64_t*)slot->accValue.val);
        offset += log_slot_size(log, offset);
        slot = log_get_local_slot(log, offset);
    }
    printf("}\n");
}



#endif