#!/bin/bash

# Test script to verify archive deletion works correctly per file type

echo "========================================="
echo "  Testing Archive Deletion Fix"
echo "========================================="
echo ""

cd /home/vm/logger

# Clean up
echo "Cleaning old files..."
rm -f var/log/*.log var/log/*.bak var/log/*.log.* var/log/*.tar.gz 2>/dev/null
mkdir -p var/log
echo ""

# Create test scenario
echo "Creating test scenario:"
echo "  1. Creating dummy log files for each type"
echo ""

# Create log files for each type (5 files each)
for type in ipstrc pdtrc ipmgr inttrc; do
    for i in 1 2 3 4 5; do
        echo "Test log content for $type file $i" > var/log/${type}.log.$i
    done
    echo "  - Created 5 files for $type"
done

echo ""
echo "Files created:"
ls -lh var/log/*.log.* | wc -l
echo ""

# Start the log rotator
echo "Starting log rotator..."
./ipmgr_log_rotator.exe &
ROTATOR_PID=$!
echo "Rotator PID: $ROTATOR_PID"
sleep 2

# Trigger compression for each file type by creating .log.5 files
echo ""
echo "Triggering compressions..."
echo ""

echo "Round 1: Creating first set of archives"
echo "----------------------------------------"
for type in ipstrc pdtrc ipmgr inttrc; do
    # Touch the .log.5 file to trigger compression
    touch var/log/${type}.log.5
    sleep 3  # Wait for compression
done

echo ""
echo "Archives after Round 1:"
ls -lh var/log/*.tar.gz 2>/dev/null
echo ""

sleep 5

echo "Round 2: Creating second set of archives"
echo "----------------------------------------"
echo "This should DELETE the old archives and create new ones"
echo ""

# Recreate files and trigger again
for type in ipstrc pdtrc ipmgr inttrc; do
    for i in 1 2 3 4 5; do
        echo "Test log content ROUND 2 for $type file $i" > var/log/${type}.log.$i
    done
done

sleep 2

for type in ipstrc pdtrc ipmgr inttrc; do
    touch var/log/${type}.log.5
    sleep 3
done

echo ""
echo "Archives after Round 2:"
ls -lh var/log/*.tar.gz 2>/dev/null
echo ""

echo "Expected: Only 4 archives (one per file type, newest ones)"
echo "If there are 8 archives, the deletion failed!"
echo ""

ARCHIVE_COUNT=$(ls var/log/*.tar.gz 2>/dev/null | wc -l)
echo "Archive count: $ARCHIVE_COUNT"

if [ "$ARCHIVE_COUNT" -eq 4 ]; then
    echo "✅ SUCCESS: Old archives were deleted correctly!"
elif [ "$ARCHIVE_COUNT" -eq 8 ]; then
    echo "❌ FAILURE: Old archives were NOT deleted!"
else
    echo "⚠️  WARNING: Unexpected archive count: $ARCHIVE_COUNT"
fi

echo ""
echo "Stopping rotator..."
kill $ROTATOR_PID 2>/dev/null
wait $ROTATOR_PID 2>/dev/null

echo ""
echo "Test complete!"

