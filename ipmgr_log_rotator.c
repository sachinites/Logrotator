/*******************************************************************************
 * File: ipmgr_log_rotator.c
 * 
 * Description:
 *   Log rotation and compression system for managing application log files.
 *   Uses inotify to monitor .bak file creation, automatically rotates numbered
 *   log files, and compresses old logs into timestamped tar.gz archives.
 *
 * Features:
 *   - Automatic detection of .bak file creation using inotify
 *   - Numbered log file rotation (log.0, log.1, ..., log.N)
 *   - Asynchronous compression using dedicated zipper thread
 *   - Zero-copy file concatenation using sendfile()
 *   - Thread-safe operation with semaphores and atomic variables
 *
 * Managed Files:
 *   - ipstrc.bak  -> IP Stack Trace logs
 *   - pdtrc.bak   -> Protocol Data Trace logs
 *   - ipmgr.bak   -> IP Manager logs
 *   - inttrc.bak  -> Internal Trace logs
 *
 ******************************************************************************/

/*
Design Consideration: 

Goal : Logs need to be rotated and zipped as when they are produced, even at a very high rate. Log Rotator Utility cannot wait/sleep/block

Log Gen could produce logs at higher rate
File Compression could be slow as compared to file rotation 
Therefore, File compression is offloaded to separate thread “Compression thread” so that log rotator thread is quickly relieved and go back listen for inotify events 
But it created back pressure – Log Rotator thread cannot rotate files if File Compressor thread is busy compressing them
Back pressure is prevented by not making log rotator thread wait for compressor thread to finish. 
Log rotator thread first checks the status of compressor thread. If the C-thread is busy, log-rotator thread append incoming .bak file logs to pdtrc.log.0 and go back listen to inotify event ( no wait here ) 
No .bak file should be missed */

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
#define printf(...) do { } while (0)
#define fprintf(...) do { } while (0)
#define perror(...) do { } while (0)
#endif 

/*******************************************************************************
 *  CONFIGURATION DEFINES BEGIN 
 *  Modify below values to configure this program
 ******************************************************************************/

/* Inotify buffer configuration */
#define EVENT_SIZE              (sizeof(struct inotify_event))
#define MAX_FILENAME            64
#define BUF_LEN                 (1024 * (EVENT_SIZE + MAX_FILENAME + 1))

/* File path configuration */
#define FILE_ABS_PATH_NAME_LEN  256

/* Customizable parameters*/
#define DEFAULT_WATCH_DIR       "var/log/"
#define DEFAULT_MAX_FILES       5  /* Number of rotated log files to keep */

/* 
 * Target log files to monitor (without .bak extension)
 * These are the base names that will be matched in inotify events
 */
static const char target_files[][MAX_FILENAME] = {
    "ipstrc",   /* IP Stack Trace logs */
    "pdtrc",    /* Protocol Data Trace logs */
    "ipmgr",    /* IP Manager logs */
    "inttrc"    /* Internal Trace logs */
};

/* Specify the number of target files */
#define DEFAULT_NUM_TARGET_FILES 4


/* CONTROL FLAGS BEGIN */

/* Remove obsolete tar files after successful archive creation */
#define CTRL_F_DEL_OBSOLETE_TAR_FILES 1
/* Remove original files after successful archive creation */
#define CTRL_F_DELETE_OBSOLETE_LOG_FILES 2

static uint16_t control_flags = CTRL_F_DEL_OBSOLETE_TAR_FILES | \
                                CTRL_F_DELETE_OBSOLETE_LOG_FILES;


/*********************************************************************************
 *  CONFIGURATION DEFINES ENDs
 *********************************************************************************/
 /********************************************************************************/
 /********************************************************************************/





#define TAR_CMD_SIZE_LEN    (DEFAULT_MAX_FILES * FILE_ABS_PATH_NAME_LEN)


/*******************************************************************************
 *                          GLOBAL VARIABLES
 ******************************************************************************/

/*
 * Thread Synchronization Primitives
 * ----------------------------------
 * zipper_thread_sync: Zero semaphore controlling access to zipper thread.
 *                     Posted when files are ready to be compressed.
 *                     
 * wait_for_thread_init: Zero semaphore ensuring threads are initialized
 *                       before ipmgr main thread proceed further.
 * operations_on_log_files : To implement mutual exclustion betwen
 *                       rotator thread and compressor thread. Log files
 *                       compression and rotation is mutually exclusive.
 */
static sem_t zipper_thread_sync; // Zero Sema
static sem_t wait_for_thread_init; // Zero Sema
static sem_t inotify_events_allow_sema; // Zero Sema

/* Spin lock for mutual exclusivity for any operation on logfiles, 
    since they are shared data being acted upon by Rotator and
    Compressor thread */
static pthread_spinlock_t operations_on_log_files;

/*
 * Atomic flag indicating compression is in progress.
 * When true, the log rotator will concatenate new .bak files to log.0
 * instead of rotating, to avoid interfering with compression.
 */
static atomic_bool zip_in_progress = false;

/*
 * Per-file-type compression state
 * Each file type (ipstrc, pdtrc, ipmgr, inttrc) tracks its own terminal_fname
 * to prevent race conditions where multiple file types trigger compression
 * simultaneously and overwrite each other's filenames.
 */
static struct {
    char terminal_fname[MAX_FILENAME];
    atomic_bool needs_compression;
} file_compression_state[DEFAULT_NUM_TARGET_FILES];

/* Spinlock to protect compression state updates */
static pthread_spinlock_t compression_state_lock;

/* Thread handles */
static pthread_t zipper_thread;
static pthread_t log_rotator_thread;

/* Inotify file descriptors */
static int inotify_fd;  /* inotify instance */
static int watch_fd;    /* watch descriptor for monitored directory */

/*******************************************************************************
 *                      FUNCTION DECLARATION
 ******************************************************************************/

void 
file_rotate(const char *base_name);



/*******************************************************************************
 *                      FILE COMPRESSION FUNCTIONS
 ******************************************************************************/

/**
 * get_file_type_index()
 * 
 * Purpose:
 *   Determines the index of a file type in the target_files array.
 *   Used to track archives separately for each file type.
 *
 * @param fname  Base filename (e.g., "ipstrc.log", "ipmgr.log")
 * @return Index (0-3) if found, -1 if not found
 */
static int
get_file_type_index(const char *fname)
{
    for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++) {
        if (strstr(fname, target_files[i]) != NULL) {
            return i;
        }
    }
    return -1;  /* Not found */
}

static void
generate_dummy_inotify_bak_event()
{
    int i;
    char cmd[512];
    char dummy_bak_fname[FILE_ABS_PATH_NAME_LEN];

    for (i = 0; i < DEFAULT_NUM_TARGET_FILES; i++)
    {
        snprintf(dummy_bak_fname, sizeof(dummy_bak_fname), 
            "%s%s.dummy.bak", DEFAULT_WATCH_DIR, target_files[i]);

        snprintf(cmd, sizeof(cmd), "touch %s", dummy_bak_fname);

        printf("\n--- Executing dummy bak file creation cmd ---\n%s\n\n", cmd);
        if (system(cmd) != 0) {
            perror("ERROR: dummy bak file creation command failed");
        }
    }
}

/* if *.log.0 exist 
     then trigger file rotate
   else no-op 
*/
static void 
handle_dummy_bak_file_creation(int findex) {

    char base_fname[FILE_ABS_PATH_NAME_LEN];
    char log0_fname[FILE_ABS_PATH_NAME_LEN];

    snprintf (log0_fname, sizeof (log0_fname), "%s%s.log.0", 
        DEFAULT_WATCH_DIR, target_files[findex]);

    if (access (log0_fname, F_OK)) return;

    pthread_spin_lock(&operations_on_log_files);

    snprintf (base_fname, sizeof(base_fname), "%s%s",
        DEFAULT_WATCH_DIR, target_files[findex]);
    /* Rotate existing numbered files */
    file_rotate(base_fname);
    /* Unlock the Critical section. */
    pthread_spin_unlock(&operations_on_log_files);    
}

static void
rename_all_log0_to_log1_log_file() {

    char log0_fname[FILE_ABS_PATH_NAME_LEN];
    char log1_fname[FILE_ABS_PATH_NAME_LEN];

    for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++)
    {
        snprintf(log0_fname, sizeof(log0_fname), "%s%s.log.0",
                 DEFAULT_WATCH_DIR, target_files[i]);

        if (access(log0_fname, F_OK)) return;

        snprintf(log1_fname, sizeof(log1_fname), "%s%s.log.1",
                 DEFAULT_WATCH_DIR, target_files[i]);

        if (rename(log0_fname, log1_fname) == 0) {
            printf("   %s(): Renamed: %s -> %s\n", __FUNCTION__, log0_fname, log1_fname);
        }
        else {
            fprintf(stderr, "%s(): ERROR: Rename failed: %s -> %s: %s\n",
                    __FUNCTION__, log0_fname, log1_fname, strerror(errno));
        }
    }
}


/**
 * compress_all_log_files_with_name()
 * 
 * Purpose:
 *   Compresses all numbered log files (log.1 to log.N) into a single
 *   timestamped tar.gz archive, then deletes the original files.
 *
 * Input:
 *   terminal_fname_param: path to the highest numbered log file (e.g., "var/log/ipmgr.log.5")
 *
 * Process:
 *   1. Parse filename to extract base name and maximum index
 *   2. Create timestamped archive name
 *   3. Build tar command to compress all numbered files
 *   4. Execute tar command
 *   5. Delete old archive (after new one succeeds)
 *   6. Delete original files on success
 *
 * Example:
 *   Input:  terminal_fname = "var/log/ipmgr.log.5"
 *   Output: var/log/ipmgr_2025-12-31_14-30-45.tar.gz
 *           (contains ipmgr.log.1, ipmgr.log.2, ..., ipmgr.log.5)
 */
static void 
compress_all_log_files_with_name(const char *terminal_fname_param)
{
    int max_index = 0;
    char base[MAX_FILENAME + 32];   /* Base path without number */
    char fname[MAX_FILENAME];       /* Filename without path */
    const char *name = terminal_fname_param;

    printf("%s() : File compression triggered by creation of %s\n", __FUNCTION__, name);

    /*
     * Parse the file number from the end of the filename
     * Example: "var/log/ipmgr.log.5" -> max_index = 5
     */
    char *last_dot = strrchr(name, '.');
    if (!last_dot || sscanf(last_dot + 1, "%d", &max_index) != 1) {
        fprintf(stderr, "ERROR: Invalid file format: %s (expected base.number)\n", name);
        return;
    }

    /* Extract base path without the number (e.g., "var/log/ipmgr.log") */
    size_t len = last_dot - name;
    strncpy(base, name, len);
    base[len] = '\0';

    /* extract filename from the path */
    char *slash = strrchr(base, '/');
    if (slash) {
        strcpy(fname, slash + 1);
    } else {
        strcpy(fname, base);
    }

    /*
     * Generate timestamp for archive name
     * Format: YYYY-MM-DD_HH-MM-SS
     * 
     * Store archives separately for each file type (ipstrc, pdtrc, ipmgr, inttrc)
     * Array size is 4 to match the 4 target file types
     */
    static char archives[DEFAULT_NUM_TARGET_FILES][256] = {{0}};
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    
    /* Determine which file type we're compressing */
    int file_idx = get_file_type_index(fname);
    if (file_idx < 0) {
        fprintf(stderr, "ERROR: Unknown file type: %s\n", fname);
        return;
    }
    
    /* Generate timestamp and new archive name */
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm);
    
    /* Save old archive name before overwriting */
    char old_archive[256];
    strncpy(old_archive, archives[file_idx], sizeof(old_archive));
    old_archive[sizeof(old_archive) - 1] = '\0';
    
    /* Create new archive name */
    snprintf(archives[file_idx], sizeof(archives[file_idx]), "%s%s_%s.tar.gz", 
             DEFAULT_WATCH_DIR, fname, timestamp);

    /*
     * Build tar command
     * -c: create archive
     * -z: compress with gzip
     * -f: output filename
     * -C: change to directory (so archive contains only filenames, not full paths)
     */
    char cmd[TAR_CMD_SIZE_LEN];
    char file_only[MAX_FILENAME * 2];
    char fullpath[FILE_ABS_PATH_NAME_LEN];
    bool do_archive = false;

    snprintf(cmd, sizeof(cmd), "tar -czf \"%s\" -C \"%s\"", archives[file_idx], DEFAULT_WATCH_DIR);

    /* Iterate through all numbered files and add them to the tar command */
    printf("\n--- Collecting Files for Archive ---\n");
    for (int i = 1; i <= max_index; i++) {
        snprintf(file_only, sizeof(file_only), "%s.%d", fname, i);
        snprintf(fullpath, sizeof(fullpath), "%s%s.%d", DEFAULT_WATCH_DIR, fname, i);

        if (access(fullpath, F_OK) == 0) {
            printf("   Found: %s\n", fullpath);
            strcat(cmd, " ");
            strcat(cmd, file_only);
            do_archive = true;
        } else {
            printf("   Missing: %s\n", fullpath);
        }
    }

    /* Exit if no files found */
    if (!do_archive) {
        printf("Nothing to archive.\n");
        return;
    }

    /* Now delete old archive before creating the new one */
    if ((control_flags & CTRL_F_DEL_OBSOLETE_TAR_FILES) && 
            old_archive[0] != '\0' && 
            access(old_archive, F_OK) == 0) {

        /* Old archive exists, try to remove it */
        if (remove(old_archive) == 0) {
            printf("Obsolete Archive %s Removed\n", old_archive);
        } else {
            printf("Obsolete Archive %s Failed to remove: ", old_archive);
            perror("Archive delete failed");
        }
    }

    /* Execute tar command */
    printf("\n--- Executing TAR Command ---\n%s\n\n", cmd);
    if (system(cmd) != 0) {
        perror("ERROR: tar command failed");
        return;
    }

    printf("\n[SUCCESS] Archive created: %s\n\n", archives[file_idx]);

    /* Remove original files after successful archive creation */
    if (control_flags & CTRL_F_DELETE_OBSOLETE_LOG_FILES) {

        printf("--- Cleaning Up Original Files ---\n");

        for (int i = 1; i <= max_index; i++) {

            char rm[256];
            snprintf(rm, sizeof(rm), "%s%s.%d", DEFAULT_WATCH_DIR, fname, i);
            
            if (remove(rm) == 0) {
                printf("   Deleted: %s\n", rm);
            } else {
                perror(rm);
            }
        }
    }

    printf("\n[SUCCESS] Archive created: %s\n\n", archives[file_idx]);
}

/**
 * zip_log_file_thread_fn()
 * 
 * Purpose:
 *   Worker thread that waits for compression requests and processes them.
 *   Runs in an infinite loop waiting on the zipper_thread_sync semaphore.
 *
 * Thread Safety:
 *   - Uses atomic operations to set/clear zip_in_progress flag
 *   - Synchronized with log rotator via semaphore
 *
 * Cancellation:
 *   Thread is cancellable at sem_wait() cancellation point
 */
static void *
zip_log_file_thread_fn(void *arg)
{
    (void)arg;  /* Unused parameter */
    char local_terminal_fname[MAX_FILENAME];
    char log0_fname[FILE_ABS_PATH_NAME_LEN];

    /* Configure thread cancellation behavior */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    /* Signal that thread initialization is complete */
    sem_post(&wait_for_thread_init);

    /* Main work loop */
    while (1) {
        /* Wait for compression request (cancellation point) */
        sem_wait(&zipper_thread_sync);
        
        /* Find which file type needs compression */
        int file_idx_to_compress = -1;
        
        pthread_spin_lock(&compression_state_lock);
        for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++) {
            if (atomic_load(&file_compression_state[i].needs_compression)) {
                file_idx_to_compress = i;
                
                /* Copy terminal filename to local variable */
                strncpy(local_terminal_fname, 
                        file_compression_state[i].terminal_fname,
                        sizeof(local_terminal_fname));
                local_terminal_fname[sizeof(local_terminal_fname) - 1] = '\0';
                
                /* Clear the flag */
                atomic_store(&file_compression_state[i].needs_compression, false);
                break;
            }
        }
        pthread_spin_unlock(&compression_state_lock);
        
        if (file_idx_to_compress < 0) {
            fprintf(stderr, "WARNING: Zipper woke up but no file needs compression\n");
            continue;
        }
        
        /* lock the Critical section. Any operation on numbered files
        is defined as C.S */
        pthread_spin_lock(&operations_on_log_files);

        /* Set atomic flag to indicate compression in progress */
        atomic_store(&zip_in_progress, true);
        
        /* Perform compression using the local copy */
        compress_all_log_files_with_name(local_terminal_fname);

        #if 0
        /* Generate dummy bak log file to handle .log.0 file created
            while the zipper thread was busy compressing logs. This will generate 
            inotify event which guide log rotator thread to perform one more log 
            rotation to rename .log.0 file to .log.1 through file rotation. IF we dont
            do this, user may see ipstrc.log.0 file in /var/log dir which would persists
            until next .bak create event. ipstrc.log.0 is transient file and must not
            exist persistently. We use dummy event to avoid extra thread sync headache.
            Here, zipper thread is signaling the log rotator thread indirectly. */

            generate_dummy_inotify_bak_event ();

        #else
            
            /* Alternate Approach : seems better than above approach */
            sem_wait(&inotify_events_allow_sema);
            rename_all_log0_to_log1_log_file();
            sem_post(&inotify_events_allow_sema);
        
        #endif 

        /* Clear atomic flag */
        atomic_store(&zip_in_progress, false);
        
        /* Unlock the Critical section. */
        pthread_spin_unlock(&operations_on_log_files);
    }
    
    return NULL;
}

/*******************************************************************************
 *                      LOG ROTATION FUNCTIONS
 ******************************************************************************/

/**
 * file_rotate()
 * 
 * Purpose:
 *   Rotates numbered log files by incrementing their numbers.
 *   When the maximum number of files is reached, signals the zipper thread.
 *
 * Process:
 *   1. Delete oldest file (log.N) if it exists
 *   2. Rename files backwards: log.N-1 -> log.N, ..., log.0 -> log.1
 *   3. If log.N-1 was renamed to log.N, signal zipper to compress
 *
 * Example:
 *   Before: log.0, log.1, log.2, log.3, log.4
 *   After:  log.1, log.2, log.3, log.4, log.5
 *           (and log.5 gets queued for compression)
 *
 * @param base_name  Base path of log file (e.g., "var/log/ipmgr")
 */
void 
file_rotate(const char *base_name)
{
    char old_name[FILE_ABS_PATH_NAME_LEN];
    char new_name[FILE_ABS_PATH_NAME_LEN];
    bool ready_to_zip = false;

    /* Delete the oldest file if it exists (e.g., ipmgr.log.5) */
    snprintf(old_name, sizeof(old_name), "%s.log.%d", 
             base_name, DEFAULT_MAX_FILES);

    if (access(old_name, F_OK) == 0) {
        if (remove(old_name) == 0) {
            printf("Deleted oldest file: %s\n", old_name);
        }
    }

    /* 
     * Rotate files backwards (N-1 -> N, N-2 -> N-1, ..., 0 -> 1)
     * This makes room for the new log.0 file
     */
    for (int i = DEFAULT_MAX_FILES - 1; i >= 0; i--) {
        snprintf(old_name, sizeof(old_name), "%s.log.%d", base_name, i);
        snprintf(new_name, sizeof(new_name), "%s.log.%d", base_name, i + 1);

        if (access(old_name, F_OK) == 0) {
            if (rename(old_name, new_name) == 0) {
                printf("Renamed: %s -> %s\n", old_name, new_name);

                /* 
                 * If we just created the highest numbered file (log.N),
                 * mark it for compression
                 */
                if (i == DEFAULT_MAX_FILES - 1) {
                    ready_to_zip = true;
                }
            } else {
                fprintf(stderr, "Error renaming %s to %s: %s\n",
                        old_name, new_name, strerror(errno));
            }
        }
    }

    /* Signal zipper thread if maximum files reached */
    if (ready_to_zip) {
        /* Determine which file type this is */
        int file_idx = get_file_type_index(base_name);
        if (file_idx < 0) {
            fprintf(stderr, "ERROR: Unknown file type for compression: %s\n", base_name);
            return;
        }
        
        /* Store filename for zipper thread with lock protection */
        pthread_spin_lock(&compression_state_lock);
        snprintf(file_compression_state[file_idx].terminal_fname, 
                 sizeof(file_compression_state[file_idx].terminal_fname), 
                 "%s.log.%d", base_name, DEFAULT_MAX_FILES);
        atomic_store(&file_compression_state[file_idx].needs_compression, true);
        pthread_spin_unlock(&compression_state_lock);

        /* Wake up zipper thread to start compression */
        sem_post(&zipper_thread_sync);
        ready_to_zip = false;
    }
}

/**
 * base_file_name_extract()
 * 
 * Purpose:
 *   Extracts the base filename from a .bak file path.
 *   Modifies the input buffer in-place by truncating at the first dot.
 *
 * Example:
 *   Input:  "var/log/ipmgr.log.1234567890.bak"
 *   Output: "var/log/ipmgr"
 *
 * @param base_name  Buffer containing full path (modified in-place)
 * @return 0 on success, -1 on failure
 */
static int
base_file_name_extract(char *base_name)
{
    /* Find last dot (should be ".bak") */
    char *last = strrchr(base_name, '.');

    if (last && strcmp(last, ".bak") == 0) {
        /* Find first dot after the base filename */
        char *first = strchr(base_name, '.');
        
        if (first) {
            /* Truncate at first dot to get base name */
            *first = '\0';
        } else {
            /* Edge case: filename has .bak but no other dots */
            *last = '\0';
        }
        return 0;
    }

    return -1;  /* Not a valid .bak file */
}

/**
 * handle_bak_file()
 * 
 * Purpose:
 *   Handles the creation of a new .bak file by either:
 *   - Normal case: Rename .bak to log.0 and rotate existing files
 *   - Zipper busy: Append .bak content to log.0 (or create log.0 if missing)
 *
 * The "zipper busy" case prevents data loss when compression is taking a
 * long time and new .bak files arrive before rotation can complete.
 *
 * @param bak_file  Filename of the .bak file (e.g., "ipmgr.log.1234567890.bak")
 */
void 
handle_bak_file(const char *bak_file, int findex)
{
    char full_bak_path[FILE_ABS_PATH_NAME_LEN];
    char numbered_file[512];
    char base_name[FILE_ABS_PATH_NAME_LEN];

    /* Build absolute path to .bak file */
    snprintf(full_bak_path, sizeof(full_bak_path), 
             "%s%s", DEFAULT_WATCH_DIR, bak_file);
    
    printf ("%s called to handle : %s\n", __FUNCTION__, full_bak_path);

    /* Verify the .bak file exists before processing */
    if (access(full_bak_path, F_OK) != 0) {
        fprintf(stderr, "ERROR: File not found: %s\n", full_bak_path);
        return;
    }

    if (strstr (full_bak_path, "dummy")) {

        handle_dummy_bak_file_creation(findex);

        if (remove (full_bak_path) != 0) {
            fprintf (stderr, "Error : Deletion of dummy bak file %s failed\n", full_bak_path);
        }
        return;
    }

    /* Extract base name (e.g., "var/log/ipmgr") */
    snprintf(base_name, sizeof(base_name), "%s", full_bak_path);
    if (base_file_name_extract(base_name) < 0) {
        fprintf(stderr, "ERROR: Invalid .bak filename format\n");
        return;
    }

    printf("\n=== Processing .bak file ===\n");
    printf("Full path: %s\n", full_bak_path);
    printf("Base name: %s\n", base_name);

    /*
     * SPECIAL CASE: Zipper thread is busy compressing files
     * 
     * Problem: If we rotate files now, we might interfere with the
     * compression process or lose data if rotation happens too fast.
     * 
     * Solution: Instead of rotating, append the new .bak content to
     * log.0 (or create log.0 if it doesn't exist). This preserves
     * all log data without interfering with compression.
     */
    if (atomic_load(&zip_in_progress)) {
        printf("INFO: Compression in progress, using append strategy\n");

        snprintf(numbered_file, sizeof(numbered_file), "%s.log.0", base_name);

        /* Check if log.0 exists */
        if (access(numbered_file, F_OK) != 0) {
            /* log.0 doesn't exist - simply rename .bak to log.0 */
            if (rename(full_bak_path, numbered_file) == 0) {
                printf("   Created: %s (renamed from .bak)\n", numbered_file);
            } else {
                fprintf(stderr, "ERROR: Failed to rename %s to %s: %s\n",
                        full_bak_path, numbered_file, strerror(errno));
            }
        } else {
            /* 
             * log.0 exists - append .bak content to it using sendfile()
             * for efficient zero-copy kernel-space transfer
             */
            int src_fd = open(full_bak_path, O_RDONLY);
            int dest_fd = open(numbered_file, O_WRONLY | O_APPEND);

            if (src_fd >= 0 && dest_fd >= 0) {
                struct stat stat_buf;

                /* Get source file size */
                if (fstat(src_fd, &stat_buf) == 0) {
                    off_t offset = 0;
                    ssize_t bytes_sent;

                    /* Transfer data using zero-copy sendfile() */
                    while (offset < stat_buf.st_size) {
                        bytes_sent = sendfile(dest_fd, src_fd, &offset, 
                                              stat_buf.st_size - offset);
                        
                        if (bytes_sent <= 0) {
                            /* Handle interrupts and temporary errors */
                            if (errno == EINTR || errno == EAGAIN) {
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
                    if (offset == stat_buf.st_size && remove(full_bak_path) == 0) {
                        printf("   Appended %ld bytes to %s\n", 
                               stat_buf.st_size, numbered_file);
                    } else {
                        fprintf(stderr, "ERROR: Failed to remove %s: %s\n",
                                full_bak_path, strerror(errno));
                    }
                } else {
                    close(src_fd);
                    close(dest_fd);
                    fprintf(stderr, "ERROR: Failed to get file size: %s\n", 
                            strerror(errno));
                }
            } else {
                if (src_fd >= 0) close(src_fd);
                if (dest_fd >= 0) close(dest_fd);
                fprintf(stderr, "ERROR: Failed to open files: %s\n", 
                        strerror(errno));
            }
        }
        return;
    }

    /*
     * NORMAL CASE: Zipper is not busy
     * Perform standard rotation: .bak -> log.0, then rotate all files
     */
    snprintf(numbered_file, sizeof(numbered_file), "%s.log.0", base_name);

    if (rename(full_bak_path, numbered_file) == 0) {
        printf("   Renamed: %s -> %s\n", full_bak_path, numbered_file);
    } else {
        fprintf(stderr, "ERROR: Rename failed: %s -> %s: %s\n",
                full_bak_path, numbered_file, strerror(errno));
    }

    /* lock the Critical section. Any operation on numbered files
        is defined as C.S */
    pthread_spin_lock(&operations_on_log_files);
    /* Rotate existing numbered files */
    file_rotate(base_name);
    /* Unlock the Critical section. */
    pthread_spin_unlock(&operations_on_log_files);
}

/**
 * log_rotate_thread_fn()
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
log_rotate_thread_fn(void *arg)
{
    (void)arg;  /* Unused parameter */
    
    char buffer[BUF_LEN];

    /* Initialize inotify instance */
    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
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

    if (watch_fd < 0) {
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
    while (1) {
        /* Read events from inotify (cancellation point) */
        int length = read(inotify_fd, buffer, BUF_LEN);

        if (length < 0) {
            perror("ERROR: inotify read failed");
            break;
        }

        /* Process all events in the buffer */
        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            /* Only process events with a filename */
            if (event->len > 0) {
                /* Check if this is a .bak file */
                    if (strstr(event->name, ".bak") == NULL ) {
                        i += EVENT_SIZE + event->len;
                        continue;
                    }

                    printf("\n[inotify] event detected: %s\n", event->name);
                    /* Check if it matches one of our target log types */
                    
                    for (int j = 0; j < DEFAULT_NUM_TARGET_FILES; j++) {
                        if (strstr(event->name, target_files[j])) {
                            printf("[inotify] Matches target: %s\n", target_files[j]);
                            sem_wait(&inotify_events_allow_sema);
                            handle_bak_file(event->name, j);
                            sem_post(&inotify_events_allow_sema);
                            break;
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
 * ipmgr_log_rotator_threads_init()
 * 
 * Purpose:
 *   Creates and initializes both worker threads (log rotator and zipper).
 *   Waits for each thread to signal successful initialization before
 *   proceeding.
 *
 * Thread Attributes:
 *   - PTHREAD_CREATE_JOINABLE: Threads can be joined for cleanup
 */
void 
ipmgr_log_rotator_threads_init(void)
{
    pthread_attr_t attr1, attr2;

    /*
     * Create Log Rotator Thread
     * Monitors directory and processes .bak files
     */
    pthread_attr_init(&attr1);
    pthread_attr_setdetachstate(&attr1, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&log_rotator_thread, &attr1, log_rotate_thread_fn, NULL) != 0) {
        fprintf(stderr, "ERROR: Failed to create log rotator thread\n");
        exit(EXIT_FAILURE);
    }
    
    /* Wait for thread to initialize */
    sem_wait(&wait_for_thread_init);
    printf(" Log Rotator thread started\n");

    pthread_attr_destroy(&attr1);

    /*
     * Create Zipper Thread
     * Compresses old log files into archives
     */
    pthread_attr_init(&attr2);
    pthread_attr_setdetachstate(&attr2, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&zipper_thread, &attr2, zip_log_file_thread_fn, NULL) != 0) {
        fprintf(stderr, "ERROR: Failed to create zipper thread\n");
        exit(EXIT_FAILURE);
    }

    /* Wait for thread to initialize */
    sem_wait(&wait_for_thread_init);
    printf(" Zipper thread started\n");

    pthread_attr_destroy(&attr2);
}

/**
 * ipmgr_start_log_rotator_thread()
 * 
 * Purpose:
 *   Public API to start the log rotation system.
 *   Initializes semaphores and creates worker threads.
 */
void 
ipmgr_start_log_rotator_thread(void)
{
    /* Initialize synchronization primitives */
    sem_init(&zipper_thread_sync, 0, 0);      /* Zero semaphore (starts at 0) */
    sem_init(&wait_for_thread_init, 0, 0);    /* Zero semaphore for init sync */
    sem_init(&inotify_events_allow_sema, 0,1); /* Binary semaphore to momentarily 
                                                    pause inotify events*/
    pthread_spin_init(
        &operations_on_log_files, PTHREAD_PROCESS_PRIVATE); /* Spinlock for mutual ex*/
    pthread_spin_init(
        &compression_state_lock, PTHREAD_PROCESS_PRIVATE); /* Spinlock for compression state */
    
    /* Initialize per-file-type compression state */
    for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++) {
        file_compression_state[i].terminal_fname[0] = '\0';
        atomic_store(&file_compression_state[i].needs_compression, false);
    }

    printf("\n========================================\n");
    printf("  Log Rotation System Starting\n");
    printf("========================================\n");

    /* Create and start worker threads */
    ipmgr_log_rotator_threads_init();

    /* Clean up init semaphore (no longer needed) */
    sem_destroy(&wait_for_thread_init);

    printf("========================================\n");
    printf("  System Ready - Monitoring for .bak files\n");
    printf("========================================\n\n");
}

/**
 * ipmgr_stop_log_rotator_thread()
 * 
 * Purpose:
 *   Public API to stop the log rotation system.
 *   Cancels both threads, waits for them to exit, and cleans up resources.
 */
void 
ipmgr_stop_log_rotator_thread(void)
{
    printf("\n========================================\n");
    printf("  Shutting Down Log Rotation System\n");
    printf("========================================\n");

    /* Cancel and join log rotator thread */
    pthread_cancel(log_rotator_thread);
    pthread_join(log_rotator_thread, NULL);
    printf(" Log rotator thread stopped\n");

    /* Cancel and join zipper thread */
    pthread_cancel(zipper_thread);
    pthread_join(zipper_thread, NULL);
    printf(" Zipper thread stopped\n");

    /* Clean up resources */
    sem_destroy(&zipper_thread_sync);
    sem_destroy(&inotify_events_allow_sema);

    pthread_spin_destroy(&operations_on_log_files);
    pthread_spin_destroy(&compression_state_lock);
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
int 
main(int argc, char **argv)
{
    (void)argc;  /* Unused parameters */
    (void)argv;

    /* Start the log rotation system */
    ipmgr_start_log_rotator_thread();

    /* Run for 60 seconds (or forever in production) */
    //sleep(60);

    /* Graceful shutdown */
    //ipmgr_stop_log_rotator_thread();

    pthread_exit(NULL);
    return EXIT_SUCCESS;
}
