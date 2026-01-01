# Logger.c Modifications - Summary

## Overview
Modified `logger.c` from a single-threaded sequential logger to a multi-threaded parallel logger with CPU affinity for stress testing `monitor.c`.

## Changes Made

### 1. Added Threading Support
**New Headers:**
- `#define _GNU_SOURCE` - For CPU affinity functions
- `#include <pthread.h>` - POSIX threads
- `#include <sched.h>` - CPU scheduling/affinity

**New Data Structure:**
```c
typedef struct {
    int thread_id;       // Thread identifier (0-3)
    int cpu_core;        // CPU core to pin to
    const char *log_file;    // Target log file
    const char *thread_name; // Service name
    void (*log_generator)(char*, size_t);  // Log generation function
} thread_data_t;
```

### 2. Thread-Safe Log Writing
**Modified Function:**
- `write_log()` - Changed from using static variable to thread-local parameter
  - Old: `static long old_size = 0;`
  - New: `long *old_size` as parameter
  - Each thread now maintains its own file size tracking

### 3. CPU Affinity Support
**New Function:**
```c
int pin_thread_to_core(int core_id)
```
- Pins calling thread to specific CPU core
- Uses `pthread_setaffinity_np()` for Linux CPU binding
- Ensures true parallelism across cores

### 4. Thread Worker Function
**New Function:**
```c
void *logger_thread(void *arg)
```
- Main loop for each logging thread
- Opens its assigned log file
- Continuously generates and writes logs
- Prints every 100th log to avoid console spam
- High frequency: ~1000 logs/second per thread

### 5. Main Function Redesign
**Old Behavior:**
- Single threaded
- Round-robin with randomness across 4 files
- ~10 logs per second total

**New Behavior:**
- Creates 4 threads with pthread_create()
- Each thread assigned to:
  - Unique CPU core (0, 1, 2, 3)
  - Unique log file
  - Unique log generator function
- ~4000 logs per second total (1000 per thread)
- Waits for all threads with pthread_join()

## Performance Comparison

| Metric | Old (Single-threaded) | New (Multi-threaded) |
|--------|----------------------|---------------------|
| Threads | 1 | 4 |
| CPU Cores Used | 1 | 4 (pinned) |
| Logs/Second | ~10 | ~4000 |
| Parallelism | Sequential | True parallel |
| Stress Level | Low | Very High |

## Architecture

```
Main Process
├── Thread 0 (CPU Core 0) → var/log/ipstrc.log
├── Thread 1 (CPU Core 1) → var/log/pdtrc.log
├── Thread 2 (CPU Core 2) → var/log/ipmgr.log
└── Thread 3 (CPU Core 3) → var/log/inttrc.log

Each Thread:
1. Pin to CPU core
2. Open log file
3. Loop:
   - Generate log message
   - Write to file (check size)
   - If size > 10KB → rename to .bak
   - Sleep 1ms
```

## Key Configuration Parameters

### Thread Count
```c
#define NUM_THREADS 4
```

### Log Rate (per thread)
```c
usleep(1000);  // 1ms = ~1000 logs/sec
```

### Rotation Threshold
```c
#define MAX_LOG_SIZE 10240  // 10KB
```

## Files Modified
1. **logger.c** - Complete rewrite from sequential to parallel

## Files Added
1. **USAGE.md** - Comprehensive usage guide
2. **CHANGES_SUMMARY.md** - This file
3. **stress_test.sh** - Automated test script

## Compilation

```bash
gcc -o logger.exe logger.c -pthread -Wall -Wextra
```

Note: `-pthread` flag is required for POSIX threads support.

## Running the Stress Test

### Option 1: Manual (Two Terminals)
Terminal 1:
```bash
./monitor.exe
```

Terminal 2:
```bash
./logger.exe
```

### Option 2: Automated Script
```bash
./stress_test.sh
```

## Expected Stress Test Results

### Log Generation
- 4 threads running simultaneously
- Each on dedicated CPU core
- ~1000 logs/second per thread
- Total: ~4000 logs/second

### File Rotation
- .bak files created every 2-3 seconds per thread
- ~16-20 .bak files per minute total
- High concurrent load on monitor.c

### Monitor Behavior
Should successfully:
- ✅ Detect all .bak files via inotify
- ✅ Rename to numbered logs (.log.0, .log.1, etc.)
- ✅ Rotate existing numbered logs
- ✅ Compress into tar.gz when reaching .log.5
- ✅ Handle concurrent .bak files from multiple threads

### System Resources
- CPU: High usage on cores 0-3
- Disk I/O: ~200-300 KB/sec writes
- Memory: Minimal (~1-2 MB per thread)

## Adjusting Stress Level

To modify the stress intensity, change the sleep time in `logger_thread()`:

```c
// High stress (current)
usleep(1000);   // ~1000 logs/sec per thread = 4000 total

// Medium stress
usleep(10000);  // ~100 logs/sec per thread = 400 total

// Low stress (original rate)
usleep(100000); // ~10 logs/sec per thread = 40 total
```

## Thread Safety Notes

1. **File Operations**: Each thread writes to its own file - no contention
2. **Random Number Generation**: Uses `rand_r()` with thread-local seed
3. **No Shared State**: Threads are completely independent
4. **CPU Affinity**: Prevents thread migration, ensures consistent performance

## Testing Verification

After running for 30 seconds, verify:
```bash
# Check for .bak files
ls -lh var/log/*.bak

# Check for numbered logs
ls -lh var/log/*.log.[0-9]

# Check for archives
ls -lh var/log/*.tar.gz

# Count total files
ls var/log/ | wc -l
```

## Success Criteria

The modification is successful if:
1. ✅ All 4 threads start without errors
2. ✅ CPU affinity is set for all threads
3. ✅ All 4 log files grow simultaneously
4. ✅ Multiple .bak files created concurrently
5. ✅ monitor.c processes all .bak files correctly
6. ✅ No data loss or corruption
7. ✅ System remains stable under load

## Known Limitations

1. **CPU Cores**: Requires at least 4 CPU cores for optimal performance
2. **Disk Speed**: Very fast SSD recommended for high write rates
3. **Console Spam**: Reduced by printing only every 100th log
4. **Monitor Lag**: Under extreme stress, .bak files may queue up temporarily

## Future Enhancements (Optional)

1. Make thread count configurable via command line
2. Add performance metrics (logs/sec counter)
3. Support for dynamic CPU core selection
4. Configurable log rate per thread
5. Signal handler for graceful shutdown
6. Real-time statistics display

---
**Date Modified:** December 31, 2025
**Tested On:** Linux 6.17.0
**Compiler:** GCC with -pthread support

