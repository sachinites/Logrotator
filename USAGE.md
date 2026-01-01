# Multi-threaded Logger - Usage Guide

## Overview
The modified `logger.c` creates 4 parallel threads, each continuously writing logs to separate log files. Each thread is pinned to a dedicated CPU core for maximum performance and true parallelism.

## Key Features
- **4 Parallel Threads**: Each thread logs to its own file independently
- **CPU Affinity**: Threads are pinned to cores 0, 1, 2, and 3
- **High Volume**: ~1000 logs/second per thread (4000 total logs/sec)
- **Automatic Rotation**: Creates .bak files when logs exceed 10KB

## Thread Configuration

| Thread ID | CPU Core | Log File | Service Name |
|-----------|----------|----------|--------------|
| 0 | Core 0 | var/log/ipstrc.log | IP Stack Trace |
| 1 | Core 1 | var/log/pdtrc.log | Protocol Data Trace |
| 2 | Core 2 | var/log/ipmgr.log | IP Manager |
| 3 | Core 3 | var/log/inttrc.log | Internal Trace |

## Compilation

```bash
gcc -o logger.exe logger.c -pthread -Wall -Wextra
```

## Running the Logger

### Start the logger (runs indefinitely):
```bash
./logger.exe
```

### Stop the logger:
Press `Ctrl+C`

## Stress Testing monitor.c

The intent of this multi-threaded design is to stress test `monitor.c` by generating high-volume log rotations from multiple sources simultaneously.

### Step 1: Start the monitor (in Terminal 1)
```bash
./monitor.exe
```

### Step 2: Start the logger (in Terminal 2)
```bash
./logger.exe
```

### What to observe:
- The logger will rapidly create .bak files (every ~10 seconds per thread)
- The monitor should detect all .bak files and:
  - Rename them to numbered logs (.log.0, .log.1, etc.)
  - Rotate existing numbered logs
  - Compress old logs into tar.gz archives
  - Handle concurrent .bak files from multiple threads

### Monitoring the stress test:
```bash
# Watch log directory in real-time (Terminal 3)
watch -n 1 'ls -lh var/log/ | tail -20'

# Monitor system resources
htop  # Check CPU usage - should see high usage on cores 0-3
```

## Adjusting Stress Level

You can modify the stress level by changing the `usleep()` value in the `logger_thread()` function:

```c
// In logger.c, line ~239:
usleep(1000);   // Current: ~1000 logs/sec per thread (HIGH stress)
usleep(10000);  // Medium: ~100 logs/sec per thread
usleep(100000); // Low: ~10 logs/sec per thread
```

After modifying, recompile with:
```bash
gcc -o logger.exe logger.c -pthread -Wall -Wextra
```

## Expected Behavior

### Normal Operation:
- All 4 threads start and pin to their CPU cores
- Each thread logs continuously to its file
- Console shows every 100th log from each thread
- .bak files are created when logs exceed 10KB

### With monitor.c running:
- .bak files should be quickly processed
- Numbered logs (.log.0 through .log.5) should appear
- When .log.5 is reached, tar.gz archives should be created
- Old numbered logs should be deleted after archiving

## Troubleshooting

### If threads fail to pin to cores:
- Check if your system has at least 4 CPU cores: `nproc`
- Run as root or with appropriate permissions: `sudo ./logger.exe`
- Modify CPU core assignments in main() if needed

### If log files aren't created:
- Ensure `var/log/` directory exists: `mkdir -p var/log`
- Check write permissions: `chmod 755 var/log`

### If monitor.c can't keep up:
- This is expected under extreme stress!
- Reduce logger stress by increasing `usleep()` value
- Monitor queue of .bak files: `ls var/log/*.bak | wc -l`

## Performance Metrics

At default settings (~1000 logs/sec per thread):
- Total log generation: ~4000 logs/second
- Disk writes: ~200-300 KB/second
- CPU usage: Distributed across 4 cores
- .bak file generation: ~1 file every 2-3 seconds per thread

## Code Structure

```
logger.c
├── Thread Management
│   ├── pin_thread_to_core()    - CPU affinity setup
│   └── logger_thread()          - Main thread loop
├── Log Generation
│   ├── generate_ipstrc_log()
│   ├── generate_pdtrc_log()
│   ├── generate_ipmgr_log()
│   └── generate_inttrc_log()
└── File Operations
    ├── write_log()              - Thread-safe log writing
    └── get_file_size()          - Check rotation threshold
```

## Success Criteria

Your stress test is successful when:
1. ✅ All 4 threads start and pin to cores
2. ✅ All 4 log files are being written
3. ✅ .bak files are created regularly
4. ✅ monitor.c detects and processes .bak files
5. ✅ Numbered logs are rotated correctly
6. ✅ tar.gz archives are created
7. ✅ System remains stable under load

Enjoy stress testing!

