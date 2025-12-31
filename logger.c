#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define LOG_FILE_1 "var/log/ipstrc.log"
#define LOG_FILE_2 "var/log/pdtrc.log"
#define LOG_FILE_3 "var/log/ipmgr.log"
#define LOG_FILE_4 "var/log/inttrc.log"
#define MAX_LOG_SIZE 10240  // 10KB in bytes

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

// Write log to file, rename to .bak if size exceeds limit
int write_log(FILE **fp, const char *filename, const char *log_message) {
    static long old_size = 0;
    long current_size = get_file_size(filename);

    if (current_size - old_size >= MAX_LOG_SIZE) {
        fclose(*fp);

        // make a new unique .bak each time
        char bak_name[256];
        snprintf(bak_name, sizeof(bak_name), "%s.%ld.bak", filename, time(NULL));

        rename(filename, bak_name);

        *fp = fopen(filename, "a"); // start new log
        old_size = 0;
    }

    fprintf(*fp, "%s", log_message);
    fflush(*fp);
    return 0;
}


int main(int argc, char *argv[]) {
    char log_buffer[512];
    int log_counter = 0;
    
    // Seed random number generator
    srand(time(NULL));
    
    printf("Random Log Generator started...\n");
    printf("Writing logs to:\n");
    printf("  - %s\n", LOG_FILE_1);
    printf("  - %s\n", LOG_FILE_2);
    printf("  - %s\n", LOG_FILE_3);
    printf("  - %s\n", LOG_FILE_4);
    printf("Rate: ~10 lines per second\n");
    printf("Max log size: %d bytes (rename to .bak when exceeded)\n", MAX_LOG_SIZE);
    printf("Press Ctrl+C to stop\n\n");
    FILE *ipstr_fp = fopen(LOG_FILE_1, "a");
    if (ipstr_fp == NULL)
    {
        fprintf(stderr, "Error opening %s: %s\n", LOG_FILE_1, strerror(errno));
        return -1;
    }
    FILE *pdtr_fp = fopen(LOG_FILE_2, "a");
    if (pdtr_fp == NULL)
    {
        fprintf(stderr, "Error opening %s: %s\n", LOG_FILE_2, strerror(errno));
        return -1;
    }
    FILE *ipmgr_fp = fopen(LOG_FILE_3, "a");
    if (ipmgr_fp == NULL)
    {
        fprintf(stderr, "Error opening %s: %s\n", LOG_FILE_3, strerror(errno));
        return -1;
    }
    FILE *inttr_fp = fopen(LOG_FILE_4, "a");
    if (inttr_fp == NULL)
    {
        fprintf(stderr, "Error opening %s: %s\n", LOG_FILE_4, strerror(errno));
        return -1;
    }

    // Main loop - generate logs continuously
    while (1) {
        // Decide which log file to write to (round-robin with some randomness)
        int target = log_counter % 4;
        
        // Add some randomness to distribution
        if (rand() % 100 < 20) {
            target = rand() % 4;
        }

        switch (target) {
            case 0:
                generate_ipstrc_log(log_buffer, sizeof(log_buffer));

                if (write_log(&ipstr_fp, LOG_FILE_1, log_buffer) == 0) {
                    printf("[%d] ipstrc: %s", log_counter + 1, log_buffer);
                }
                break;
            
            case 1:
                generate_pdtrc_log(log_buffer, sizeof(log_buffer));
                if (write_log(&pdtr_fp, LOG_FILE_2, log_buffer) == 0) {
                    printf("[%d] pdtrc: %s", log_counter + 1, log_buffer);
                }
                break;
            
            case 2:
                generate_ipmgr_log(log_buffer, sizeof(log_buffer));
                if (write_log(&ipmgr_fp, LOG_FILE_3, log_buffer) == 0) {
                    printf("[%d] ipmgr: %s", log_counter + 1, log_buffer);
                }
                break;            
            case 3:
                generate_inttrc_log(log_buffer, sizeof(log_buffer));
                if (write_log(&inttr_fp, LOG_FILE_4, log_buffer) == 0) {
                    printf("[%d] inttrc: %s", log_counter + 1, log_buffer);
                }
                break;
        }
        
        log_counter++;
        
        // Sleep for 100ms to achieve ~10 logs per second
        usleep(10000);  // 100,000 microseconds = 100ms
    }
    
    return 0;
}

