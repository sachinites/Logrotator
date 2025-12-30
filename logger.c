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

// Get file size in bytes
long get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return 0;  // File doesn't exist or error
}

// Rename log file to .bak file
int rename_to_bak(const char *filename) {
    char bak_filename[512];
    
    // Create .bak filename by replacing .log with .bak
    strncpy(bak_filename, filename, sizeof(bak_filename) - 1);
    bak_filename[sizeof(bak_filename) - 1] = '\0';
    
    // Find and replace .log with .bak
    char *log_ext = strstr(bak_filename, ".log");
    if (log_ext != NULL) {
        strcpy(log_ext, ".bak");
    } else {
        // If no .log extension, just append .bak
        strncat(bak_filename, ".bak", sizeof(bak_filename) - strlen(bak_filename) - 1);
    }
    
    // Remove old .bak file if it exists
    if (access(bak_filename, F_OK) == 0) {
        if (remove(bak_filename) != 0) {
            fprintf(stderr, "Warning: Could not remove old %s: %s\n", 
                    bak_filename, strerror(errno));
        }
    }
    
    // Rename current log file to .bak
    if (rename(filename, bak_filename) == 0) {
        printf("\n[ROTATION] Renamed %s to %s\n", filename, bak_filename);
        return 0;
    } else {
        fprintf(stderr, "Error renaming %s to %s: %s\n", filename, bak_filename, strerror(errno));
        return -1;
    }
}

// Write log to file, rename to .bak if size exceeds limit
int write_log(const char *filename, const char *log_message) {
    // Check file size before writing
    long current_size = get_file_size(filename);
n    
    // If file exceeds max size, rename to .bak and start fresh
    if (current_size >= MAX_LOG_SIZE) {
        printf("\n[SIZE LIMIT] File %s reached %ld bytes\n", filename, current_size);
        rename_to_bak(filename);
    }
    
    // Open file in append mode (will create new file if it was renamed)
    FILE *fp = fopen(filename, "a");
    if (fp == NULL) {
        fprintf(stderr, "Error opening %s: %s\n", filename, strerror(errno));
        return -1;
    }
    
    fprintf(fp, "%s", log_message);
    fflush(fp);
    fclose(fp);
    
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
    printf("Rate: ~10 lines per second\n");
    printf("Max log size: %d bytes (rename to .bak when exceeded)\n", MAX_LOG_SIZE);
    printf("Press Ctrl+C to stop\n\n");
    
    // Main loop - generate logs continuously
    while (1) {
        // Decide which log file to write to (round-robin with some randomness)
        int target = log_counter % 3;
        
        // Add some randomness to distribution
        if (rand() % 100 < 20) {
            target = rand() % 3;
        }
        
        switch (target) {
            case 0:
                generate_ipstrc_log(log_buffer, sizeof(log_buffer));
                if (write_log(LOG_FILE_1, log_buffer) == 0) {
                    printf("[%d] ipstrc: %s", log_counter + 1, log_buffer);
                }
                break;
            
            case 1:
                generate_pdtrc_log(log_buffer, sizeof(log_buffer));
                if (write_log(LOG_FILE_2, log_buffer) == 0) {
                    printf("[%d] pdtrc: %s", log_counter + 1, log_buffer);
                }
                break;
            
            case 2:
                generate_ipmgr_log(log_buffer, sizeof(log_buffer));
                if (write_log(LOG_FILE_3, log_buffer) == 0) {
                    printf("[%d] ipmgr: %s", log_counter + 1, log_buffer);
                }
                break;
        }
        
        log_counter++;
        
        // Sleep for 100ms to achieve ~10 logs per second
        usleep(10000);  // 100,000 microseconds = 100ms
    }
    
    return 0;
}

