#pragma once

// time durations
#define SHORT_SLEEP_DURATION_NS 10 * 1000       // 10 us = 10 * 1'000 ns
#define LE_SLEEP_DURATION_NS 999 * 1000 * 1000  // 100 ms = 100 * 1'000'000 ns
#define LE_COUNTER_READ_PERIOD_SEC 0.1          // 100 ms

// IB setup
#define IP_PORT 18515
#define IB_PORT 1
#define MAX_SEND_WR 100
#define MAX_RECV_WR 1
#define MAX_SEND_SGE 1
#define MAX_RECV_SGE 1
#define MAX_INLINE_DATA 32
#define PKEY_INDEX 0
#define MAX_DEST_RD_ATOMIC 1
#define MIN_RNR_TIMER 12
#define AH_ATTR_IS_GLOBAL 0
#define AH_ATTR_SL 1
#define AH_ATTR_SRC_PATH_BITS 0
#define TIMEOUT 14
#define RETRY_CNT 7
#define RNR_RETRY 7
#define MAX_RD_ATOMIC 1
#define COMP_VECTOR 0

// for thread pinning
#define MAIN_THREAD_CPU 0
#define LE_THREAD_CPU 2
#define PERM_THREAD_CPU 4

#define TEST_SIZE 10000
#define MEDIAN_SAMPLE_SIZE 29999 // increase log length to e.g. 10MB to test with more

#define CONFIG_FILE_NAME "./config"

#define BATCH_SIZE 64
