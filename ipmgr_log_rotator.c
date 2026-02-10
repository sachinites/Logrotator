
#define _POSIX_C_SOURCE 200809L

/* Standard Library Headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <regex.h>

/* System Headers */
#include <sys/inotify.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Threading Headers */
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#if 0
#define printf(...) \
    do              \
    {               \
    } while (0)
#define fprintf(...) \
    do               \
    {                \
    } while (0)
#define perror(...) \
    do              \
    {               \
    } while (0)
#endif

/*******************************************************************************
 *  CONFIGURATION DEFINES BEGIN
 *  Modify below values to configure this program
 ******************************************************************************/

/* Inotify buffer configuration */
#define EVENT_SIZE (sizeof(struct inotify_event))
#define MAX_FILENAME 64
#define BUF_LEN (1024 * (EVENT_SIZE + MAX_FILENAME + 1))

/* File path configuration */
#define FILE_ABS_PATH_NAME_LEN 256

/* Customizable parameters*/
#define DEFAULT_WATCH_DIR "var/log/"

/*
 * Target log files to monitor (without .bak extension)
 * These are the base names that will be matched in inotify events
 */
static const char target_files[][MAX_FILENAME] = {
    "ipstrc", /* IP Stack Trace logs */
    "pdtrc",  /* Protocol Data Trace logs */
    "inttrc"  /* Internal Trace logs */
};

static const char target_files_bak_files[][MAX_FILENAME] = {
    "ipstrc.bak", /* IP Stack Trace logs */
    "pdtrc.bak",  /* Protocol Data Trace logs */
    "inttrc.bak"  /* Internal Trace logs */
};

/* Specify the number of target files */
#define DEFAULT_NUM_TARGET_FILES 3

/* Semaphore to wait until the new thread is initialized */
static sem_t wait_for_thread_init;

/* Thread handles */
static pthread_t bak_watcher_thread;

/* Inotify file descriptors */
static int inotify_fd; /* inotify instance */
static int watch_fd;   /* watch descriptor for monitored directory */

/* -----------------  Helper APIs Begin ------------------*/

/**
 * get_file_size()
 *
 * Purpose:
 *   Returns the size in bytes of the file at the given path.
 *
 * @param path  Path to the file
 * @return File size in bytes, or (off_t)-1 on error (e.g. file not found)
 */
static off_t
get_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return (off_t)-1;
    }
    return st.st_size;
}

/**
 * base_file_name_extract()
 *
 * Purpose:
 *   Extracts the base filename from a .bak file path.
 *   Modifies the input buffer in-place by truncating at the first dot.
 *
 * Example:
 *   Input:  "var/log/pdtrc.1234567890.bak"
 *   Output: "var/log/pdtrc"
 *
 * @param base_name  Buffer containing full path (modified in-place)
 * @return 0 on success, -1 on failure
 */
static int
base_file_name_extract(char *base_name)
{
    /* Find last dot (should be ".bak") */
    char *last = strrchr(base_name, '.');

    if (last && strcmp(last, ".bak") == 0)
    {
        /* Find first dot after the base filename */
        char *first = strchr(base_name, '.');

        if (first)
        {
            /* Truncate at first dot to get base name */
            *first = '\0';
        }
        else
        {
            /* Edge case: filename has .bak but no other dots */
            *last = '\0';
        }
        return 0;
    }

    return -1; /* Not a valid .bak file */
}

/* -----------------  Helper APIs End ------------------*/

/**
 * handle_bak_file()
 * @param bak_file  Filename of the .bak file (e.g., "pdtrc.1234567890.bak")
 *
 * Note : Logger must not generate pdtrc.bak , it will be ignored by this program.
 * It has to be appended by timestamp
 */

void handle_bak_file(const char *input_bak_file, int findex)
{
    char full_bak_path[FILE_ABS_PATH_NAME_LEN]; // var/log/pdtrc.1234567890.bak
    char bak_file[512];                         // var/log/pdtrc.bak
    char base_name[FILE_ABS_PATH_NAME_LEN];     // var/log/pdtrc

    /* Build absolute path to .bak file */
    snprintf(full_bak_path, sizeof(full_bak_path),
             "%s%s", DEFAULT_WATCH_DIR, input_bak_file);

    printf("%s called to handle : %s\n", __FUNCTION__, full_bak_path);

    /* Verify the .bak file exists before processing */
    if (access(full_bak_path, F_OK) != 0)
    {
        fprintf(stderr, "ERROR: File not found: %s\n", full_bak_path);
        return;
    }

    snprintf(base_name, sizeof(base_name), "%s", full_bak_path);

    /* Extract base name (e.g., "var/log/pdtrc") */
    if (base_file_name_extract(base_name) < 0)
    {
        fprintf(stderr, "ERROR: Invalid .bak filename format\n");
        return;
    }

    printf("\n=== Processing .bak file ===\n");
    printf("Full path: %s\n", full_bak_path);
    printf("Base name: %s\n", base_name);

    snprintf(bak_file, sizeof(bak_file), "%s.bak", base_name);

    /* Check if var/log/pdtrc.bak exists */
    if (access(bak_file, F_OK) != 0)
    {
        /* pdtrc.bak doesn't exist - simply rename var/log/pdtrc.TS.bak to var/log/pdtrc.bak */
        if (rename(full_bak_path, bak_file) == 0)
        {
            printf("   Created: %s (renamed from .bak)\n", bak_file);
        }
        else
        {
            fprintf(stderr, "ERROR: Failed to rename %s to %s: %s\n",
                    full_bak_path, bak_file, strerror(errno));
        }
    }

    /* pdtrc.bak exist, check if its size is 0, then follow rename i.e.
        rename pdtrc.TS.bak to pdtrc.bak
    */
    else if (get_file_size(bak_file) == 0)
    {
        if (rename(full_bak_path, bak_file) == 0)
        {
            printf("   Created: %s (renamed from .bak)\n", bak_file);
        }
        else
        {
            fprintf(stderr, "ERROR: Failed to rename %s to %s: %s\n",
                    full_bak_path, bak_file, strerror(errno));
        }
    }
    else
    {
        /*
         * pdtrc.bak exists - append pdtrc.<TS>.bak content to pdtrc.bak
         * using sendfile()
         * for efficient zero-copy kernel-space transfer.
         * NOTE: Do NOT open dest with O_APPEND - Linux sendfile() returns
         * EINVAL when out_fd has O_APPEND set. Open with O_WRONLY and
         * seek to end before calling sendfile().
         */
        int src_fd = open(full_bak_path, O_RDONLY);
        int dest_fd = open(bak_file, O_WRONLY);

        if (src_fd >= 0 && dest_fd >= 0)
        {
            if (lseek(dest_fd, 0, SEEK_END) < 0)
            {
                fprintf(stderr, "ERROR: lseek failed: %s\n", strerror(errno));
                close(src_fd);
                close(dest_fd);
                return;
            }

            struct stat stat_buf;

            /* Get source file size */
            if (fstat(src_fd, &stat_buf) == 0)
            {
                off_t offset = 0;
                ssize_t bytes_sent;

                /* Transfer data using zero-copy sendfile() */
                while (offset < stat_buf.st_size)
                {
                    bytes_sent = sendfile(dest_fd, src_fd, &offset,
                                          (size_t)(stat_buf.st_size - offset));

                    if (bytes_sent <= 0)
                    {
                        /* Handle interrupts and temporary errors */
                        if (errno == EINTR || errno == EAGAIN)
                        {
                            continue;
                        }
                        fprintf(stderr, "ERROR: sendfile failed: %s\n",
                                strerror(errno));
                        break;
                    }
                }

                close(src_fd);
                close(dest_fd);

                /* Remove .bak file after successful append */
                if (offset == stat_buf.st_size && remove(full_bak_path) == 0)
                {
                    printf("   Appended %ld bytes to %s\n",
                           (long)stat_buf.st_size, bak_file);
                }
                else
                {
                    fprintf(stderr, "ERROR: Failed to remove %s: %s\n",
                            full_bak_path, strerror(errno));
                }
            }
            else
            {
                close(src_fd);
                close(dest_fd);
                fprintf(stderr, "ERROR: Failed to get file size: %s\n",
                        strerror(errno));
            }
        }
        else
        {
            if (src_fd >= 0)
                close(src_fd);
            if (dest_fd >= 0)
                close(dest_fd);
            fprintf(stderr, "ERROR: Failed to open files: %s\n",
                    strerror(errno));
        }
    }
}

/**
 * inotify_bak_file_watcher_thread_fn()
 *
 * Purpose:
 *   Main worker thread that monitors the log directory for .bak file creation
 *   using inotify and processes them as they arrive.
 *
 * Process:
 *   1. Initialize inotify and watch the log directory
 *   2. Enter infinite loop reading inotify events
 *   3. Filter for .bak files matching target log types
 *   4. Process each matching .bak file
 *
 * Thread Safety:
 *   Cancellable at read() cancellation point
 */
static void *
inotify_bak_file_watcher_thread_fn(void *arg)
{
    (void)arg; /* Unused parameter */

    char buffer[BUF_LEN];

    /* Initialize inotify instance */
    inotify_fd = inotify_init();
    if (inotify_fd < 0)
    {
        perror("ERROR: inotify_init failed");
        return NULL;
    }

    /*
     * Add watch for log directory
     * IN_CREATE: Triggered when files are created
     * IN_MOVED_TO: Triggered when files are moved into directory
     */
    watch_fd = inotify_add_watch(inotify_fd, DEFAULT_WATCH_DIR,
                                 IN_CREATE | IN_MOVED_TO);

    if (watch_fd < 0)
    {
        perror("ERROR: inotify_add_watch failed");
        close(inotify_fd);
        return NULL;
    }

    printf("Monitoring directory: %s\n", DEFAULT_WATCH_DIR);

    /* Configure thread cancellation behavior */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    /* Signal that thread initialization is complete */
    sem_post(&wait_for_thread_init);

    /*
     * Main event processing loop
     * Blocks on read() waiting for inotify events
     */
    while (1)
    {
        /* Read events from inotify (cancellation point) */
        int length = read(inotify_fd, buffer, BUF_LEN);

        if (length < 0)
        {
            perror("ERROR: inotify read failed");
            break;
        }

        /* Process all events in the buffer */
        int i = 0;
        while (i < length)
        {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            /* Only process events with a filename */
            if (event->len > 0)
            {

                /* Check if this is a .bak file */
                if (strstr(event->name, ".bak") == NULL)
                {
                    i += EVENT_SIZE + event->len;
                    continue;
                }

                /* Ignore any files generated by logrotate utility itself
                eg : pdtrc.bak.1 , pdtrc.bak.1.gz */
                if (strstr(event->name, ".bak."))
                {
                    printf("[inotify] Ignoring bak file: %s, invalid format\n", event->name);
                    i += EVENT_SIZE + event->len;
                    continue;
                }

                /* logger is suppose to produce .bak file appended with Timestamp
                i.e. name should be pdtrc.<TS>.bak., for example : pdtrc.1234567890.bak
                i.e. ignore any .bak file which is not appended with Timestamp
                */

                printf("\n[inotify] event detected: %s\n", event->name);

                /* Check if it matches one of our target log types */

                for (int j = 0; j < DEFAULT_NUM_TARGET_FILES; j++)
                {

                    if (strstr(event->name, target_files[j]))
                    {

                        printf("[inotify] Matches target: %s\n", target_files[j]);

                        /* if it is exact bak file, ignore this event. It is produced
                        by this program itself and not logger */
                        if (strcmp(event->name, target_files_bak_files[j]) == 0)
                        {
                            printf("[inotify] Ignoring bak file: %s, self created\n", event->name);
                            continue;
                        }
                        handle_bak_file(event->name, j);
                    }
                }
            }

            /* Move to next event */
            i += EVENT_SIZE + event->len;
        }
    }

    /* Cleanup (unreachable code - thread is cancelled) */
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);
    return NULL;
}

/*******************************************************************************
 *                      THREAD MANAGEMENT FUNCTIONS
 ******************************************************************************/

/**
 * inotify_watcher_thread_init()
 *
 * Purpose:
 *   Creates and initializes both worker threads (log rotator and zipper).
 *   Waits for each thread to signal successful initialization before
 *   proceeding.
 *
 * Thread Attributes:
 *   - PTHREAD_CREATE_JOINABLE: Threads can be joined for cleanup
 */
void inotify_watcher_thread_init(void)
{
    pthread_attr_t attr1;
    pthread_attr_init(&attr1);
    pthread_attr_setdetachstate(&attr1, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&bak_watcher_thread, &attr1, inotify_bak_file_watcher_thread_fn, NULL) != 0)
    {
        fprintf(stderr, "ERROR: Failed to create log rotator thread\n");
        exit(EXIT_FAILURE);
    }

    /* Wait for thread to initialize */
    sem_wait(&wait_for_thread_init);
    printf(" bak file watcher thread started\n");

    pthread_attr_destroy(&attr1);
}

/**
 * ipmgr_start_bak_file_watcher_thread()
 *
 * Purpose:
 *   Public API to start the log rotation system.
 *   Initializes semaphores and creates worker threads.
 */
void ipmgr_start_bak_file_watcher_thread(void)
{
    /* Initialize synchronization primitives */
    sem_init(&wait_for_thread_init, 0, 0); /* Zero semaphore for init sync */

    /* Create and start worker threads */
    inotify_watcher_thread_init();

    /* Clean up init semaphore (no longer needed) */
    sem_destroy(&wait_for_thread_init);

    printf("========================================\n");
    printf("  System Ready - Watching for .bak files\n");
    printf("========================================\n\n");
}

/**
 * ipmgr_stop_bak_file_watcher_thread()
 *
 * Purpose:
 *   Public API to stop the log rotation system.
 *   Cancels both threads, waits for them to exit, and cleans up resources.
 */
void ipmgr_stop_bak_file_watcher_thread(void)
{
    /* Cancel and join log rotator thread */
    pthread_cancel(bak_watcher_thread);
    pthread_join(bak_watcher_thread, NULL);
    printf(" bak file watcher thread stopped\n");

    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);

    printf("========================================\n");
    printf("  System Stopped Successfully\n");
    printf("========================================\n\n");
}

/*******************************************************************************
 *                              MAIN FUNCTION
 ******************************************************************************/

/**
 * main()
 *
 * Test driver for the log rotation system.
 * Starts the system, runs for 60 seconds, then shuts down.
 */
int main(int argc, char **argv)
{
    (void)argc; /* Unused parameters */
    (void)argv;

    /* Start the log rotation system */
    ipmgr_start_bak_file_watcher_thread();

    /* Run for 60 seconds (or forever in production) */
    // sleep(60);

    /* Graceful shutdown */
    // ipmgr_stop_bak_file_watcher_thread();

    pthread_exit(NULL);
    return EXIT_SUCCESS;
}
