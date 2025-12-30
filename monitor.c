#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

#define EVENT_SIZE (sizeof(struct inotify_event))
#define MAX_FILENAME 255
#define BUF_LEN (1024 * (EVENT_SIZE + MAX_FILENAME + 1))

// Default values (can be overridden by command-line args)
#define DEFAULT_MAX_FILES 3
#define DEFAULT_WATCH_DIR "var/log/"
#define DEFAULT_NUM_TARGET_FILES 4

// Global configuration
int MAX_FILES = DEFAULT_MAX_FILES;
const char *watch_dir = DEFAULT_WATCH_DIR;
int NUM_TARGET_FILES = DEFAULT_NUM_TARGET_FILES;
char **target_files = NULL;

void zip_all_files(char *name) {

    if(!name) return;

    char base[512], dir[512], fname[256];
    int max_index = 0;

    /* Parse .number at end */
    char *last_dot = strrchr(name, '.');
    if (!last_dot || sscanf(last_dot+1, "%d", &max_index) != 1) {
        fprintf(stderr, "ERROR: Invalid file format: %s (expected base.number)\n", name);
        return;
    }

    /* Extract base "path/filename" without number */
    size_t len = last_dot - name;
    strncpy(base, name, len);
    base[len] = '\0';

    /* Split path and filename */
    char *slash = strrchr(base, '/');
    if (slash) {
        size_t dlen = slash - base;
        strncpy(dir, base, dlen);
        dir[dlen] = '\0';
        strcpy(fname, slash + 1);
    } else {
        strcpy(dir, ".");
        strcpy(fname, base);
    }

    /* Timestamp for archive */
    char archive[600];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm);

    snprintf(archive, sizeof(archive), "%s/%s_%s.tar.gz", dir, fname, timestamp);

    /* Build tar command: only filenames inside archive */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -czf \"%s\" -C \"%s\"", archive, dir);

    printf("\n--- Adding files ---\n");
    for (int i = 1; i <= max_index; i++) {
        char file_only[256], fullpath[600];
        snprintf(file_only, sizeof(file_only), "%s.%d", fname, i);
        snprintf(fullpath, sizeof(fullpath), "%s/%s.%d", dir, fname, i);

        if (access(fullpath, F_OK) == 0) {
            printf("✔ Found: %s\n", fullpath);
            strcat(cmd, " ");
            strcat(cmd, file_only);
        } else {
            printf("✘ Missing: %s\n", fullpath);
        }
    }

    printf("\n--- Running TAR ---\n%s\n\n", cmd);

    if(system(cmd) != 0) {
        perror("tar failed");
        return;
    }

    /* Remove originals AFTER successful archive */
    printf("--- Removing originals ---\n");
    for (int i = 1; i <= max_index; i++) {
        char rm[600];
        snprintf(rm, sizeof(rm), "%s/%s.%d", dir, fname, i);
        if(remove(rm) == 0)
            printf("✔ Deleted: %s\n", rm);
        else
            perror(rm);
    }

    printf("\n🎯 DONE: %s\n\n", archive);
}

// Function to rotate numbered log files
void rotate_numbered_files(const char *base_name) {
    char old_name[512];
    char new_name[512];

    // Delete the oldest file if it exists
    snprintf(old_name, sizeof(old_name), "%s.log.%d", base_name, MAX_FILES);
    if (access(old_name, F_OK) == 0) {
        if (remove(old_name) == 0) {
            printf("Deleted oldest file: %s\n", old_name);
        }
    }

    int zip = 0;
    // Rename files backwards
    for (int i = MAX_FILES - 1; i >= 0; i--) {
        snprintf(old_name, sizeof(old_name), "%s.log.%d", base_name, i);
        snprintf(new_name, sizeof(new_name), "%s.log.%d", base_name, i + 1);

        if (access(old_name, F_OK) == 0) {
            if (rename(old_name, new_name) == 0) {
                printf("Renamed %s to %s\n", old_name, new_name);
                if (i == MAX_FILES -1) zip = 1;
            } else {
                fprintf(stderr, "Error renaming %s to %s: %s\n",
                        old_name, new_name, strerror(errno));
            }
        }
    }
    if (zip){
        char fname[512];
        snprintf (fname, sizeof(fname), "%s.log.%d" , base_name, MAX_FILES);
        zip_all_files(fname);
        zip = 0;
    }
}

// Function to handle .bak file creation
void handle_bak_file(const char *bak_file) {
    char full_bak_path[512];
    char numbered_file[512];
    char base_name[512];

    // Build full path to .bak file
    snprintf(full_bak_path, sizeof(full_bak_path), "%s%s", watch_dir, bak_file);
    
    // Small delay to ensure file system operations are complete
    usleep(100000);  // 100ms
    
    // Verify the .bak file exists before processing
    if (access(full_bak_path, F_OK) != 0) {
        fprintf(stderr, "File not found: %s\n", full_bak_path);
        return;
    }
    
    // Build base name with full path (remove .bak extension)
    snprintf(base_name, sizeof(base_name), "%s", full_bak_path);
    
    char *dot = strrchr(base_name, '.');
    if (dot && strcmp(dot, ".bak") == 0) {
        *dot = '\0';
    } else {
        fprintf(stderr, "Unexpected .bak file format: %s\n", bak_file);
        return;
    }

    printf("Detected .bak file: %s (base: %s)\n", full_bak_path, base_name);

    // Rename .bak to .log.1
    snprintf(numbered_file, sizeof(numbered_file), "%s.log.0", base_name);

    if (rename(full_bak_path, numbered_file) == 0) {
        printf("Renamed %s to %s\n", full_bak_path, numbered_file);
    } else {
        fprintf(stderr, "Error renaming %s to %s: %s\n",
                full_bak_path, numbered_file, strerror(errno));
    }
    // Rotate existing numbered files
    rotate_numbered_files(base_name);
}

static void *sms_log_rotate(void *arg) {
    int inotify_fd;
    int watch_fd;
    char buffer[BUF_LEN];

    printf("Log Monitor started. Watching for .bak file creation...\n");

    // Initialize inotify
    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init");
        return 0;
    }

    // Watch directory for file creation
    watch_fd = inotify_add_watch(inotify_fd, watch_dir, IN_CREATE | IN_MOVED_TO);
    if (watch_fd < 0) {
        perror("inotify_add_watch");
        close(inotify_fd);
        return 0;
    }

    printf("Monitoring directory: %s\n", watch_dir);

    // Main event loop
    while (1) {
        int length = read(inotify_fd, buffer, BUF_LEN);
        if (length < 0) {
            perror("read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            if (event->len > 0) {
                printf("DEBUG: Event detected - mask: 0x%x, name: %s\n", event->mask, event->name);

                if (strstr(event->name, ".bak") != NULL) {
                    printf("DEBUG: .bak file detected: %s\n", event->name);

                    // Check if it's one of our target files
                    for (int j = 0; j < NUM_TARGET_FILES; j++) {
                        if (strcmp(event->name, target_files[j]) == 0) {
                            printf("DEBUG: Processing target file: %s\n", event->name);
                            handle_bak_file(event->name);
                            break;
                        }
                    }
                }
            }

            i += EVENT_SIZE + event->len;
        }
    }

    // Cleanup
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);

    return 0;
}

void sms_log_rotator_thread_init(void) {
    pthread_t thread_id;

    if (pthread_create(&thread_id, NULL, sms_log_rotate, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    printf("Log Monitor thread started.\n");
}

#if 1
int main(int argc, char *argv[]) {
    // Optional command-line args:
    // argv[1] = directory, argv[2] = max files, argv[3..] = target files
    if (argc > 1) {
        watch_dir = argv[1];
    }
    if (argc > 2) {
        MAX_FILES = atoi(argv[2]);
    }
    if (argc > 3) {
        NUM_TARGET_FILES = argc - 3;
        target_files = &argv[3];
    } else {
        // Default target files
        static char *default_files[] = {"ipstrc.bak", "pdtrc.bak", "ipmgr.bak", "inttrc.bak"};
        target_files = default_files;
    }

    printf("Configuration:\n");
    printf("Directory: %s\n", watch_dir);
    printf("Max rotated files: %d\n", MAX_FILES);
    printf("Target files:\n");
    for (int i = 0; i < NUM_TARGET_FILES; i++) {
        printf("  %s\n", target_files[i]);
    }

    sms_log_rotator_thread_init();

    // Keep main thread alive
    pthread_exit(0);
    return 0;
}
#endif