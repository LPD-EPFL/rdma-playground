#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <unistd.h>
#include <errno.h>

#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#include "timers.h"

const int KEY_SIZE = 16;
const int VALUE_SIZE = 32;

#define UDS

#ifdef MEDIAN_SAMPLE_SIZE
#  undef MEDIAN_SAMPLE_SIZE
#endif
#define MEDIAN_SAMPLE_SIZE 500

#ifdef UDS
#  define SV_SOCK_PATH "/home/xygkis/rdma-playground/redis/redis.sock"
#endif

uint8_t data[1024];

TIMESTAMP_T write_timestamps[MEDIAN_SAMPLE_SIZE+1];
TIMESTAMP_T read_timestamps[MEDIAN_SAMPLE_SIZE+1];
uint64_t elapsed_times[MEDIAN_SAMPLE_SIZE];
uint64_t elapsed_times[MEDIAN_SAMPLE_SIZE];
uint64_t elapsed_times_ordered[MEDIAN_SAMPLE_SIZE];

int cmp_func(const void *a, const void *b) {
    return (int) ( *(uint64_t*)a - *(uint64_t*)b ); // ascending
}

void mkrndstr_ipa(int length, char *randomString) {
    static char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    if (length) {
        if (randomString) {
            int l = (int) (sizeof(charset) - 1);
            for (int n = 0; n < length; n++) {
                int key = rand() % l;
                randomString[n] = charset[key];
            }

            randomString[length] = '\0';
        }
    }
}

int stick_this_thread_to_core(int core_id) {
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   if (core_id < 0 || core_id >= num_cores)
      return EINVAL;

   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);

   pthread_t current_thread = pthread_self();
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

void *reader(void *ptr) {
    int sockfd = *(int *)ptr;

    char tmp[1024];

    for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
        int ret = read(sockfd, tmp, 1024);
        GET_TIMESTAMP(read_timestamps[i]);

        // tmp[ret] = 0;
        // printf("%s", tmp);
    }

    return NULL;
}

void *writer(void *ptr) {
    int sockfd = *(int *)ptr;
    char tmp[1024];

    char key[KEY_SIZE+1], value[VALUE_SIZE+1];

    for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
        mkrndstr_ipa(KEY_SIZE, key);
        mkrndstr_ipa(VALUE_SIZE, value);
        int len = sprintf(tmp, "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
            KEY_SIZE,
            key,
            VALUE_SIZE,
            value);

        // printf("Writing %s\n", tmp);

        GET_TIMESTAMP(write_timestamps[i]);
        int written = write(sockfd, tmp, len);
        if (written != len) {
            fprintf(stderr, "Failed to write everything\n");
        }

        usleep(1000 * 50);
    }

    return NULL;
}

int main() {
    TIMESTAMP_INIT

    // Connect to redis
    int sockfd;

    #ifdef UDS
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    #else
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    #endif

    if (sockfd == -1) {
        fprintf(stderr, "Socket creation failed\n");
        return EXIT_FAILURE;
    } else {
        printf("Socket successfully created\n");
    }

    #ifdef UDS
    struct sockaddr_un servaddr;
    #else
    struct sockaddr_in servaddr;
    #endif

    bzero(&servaddr, sizeof(servaddr));

    #ifdef UDS
    servaddr.sun_family = AF_UNIX;
    if (sizeof(servaddr.sun_path) - 1 < sizeof(SV_SOCK_PATH) ) {
        fprintf(stderr, "SV_SOCK_PATH is too long\n");
        return EXIT_FAILURE;
    }
    strncpy(servaddr.sun_path, SV_SOCK_PATH, sizeof(servaddr.sun_path) - 1);
    #else
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    servaddr.sin_port = htons(6379);
    #endif

    // connect the client socket to server socket
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
        fprintf(stderr, "Connection with the server failed\n");
        return EXIT_FAILURE;
    } else {
        printf("Connected to Redis\n");
    }


    // Spawn the reader/writer threads
    pthread_t reader_thread, writer_thread;
    int ret;
    ret = pthread_create(&reader_thread, NULL, reader, &sockfd);
    if (ret) {
        fprintf(stderr, "Error creating reader thread\n");
        return EXIT_FAILURE;
    }

    ret = pthread_create(&writer_thread, NULL, writer, &sockfd);
    if (ret) {
        fprintf(stderr, "Error creating writer thread\n");
        return EXIT_FAILURE;
    }

    pthread_join(reader_thread, NULL);
    pthread_join(writer_thread, NULL);

    // Processing of measurements
    for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
        elapsed_times[i] = ELAPSED_NSEC(write_timestamps[i], read_timestamps[i]);
        elapsed_times_ordered[i] = elapsed_times[i];

    }
    qsort(elapsed_times_ordered, MEDIAN_SAMPLE_SIZE, sizeof(uint64_t), cmp_func);

    uint64_t highest[3] = {elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-3],
                        elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-2],
                        elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-1]};
    uint64_t moments[3];
    for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
        for (int j = 0; j < 3; j++) {
            if (elapsed_times[i] == highest[j]) {
                moments[j] = i;
            }
        }
    }

    double average = 0;
    for (int i = 0; i < MEDIAN_SAMPLE_SIZE; i++) {
        average += (double) elapsed_times[i]/MEDIAN_SAMPLE_SIZE;
    }
    double median = elapsed_times_ordered[(int)(0.5 * MEDIAN_SAMPLE_SIZE)];
    double percentile_98 = elapsed_times_ordered[(int)(0.98 * MEDIAN_SAMPLE_SIZE)];
    double percentile_02 = elapsed_times_ordered[(int)(0.02 * MEDIAN_SAMPLE_SIZE)];

    printf("Sample size = %d\n", MEDIAN_SAMPLE_SIZE);
    printf("Average: %.2f\n", average);
    printf("Min = %lu ns\n", elapsed_times_ordered[0]);
    printf("02th percentile = %.2f ns\n", percentile_02);
    printf("Median = %.2f ns\n", median);
    printf("98th percentile = %.2f ns\n", percentile_98);
    printf("TOP 3 = %luth proposal - %lu ns, %luth proposal - %lu ns, %luth proposal - %lu ns\n",
            moments[0]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-3],
            moments[1]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-2],
            moments[2]+1, elapsed_times_ordered[MEDIAN_SAMPLE_SIZE-1]);
    printf("\n");


    return 0;
}