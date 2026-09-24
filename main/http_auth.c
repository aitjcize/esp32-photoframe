#include "http_auth.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

// Kept free of ESP-IDF headers (including mbedtls) so the whole credential
// path can be exercised by host tests rather than only on device.

#define BASIC_PREFIX "Basic "
// "username:password" for the longest password we accept, plus room for a
// generous username. Anything longer cannot be a credential we would match.
#define DECODED_MAX 192

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

// Decodes standard base64 into out (NUL-terminated). Returns the decoded
// length, or -1 on any malformed input or overflow. Rejects embedded NULs so a
// truncated credential cannot compare equal to a shorter secret.
static int b64_decode(const char *in, char *out, size_t out_size)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;

    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '=') {
            break;  // padding: nothing meaningful follows
        }
        int v = b64_value(*p);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char decoded = (char) ((acc >> bits) & 0xFF);
            if (decoded == '\0' || written + 1 >= out_size) {
                return -1;
            }
            out[written++] = decoded;
        }
    }

    out[written] = '\0';
    return (int) written;
}

// Length-safe, data-independent comparison.
static bool constant_time_equal(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    unsigned char diff = (unsigned char) ((alen ^ blen) != 0);
    for (size_t i = 0; i < alen; i++) {
        diff |= (unsigned char) (a[i] ^ b[i < blen ? i : 0]);
    }
    return diff == 0;
}

bool http_auth_header_matches(const char *auth_header, const char *expected)
{
    if (expected == NULL || expected[0] == '\0') {
        return true;  // authentication disabled
    }
    if (auth_header == NULL) {
        return false;
    }
    if (strncasecmp(auth_header, BASIC_PREFIX, strlen(BASIC_PREFIX)) != 0) {
        return false;
    }

    char decoded[DECODED_MAX];
    if (b64_decode(auth_header + strlen(BASIC_PREFIX), decoded, sizeof(decoded)) < 0) {
        return false;
    }

    // "username:password" -- the username is not checked. A password may
    // itself contain ':', so split on the first one only.
    const char *sep = strchr(decoded, ':');
    if (sep == NULL) {
        return false;
    }
    return constant_time_equal(sep + 1, expected);
}

// ---------------------------------------------------------------------------
// Brute-force limiter

#define LIMITER_SLOTS 8

typedef struct {
    bool used;
    uint8_t addr[HTTP_AUTH_ADDR_LEN];
    uint32_t failures;
    int64_t locked_until_ms;
    int64_t last_seen_ms;
} limiter_slot_t;

static limiter_slot_t s_slots[LIMITER_SLOTS];

static limiter_slot_t *limiter_find(const uint8_t *addr)
{
    for (int i = 0; i < LIMITER_SLOTS; i++) {
        if (s_slots[i].used && memcmp(s_slots[i].addr, addr, HTTP_AUTH_ADDR_LEN) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

bool http_auth_limiter_allowed(const uint8_t *addr, int64_t now_ms, int64_t *retry_after_ms)
{
    limiter_slot_t *slot = limiter_find(addr);
    if (slot == NULL || now_ms >= slot->locked_until_ms) {
        return true;
    }
    if (retry_after_ms) {
        *retry_after_ms = slot->locked_until_ms - now_ms;
    }
    return false;
}

void http_auth_limiter_record(const uint8_t *addr, bool success, int64_t now_ms)
{
    limiter_slot_t *slot = limiter_find(addr);
    if (success) {
        if (slot) {
            slot->used = false;
        }
        return;
    }

    if (slot == NULL) {
        // Take a free slot, else evict the client seen least recently.
        slot = &s_slots[0];
        for (int i = 0; i < LIMITER_SLOTS; i++) {
            if (!s_slots[i].used) {
                slot = &s_slots[i];
                break;
            }
            if (s_slots[i].last_seen_ms < slot->last_seen_ms) {
                slot = &s_slots[i];
            }
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->addr, addr, HTTP_AUTH_ADDR_LEN);
    }

    slot->failures++;
    slot->last_seen_ms = now_ms;
    if (slot->failures > HTTP_AUTH_FREE_FAILURES) {
        // 30 s after the first failure past the free ones, doubling from there.
        uint32_t extra = slot->failures - HTTP_AUTH_FREE_FAILURES - 1;
        int64_t lockout = HTTP_AUTH_LOCKOUT_BASE_MS;
        while (extra-- > 0 && lockout < HTTP_AUTH_LOCKOUT_MAX_MS) {
            lockout *= 2;
        }
        if (lockout > HTTP_AUTH_LOCKOUT_MAX_MS) {
            lockout = HTTP_AUTH_LOCKOUT_MAX_MS;
        }
        slot->locked_until_ms = now_ms + lockout;
    }
}

void http_auth_limiter_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
}
