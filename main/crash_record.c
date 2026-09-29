#include "crash_record.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// Xtensa EXCCAUSE names, as the panic handler prints them.
static const char *const EXC_NAMES[] = {
    "IllegalInstruction",
    "Syscall",
    "InstructionFetchError",
    "LoadStoreError",
    "Level1Interrupt",
    "Alloca",
    "IntegerDivideByZero",
    "PCValue",
    "Privileged",
    "LoadStoreAlignment",
    NULL,
    NULL,
    "InstrPDAddrError",
    "LoadStorePIFDataError",
    "InstrPIFAddrError",
    "LoadStorePIFAddrError",
    "InstTLBMiss",
    "InstTLBMultiHit",
    "InstFetchPrivilege",
    NULL,
    "InstrFetchProhibited",
    NULL,
    NULL,
    NULL,
    "LoadStoreTLBMiss",
    "LoadStoreTLBMultihit",
    "LoadStorePrivilege",
    NULL,
    "LoadProhibited",
    "StoreProhibited",
};

// The core dump stores panics raised from an interrupt (watchdog, cache
// error, ...) as 64 + the panic handler's PANIC_RSN_* index.
#define PSEUDO_CAUSE_BASE 64
static const char *const PSEUDO_NAMES[] = {
    "Unknown reason",
    "Unhandled debug exception",
    "Double exception",
    "Unhandled kernel exception",
    "Coprocessor exception",
    "Interrupt wdt timeout on CPU0",
    "Interrupt wdt timeout on CPU1",
    "Cache error",
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// Copy with whitespace runs (the details can span lines) folded to one space.
static void copy_one_line(char *dst, size_t len, const char *src)
{
    size_t n = 0;
    bool pending_space = false;
    for (; *src && n + 1 < len; src++) {
        if (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
            pending_space = n > 0;
            continue;
        }
        if (pending_space) {
            if (n + 2 >= len) {
                break;
            }
            dst[n++] = ' ';
            pending_space = false;
        }
        dst[n++] = *src;
    }
    dst[n] = '\0';
}

void crash_record_set_reason(crash_record_t *rec, const char *details, uint32_t exc_cause,
                             uint32_t exc_vaddr)
{
    if (details) {
        copy_one_line(rec->reason, sizeof(rec->reason), details);
        if (rec->reason[0]) {
            return;
        }
    }
    if (exc_cause < ARRAY_LEN(EXC_NAMES) && EXC_NAMES[exc_cause]) {
        snprintf(rec->reason, sizeof(rec->reason), "%s, vaddr 0x%08x", EXC_NAMES[exc_cause],
                 (unsigned) exc_vaddr);
    } else if (exc_cause >= PSEUDO_CAUSE_BASE &&
               exc_cause - PSEUDO_CAUSE_BASE < ARRAY_LEN(PSEUDO_NAMES)) {
        snprintf(rec->reason, sizeof(rec->reason), "%s",
                 PSEUDO_NAMES[exc_cause - PSEUDO_CAUSE_BASE]);
    } else {
        snprintf(rec->reason, sizeof(rec->reason), "EXCCAUSE %u", (unsigned) exc_cause);
    }
}

void crash_record_format(const crash_record_t *rec, const char *board, char *out, size_t len)
{
    if (len == 0) {
        return;
    }
    char bt[CRASH_RECORD_BT_MAX * 11 + 16] = "";
    size_t n = 0;
    for (unsigned i = 0; i < rec->bt_depth && i < CRASH_RECORD_BT_MAX; i++) {
        n += snprintf(bt + n, sizeof(bt) - n, " 0x%08x", (unsigned) rec->bt[i]);
    }
    if (rec->bt_corrupted) {
        snprintf(bt + n, sizeof(bt) - n, " (corrupted)");
    }

    char found[24] = "clock not set";
    if (rec->found_at > 0) {
        time_t t = (time_t) rec->found_at;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(found, sizeof(found), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }

    snprintf(out, len,
             "%s | task %s, pc 0x%08x, bt%s | fw %s, elf %s, board %s | found %s | dump %u B",
             rec->reason, rec->task, (unsigned) rec->pc, bt,
             rec->firmware[0] ? rec->firmware : "unknown",
             rec->elf_sha[0] ? rec->elf_sha : "unknown", board, found, (unsigned) rec->dump_size);
}
