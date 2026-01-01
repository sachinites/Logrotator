#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>

#define LOG_FILE_1 "var/log/ipstrc.log"
#define LOG_FILE_2 "var/log/pdtrc.log"
#define LOG_FILE_3 "var/log/ipmgr.log"
#define LOG_FILE_4 "var/log/inttrc.log"
#define MAX_LOG_SIZE 10240  // 10KB in bytes
#define NUM_THREADS 4
#define LOG_DIR_PATH "var/log"

// Thread data structure
typedef struct {
    int thread_id;
    int cpu_core;
    const char *log_file;
    const char *thread_name;
    void (*log_generator)(char*, size_t);
} thread_data_t;

// Log level types
const char *log_levels[] = {"INFO", "WARN", "ERROR", "DEBUG"};
const int num_log_levels = 4;

// Sample log messages for each service
const char *ipstrc_messages[] = {
    "Connection established from 192.168.1.100",
    "Packet received: size=%d bytes",
    "Connection timeout detected",
    "Routing table updated",
    "NAT translation added",
    "Firewall rule applied",
    "TCP handshake completed",
    "UDP datagram processed",
    "Network interface status changed",
    "IP address conflict detected"
};

const char *inttrc_messages[] = {
    "Connection established from 192.168.1.100",
    "Packet received: size=%d bytes",
    "Connection timeout detected",
    "Routing table updated",
    "NAT translation added",
    "Firewall rule applied",
    "TCP handshake completed",
    "UDP datagram processed",
    "Network interface status changed",
    "IP address conflict detected"
};

const char *pdtrc_messages[] = {
    "Protocol data unit received",
    "Session initiated with client",
    "Data transmission in progress",
    "Buffer overflow prevented",
    "Checksum validation passed",
    "Sequence number: %d",
    "Retransmission attempt %d",
    "Flow control activated",
    "Window size adjusted to %d",
    "Protocol version negotiated"
};

const char *ipmgr_messages[] = {
    "IP allocation request processed",
    "DHCP lease renewed",
    "Address pool utilization: %d%%",
    "Static IP assignment completed",
    "IP conflict resolution in progress",
    "Subnet mask updated",
    "Gateway configuration changed",
    "DNS server registered",
    "IP address released",
    "Network range expanded"
};


// Get current timestamp in ISO 8601 format
void get_timestamp(char *buffer, size_t size) {
    time_t now;
    struct tm *tm_info;
    
    time(&now);
    tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Generate a random log message for ipstrc
void generate_ipstrc_log(char *buffer, size_t size) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    int msg_idx = rand() % 10;
    int level_idx = rand() % num_log_levels;
    int random_val = rand() % 1000;
    
    snprintf(buffer, size, "[%s] [%s] ", timestamp, log_levels[level_idx]);
    
    char msg[256];
    snprintf(msg, sizeof(msg), ipstrc_messages[msg_idx], random_val);
    strncat(buffer, msg, size - strlen(buffer) - 1);
    strncat(buffer, "\n", size - strlen(buffer) - 1);
}

// Generate a random log message for pdtrc
void generate_pdtrc_log(char *buffer, size_t size) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    int msg_idx = rand() % 10;
    int level_idx = rand() % num_log_levels;
    int random_val = rand() % 100;
    
    snprintf(buffer, size, "[%s] [%s] ", timestamp, log_levels[level_idx]);
    
    char msg[256];
    snprintf(msg, sizeof(msg), pdtrc_messages[msg_idx], random_val);
    strncat(buffer, msg, size - strlen(buffer) - 1);
    strncat(buffer, "\n", size - strlen(buffer) - 1);
}

// Generate a random log message for ipmgr
void generate_ipmgr_log(char *buffer, size_t size) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    int msg_idx = rand() % 10;
    int level_idx = rand() % num_log_levels;
    int random_val = rand() % 100;
    
    snprintf(buffer, size, "[%s] [%s] ", timestamp, log_levels[level_idx]);
    
    char msg[256];
    snprintf(msg, sizeof(msg), ipmgr_messages[msg_idx], random_val);
    strncat(buffer, msg, size - strlen(buffer) - 1);
    strncat(buffer, "\n", size - strlen(buffer) - 1);
}

// Generate a random log message for ipmgr
void generate_inttrc_log(char *buffer, size_t size) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    int msg_idx = rand() % 10;
    int level_idx = rand() % num_log_levels;
    int random_val = rand() % 100;
    
    snprintf(buffer, size, "[%s] [%s] ", timestamp, log_levels[level_idx]);
    
    char msg[256];
    snprintf(msg, sizeof(msg), inttrc_messages[msg_idx], random_val);
    strncat(buffer, msg, size - strlen(buffer) - 1);
    strncat(buffer, "\n", size - strlen(buffer) - 1);
}

// Get file size in bytes
long get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return 0;  // File doesn't exist or error
}

// Write log to file, rename to .bak if size exceeds limit (thread-safe version)
int write_log(FILE **fp, thread_data_t *data, const char *log_message, long *old_size) {
    long current_size = get_file_size(data->log_file);

    if (current_size - *old_size >= MAX_LOG_SIZE) {
        fclose(*fp);

        // make a new unique .bak each time
        char bak_name[256];
        time_t now = time(NULL);
        
        snprintf(bak_name, sizeof(bak_name), "%s/%s.%ld.bak", 
            LOG_DIR_PATH, data->thread_name, now);

        rename(data->log_file, bak_name);

        *fp = fopen(data->log_file, "a"); // start new log
        if (*fp == NULL) {
            fprintf(stderr, "Error reopening %s: %s\n", data->log_file, strerror(errno));
            return -1;
        }
        *old_size = 0;
    }

    fprintf(*fp, "%s", log_message);
    fflush(*fp);
    return 0;
}


// Pin thread to a specific CPU core
int pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    pthread_t current_thread = pthread_self();
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    
    if (result != 0) {
        fprintf(stderr, "Error setting CPU affinity for core %d: %s\n", 
                core_id, strerror(result));
        return -1;
    }
    
    return 0;
}

// Logger thread function
void *logger_thread(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    char log_buffer[512];
    int log_counter = 0;
    long old_size = 0;
    
    // Pin this thread to its assigned CPU core
    if (pin_thread_to_core(data->cpu_core) == 0) {
        printf("[Thread %d] Successfully pinned to CPU core %d\n", 
               data->thread_id, data->cpu_core);
    }
    
    // Seed random number generator with thread-specific seed
    unsigned int seed = time(NULL) + data->thread_id;
    
    // Open log file
    FILE *fp = fopen(data->log_file, "a");
    if (fp == NULL) {
        fprintf(stderr, "[Thread %d] Error opening %s: %s\n", 
                data->thread_id, data->log_file, strerror(errno));
        return NULL;
    }
    
    printf("[Thread %d] Started logging to %s\n", data->thread_id, data->log_file);
    
    // Main logging loop
    while (1) {
        // Generate log message using thread-specific generator
        data->log_generator(log_buffer, sizeof(log_buffer));
        
        // Write log (thread-safe version)
        if (write_log(&fp, data, log_buffer, &old_size) == 0) {
            // Optionally print to console (can be disabled for pure stress test)
            if (log_counter % 100 == 0) {  // Print every 100th log to reduce console spam
                printf("[Thread %d][%d] %s: %s", 
                       data->thread_id, log_counter, data->thread_name, log_buffer);
            }
        }
        
        log_counter++;
        
        // Very short sleep for high stress - ~1000 logs per second per thread
        // Adjust this value to control stress level:
        // 1000 = ~1000 logs/sec, 10000 = ~100 logs/sec, 100000 = ~10 logs/sec
        usleep(1000);
        
        // Use thread-local random
        rand_r(&seed);
    }
    
    fclose(fp);
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc;  // Unused parameter
    (void)argv;  // Unused parameter
    
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    
    // Seed random number generator
    srand(time(NULL));
    
    printf("========================================\n");
    printf("  Multi-threaded Log Generator\n");
    printf("========================================\n");
    printf("Number of threads: %d\n", NUM_THREADS);
    printf("Writing logs to:\n");
    printf("  - %s\n", LOG_FILE_1);
    printf("  - %s\n", LOG_FILE_2);
    printf("  - %s\n", LOG_FILE_3);
    printf("  - %s\n", LOG_FILE_4);
    printf("Rate: ~1000 logs/sec per thread (4000 total)\n");
    printf("Max log size: %d bytes (rename to .bak when exceeded)\n", MAX_LOG_SIZE);
    printf("CPU Affinity: Each thread pinned to separate core\n");
    printf("Press Ctrl+C to stop\n");
    printf("========================================\n\n");
    
    // Configure thread data
    thread_data[0] = (thread_data_t){
        .thread_id = 0,
        .cpu_core = 0,
        .log_file = LOG_FILE_1,
        .thread_name = "ipstrc",
        .log_generator = generate_ipstrc_log
    };
    
    thread_data[1] = (thread_data_t){
        .thread_id = 1,
        .cpu_core = 1,
        .log_file = LOG_FILE_2,
        .thread_name = "pdtrc",
        .log_generator = generate_pdtrc_log
    };
    
    thread_data[2] = (thread_data_t){
        .thread_id = 2,
        .cpu_core = 2,
        .log_file = LOG_FILE_3,
        .thread_name = "ipmgr",
        .log_generator = generate_ipmgr_log
    };
    
    thread_data[3] = (thread_data_t){
        .thread_id = 3,
        .cpu_core = 3,
        .log_file = LOG_FILE_4,
        .thread_name = "inttrc",
        .log_generator = generate_inttrc_log
    };
    
    // Create threads
    printf("Creating threads...\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        int result = pthread_create(&threads[i], NULL, logger_thread, &thread_data[i]);
        if (result != 0) {
            fprintf(stderr, "Error creating thread %d: %s\n", i, strerror(result));
            return EXIT_FAILURE;
        }
    }
    
    printf("\nAll threads created and running!\n");
    printf("Stress testing monitor.c...\n\n");
    
    // Wait for all threads (will run indefinitely until Ctrl+C)
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    return EXIT_SUCCESS;
}

