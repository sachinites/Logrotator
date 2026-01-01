#!/bin/bash

# Stress Test Script for ipmgr_log_rotator.c
# Runs both ipmgr_log_rotator and logger together

echo "========================================="
echo "  ipmgr_log_rotator.c Stress Test"
echo "========================================="
echo ""

# Check if executables exist
if [ ! -f "./ipmgr_log_rotator.exe" ]; then
    echo "ERROR: ipmgr_log_rotator.exe not found"
    echo "Please compile ipmgr_log_rotator.c first:"
    echo "  gcc -o ipmgr_log_rotator.exe ipmgr_log_rotator.c -pthread"
    exit 1
fi

if [ ! -f "./logger.exe" ]; then
    echo "ERROR: logger.exe not found"
    echo "Please compile logger.c first:"
    echo "  gcc -o logger.exe logger.c -pthread"
    exit 1
fi

# Ensure log directory exists
mkdir -p var/log

# Clean old logs (optional - comment out if you want to keep them)
echo "Cleaning old log files..."
rm -f var/log/*.log var/log/*.bak 2>/dev/null
echo ""

# Start ipmgr_log_rotator in background
echo "Starting ipmgr_log_rotator.exe..."
./ipmgr_log_rotator.exe &
ipmgr_log_rotator_PID=$!
echo "ipmgr_log_rotator PID: $ipmgr_log_rotator_PID"
sleep 2

# Start logger in foreground
echo ""
echo "Starting logger.exe (Press Ctrl+C to stop)..."
echo "========================================="
echo ""
./logger.exe

# Cleanup on exit
echo ""
echo "Stopping ipmgr_log_rotator.exe..."
kill $ipmgr_log_rotator_PID 2>/dev/null
wait $ipmgr_log_rotator_PID 2>/dev/null

echo ""
echo "Stress test completed!"
echo ""
echo "Check results:"
echo "  ls -lh var/log/"

