# Archive Deletion Fix - Explanation

## The Problem

### Original Issue (Before `strstr()`)
Using a **single static `archive` variable** for **all 4 file types** caused the wrong archives to be deleted:

```c
static char archive[256];  // One variable for ALL file types!
```

**What happened:**
```
Time 1: ipstrc compression
  archive = "var/log/ipstrc.log_2025-12-31_21-00-00.tar.gz"

Time 2: pdtrc compression  
  archive = "var/log/pdtrc.log_2025-12-31_21-01-00.tar.gz"  ← Overwrites!
  Delete check: access(archive) → Deletes ipstrc archive by mistake!

Time 3: ipstrc compression again
  Old ipstrc archive is GONE (was deleted by pdtrc)
  ❌ Wrong file deleted!
```

### Your Fix with `strstr()`
You added `strstr(archive, fname)` to prevent deleting wrong file type:

```c
if (archive[0] != '\0' && strstr(archive, fname) && access(archive, F_OK) == 0)
```

**Why this didn't work:**
```
Time 1: ipstrc compression
  archive = "var/log/ipstrc.log_2025-12-31_21-00-00.tar.gz"

Time 2: pdtrc compression
  archive = "var/log/pdtrc.log_2025-12-31_21-01-00.tar.gz"  ← Overwrites!
  
Time 3: ipstrc compression again
  Check: strstr(archive, "ipstrc") 
         strstr("var/log/pdtrc.log_...", "ipstrc") → NULL (not found!)
  ❌ Old ipstrc archive NOT deleted (condition fails)

Time 4: pdtrc compression again  
  Check: strstr(archive, "pdtrc")
         strstr("var/log/pdtrc.log_...", "pdtrc") → Match!
  ✅ Old pdtrc archive deleted correctly
```

**Result:** Only worked for the LAST file type compressed, not all types!

## The Root Cause

The fundamental issue: **One variable trying to track 4 different things!**

```
Single archive[256] variable:
┌────────────────────────────────────┐
│ Shared by:                         │
│  - ipstrc.log                      │
│  - pdtrc.log                       │
│  - ipmgr.log                       │
│  - inttrc.log                      │
│                                    │
│ Problem: Last write wins!          │
│ Other 3 file types lose tracking!  │
└────────────────────────────────────┘
```

## The Solution

Use an **array of archives**, one slot per file type:

```c
// OLD: Single variable
static char archive[256];

// NEW: Array with 4 slots (one per file type)
static char archives[4][256] = {{0}};
//              ^              
//              Index 0 = ipstrc
//              Index 1 = pdtrc  
//              Index 2 = ipmgr
//              Index 3 = inttrc
```

### Implementation

#### Step 1: Helper Function to Find Index
```c
static int get_file_type_index(const char *fname)
{
    // fname could be "ipstrc.log", "pdtrc.log", etc.
    for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++) {
        if (strstr(fname, target_files[i]) != NULL) {
            return i;  // Return index: 0, 1, 2, or 3
        }
    }
    return -1;  // Not found
}
```

#### Step 2: Determine Which Slot to Use
```c
int file_idx = get_file_type_index(fname);
// file_idx = 0 for ipstrc, 1 for pdtrc, 2 for ipmgr, 3 for inttrc
```

#### Step 3: Delete OLD Archive for THIS Specific Type
```c
// Check THIS file type's slot only
if (archives[file_idx][0] != '\0' && access(archives[file_idx], F_OK) == 0) {
    remove(archives[file_idx]);  // Delete old archive for THIS type
}
```

#### Step 4: Store NEW Archive Name in THIS Slot
```c
snprintf(archives[file_idx], sizeof(archives[file_idx]), 
         "%s%s_%s.tar.gz", DEFAULT_WATCH_DIR, fname, timestamp);
```

## How It Works Now

### Example Timeline

```
Time 1: ipstrc compression (file_idx = 0)
  archives[0] = "var/log/ipstrc.log_2025-12-31_21-00-00.tar.gz"
  archives[1] = "" (empty)
  archives[2] = "" (empty)
  archives[3] = "" (empty)

Time 2: pdtrc compression (file_idx = 1)
  archives[0] = "var/log/ipstrc.log_2025-12-31_21-00-00.tar.gz"  ← Still there!
  archives[1] = "var/log/pdtrc.log_2025-12-31_21-01-00.tar.gz"
  archives[2] = "" (empty)
  archives[3] = "" (empty)

Time 3: ipmgr compression (file_idx = 2)
  archives[0] = "var/log/ipstrc.log_2025-12-31_21-00-00.tar.gz"  ← Still there!
  archives[1] = "var/log/pdtrc.log_2025-12-31_21-01-00.tar.gz"   ← Still there!
  archives[2] = "var/log/ipmgr.log_2025-12-31_21-02-00.tar.gz"
  archives[3] = "" (empty)

Time 4: ipstrc compression AGAIN (file_idx = 0)
  Check: archives[0][0] != '\0' → TRUE (has old value)
         access(archives[0]) → File exists
  Action: remove(archives[0]) → ✅ Delete OLD ipstrc archive
  archives[0] = "var/log/ipstrc.log_2025-12-31_21-10-00.tar.gz"  ← New one!
  archives[1] = "var/log/pdtrc.log_2025-12-31_21-01-00.tar.gz"   ← Untouched!
  archives[2] = "var/log/ipmgr.log_2025-12-31_21-02-00.tar.gz"   ← Untouched!
  archives[3] = "" (empty)
```

### Visual Comparison

**BEFORE (Single Variable):**
```
┌─────────────────────────────┐
│  archive[256]               │
├─────────────────────────────┤
│  Last compressed file only! │
│  (Other 3 lose tracking)    │
└─────────────────────────────┘
```

**AFTER (Array of Variables):**
```
┌─────────────────────────────┐
│  archives[0][256]           │  → ipstrc tracking
├─────────────────────────────┤
│  archives[1][256]           │  → pdtrc tracking
├─────────────────────────────┤
│  archives[2][256]           │  → ipmgr tracking
├─────────────────────────────┤
│  archives[3][256]           │  → inttrc tracking
└─────────────────────────────┘
     Each file type independent!
```

## Changes Made

### File: `ipmgr_log_rotator.c`

#### 1. Added Helper Function
```c
static int get_file_type_index(const char *fname)
{
    for (int i = 0; i < DEFAULT_NUM_TARGET_FILES; i++) {
        if (strstr(fname, target_files[i]) != NULL) {
            return i;
        }
    }
    return -1;
}
```

#### 2. Changed Archive Storage
```c
// OLD
static char archive[256];

// NEW  
static char archives[4][256] = {{0}};
```

#### 3. Updated Logic
```c
// Determine which file type
int file_idx = get_file_type_index(fname);

// Delete old archive for THIS type only
if (archives[file_idx][0] != '\0' && access(archives[file_idx], F_OK) == 0) {
    remove(archives[file_idx]);
}

// Store new archive name for THIS type
snprintf(archives[file_idx], ...);

// Use in tar command
snprintf(cmd, sizeof(cmd), "tar -czf \"%s\" ...", archives[file_idx]);

// Print success for THIS type
printf("Archive created: %s\n", archives[file_idx]);
```

## Testing

### Manual Test
```bash
# Run test script
./test_archive_deletion.sh

# Expected output:
# Round 1: Creates 4 archives (one per type)
# Round 2: Deletes old 4, creates new 4
# Final count: 4 archives total (✅ old ones deleted)
```

### Verification
```bash
# After running stress test for a while:
ls -lh var/log/*.tar.gz

# Should see only 4 archives (one per file type), not 8 or 12!
```

## Benefits

1. **✅ Correct Deletion**: Each file type tracks its own archive
2. **✅ No Cross-Contamination**: ipstrc doesn't affect pdtrc, etc.
3. **✅ Memory Efficient**: Only 4 x 256 bytes = 1KB static memory
4. **✅ Thread-Safe**: Static arrays are safe for single-threaded zipper
5. **✅ Maintainable**: Clear array index mapping to file types

## Why strstr() Alone Wasn't Enough

The `strstr()` check was trying to solve the symptom, not the root cause:

**Problem:** One variable tracking multiple things
**Your attempt:** Check if variable matches before deletion
**Why it failed:** Variable only remembered LAST value

**Proper solution:** One variable PER thing being tracked

## Summary

**Root Cause:**
- Single variable couldn't track 4 different file types

**Symptom:**
- Old archives not deleted (or wrong ones deleted)

**Your strstr() Fix:**
- Prevented wrong file type deletion ✅
- But only worked for most recent file type ❌

**Proper Fix:**
- Array with separate slots for each file type ✅
- Each type independently tracks its archive ✅
- All old archives properly deleted ✅

Now each file type (ipstrc, pdtrc, ipmgr, inttrc) has its own dedicated tracking slot, and old archives are deleted correctly!

---

**Testing:** Run `./test_archive_deletion.sh` to verify the fix works correctly.

