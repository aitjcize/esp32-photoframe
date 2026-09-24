#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Check an HTTP Basic Authorization header against the expected password.
 *
 * The username half of the credential is ignored: the password is the whole
 * secret. Comparison is data-independent so a wrong password cannot be
 * narrowed down by timing the reply.
 *
 * @param auth_header   Raw Authorization header value, e.g. `Basic dXNlcjpwdw==`.
 *                      May be NULL.
 * @param expected      Configured password. NULL or empty means authentication
 *                      is disabled, in which case this always returns true.
 * @return true when the request may proceed.
 */
bool http_auth_header_matches(const char *auth_header, const char *expected);

/*
 * Brute-force limiter for the password gate.
 *
 * Each client address gets HTTP_AUTH_FREE_FAILURES wrong passwords for free;
 * after that it is locked out for HTTP_AUTH_LOCKOUT_BASE_MS, doubling with each
 * further failure up to HTTP_AUTH_LOCKOUT_MAX_MS. A correct password clears the
 * client's record. While locked out a client is refused without its password
 * even being checked, so it cannot keep guessing.
 *
 * Only a handful of clients are tracked; when the table is full the one seen
 * least recently is forgotten. Not thread-safe: the HTTP server calls it from
 * its single worker task.
 */

#define HTTP_AUTH_ADDR_LEN 16  // big enough for IPv6; IPv4 is stored mapped
#define HTTP_AUTH_FREE_FAILURES 5
#define HTTP_AUTH_LOCKOUT_BASE_MS 30000
#define HTTP_AUTH_LOCKOUT_MAX_MS (15 * 60 * 1000)

/**
 * @brief Whether a client may attempt authentication now.
 * @param addr            Client address, HTTP_AUTH_ADDR_LEN bytes.
 * @param now_ms          Monotonic time in milliseconds.
 * @param retry_after_ms  Set to the remaining lockout when this returns false.
 */
bool http_auth_limiter_allowed(const uint8_t *addr, int64_t now_ms, int64_t *retry_after_ms);

/**
 * @brief Record the outcome of a password check for a client.
 * @param success  true clears the client's record; false counts a failure.
 */
void http_auth_limiter_record(const uint8_t *addr, bool success, int64_t now_ms);

/** @brief Forget every client (tests, and when the password changes). */
void http_auth_limiter_reset(void);
