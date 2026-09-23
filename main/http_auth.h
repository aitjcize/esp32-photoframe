#pragma once

#include <stdbool.h>

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
