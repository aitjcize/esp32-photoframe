#include "chime_name.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

bool chime_filename_is_valid(const char *name)
{
    if (!name || !name[0] || strlen(name) >= CHIME_FILENAME_MAX_LEN) {
        return false;
    }
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    const char *ext = strrchr(name, '.');
    if (!ext || ext == name || strcasecmp(ext, ".wav") != 0) {
        return false;
    }

    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool chime_sanitize_filename(const char *original, char *dest, size_t dest_len)
{
    if (!original || !dest || dest_len < 6) {
        return false;
    }

    const char *base = original;
    const char *slash = strrchr(original, '/');
    if (slash && slash[1]) {
        base = slash + 1;
    }
    const char *bslash = strrchr(base, '\\');
    if (bslash && bslash[1]) {
        base = bslash + 1;
    }

    char tmp[CHIME_FILENAME_MAX_LEN];
    size_t n = 0;
    for (const char *p = base; *p && n + 1 < sizeof(tmp); p++) {
        unsigned char c = (unsigned char) *p;
        if (isalnum(c) || c == '.' || c == '_' || c == '-') {
            tmp[n++] = (char) c;
        } else if (c == ' ') {
            tmp[n++] = '_';
        }
    }
    tmp[n] = '\0';

    while (n > 0 && tmp[n - 1] == '.') {
        tmp[--n] = '\0';
    }
    if (n == 0) {
        return false;
    }

    char *ext = strrchr(tmp, '.');
    if (!ext || strcasecmp(ext, ".wav") != 0) {
        if (n + 4 >= sizeof(tmp)) {
            n = sizeof(tmp) - 5;
            tmp[n] = '\0';
        }
        memcpy(tmp + n, ".wav", 5);
    } else {
        memcpy(ext, ".wav", 5);
    }

    if (!chime_filename_is_valid(tmp)) {
        return false;
    }
    strncpy(dest, tmp, dest_len - 1);
    dest[dest_len - 1] = '\0';
    return true;
}
