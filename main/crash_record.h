#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The last crash, summarised from the core dump the next boot finds. Stored in
// NVS as a blob (newest wins), so the layout is versioned: bump
// CRASH_RECORD_VERSION whenever a field changes.
//
// Kept free of ESP-IDF headers so the formatting is host-tested.

#define CRASH_RECORD_VERSION 1
#define CRASH_RECORD_BT_MAX 8
#define CRASH_RECORD_LINE_MAX 512

typedef struct {
    uint8_t version;
    uint8_t bt_depth;
    bool bt_corrupted;
    uint32_t pc;
    uint32_t bt[CRASH_RECORD_BT_MAX];
    uint32_t dump_size;
    int64_t found_at;  // boot that found the dump (seconds, UTC); 0 if the clock wasn't set
    char task[16];
    char firmware[32];  // "" when the dump came from another build than the running one
    char elf_sha[9];    // first 8 hex chars of the crashed build's ELF SHA-256
    char reason[128];
} crash_record_t;

// Fill rec->reason: the panic details when the dump has them (abort, assert,
// stack overflow; may be NULL or empty), else the Xtensa exception cause.
void crash_record_set_reason(crash_record_t *rec, const char *details, uint32_t exc_cause,
                             uint32_t exc_vaddr);

// One line with everything needed to decode the crash, as logged at boot and
// copied into bug reports.
void crash_record_format(const crash_record_t *rec, const char *board, char *out, size_t len);
