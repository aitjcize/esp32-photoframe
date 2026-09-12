#include "settings_backup.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *k_presets[] = {"triad",     "dingdong", "doublebeep",
                                  "ascending", "softping", "alert"};

static bool preset_is_valid(const char *preset)
{
    if (!preset || !preset[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); i++) {
        if (strcmp(preset, k_presets[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool rotation_mode_is_valid(const char *mode)
{
    return mode && (strcmp(mode, "url") == 0 || strcmp(mode, "storage") == 0 ||
                    strcmp(mode, "sdcard") == 0);
}

static bool source_is_valid(const char *source)
{
    return source && (strcmp(source, "preset") == 0 || strcmp(source, "wav") == 0 ||
                      strcmp(source, "uploaded") == 0);
}

static bool pull_mode_is_valid(const char *mode)
{
    return mode && (strcmp(mode, "once") == 0 || strcmp(mode, "with_rotate") == 0);
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        (*p)++;
    }
}

static bool expect_char(const char **p, char c)
{
    skip_ws(p);
    if (**p != c) {
        return false;
    }
    (*p)++;
    return true;
}

static bool skip_string(const char **p)
{
    skip_ws(p);
    if (**p != '"') {
        return false;
    }
    (*p)++;
    while (**p && **p != '"') {
        if (**p == '\\' && (*p)[1]) {
            *p += 2;
        } else {
            (*p)++;
        }
    }
    if (**p != '"') {
        return false;
    }
    (*p)++;
    return true;
}

static bool skip_value(const char **p);

static bool skip_object_or_array(const char **p, char open_c, char close_c)
{
    skip_ws(p);
    if (**p != open_c) {
        return false;
    }
    (*p)++;
    skip_ws(p);
    if (**p == close_c) {
        (*p)++;
        return true;
    }
    while (**p) {
        if (open_c == '{') {
            if (!skip_string(p) || !expect_char(p, ':') || !skip_value(p)) {
                return false;
            }
        } else if (!skip_value(p)) {
            return false;
        }
        skip_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == close_c) {
            (*p)++;
            return true;
        }
        return false;
    }
    return false;
}

static bool skip_value(const char **p)
{
    skip_ws(p);
    if (**p == '"') {
        return skip_string(p);
    }
    if (**p == '{') {
        return skip_object_or_array(p, '{', '}');
    }
    if (**p == '[') {
        return skip_object_or_array(p, '[', ']');
    }
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        return true;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        return true;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return true;
    }
    if (**p == '-' || (**p >= '0' && **p <= '9')) {
        if (**p == '-') {
            (*p)++;
        }
        while (**p >= '0' && **p <= '9') {
            (*p)++;
        }
        if (**p == '.') {
            (*p)++;
            while (**p >= '0' && **p <= '9') {
                (*p)++;
            }
        }
        return true;
    }
    return false;
}

static bool parse_string(const char **p, char *out, size_t out_len)
{
    skip_ws(p);
    if (**p != '"') {
        return false;
    }
    (*p)++;
    size_t i = 0;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char) **p;
        if (c == '\\') {
            (*p)++;
            if (!**p) {
                return false;
            }
            c = (unsigned char) **p;
            switch (c) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                // Only the two-digit ASCII form we emit (\u00XX) is required.
                if ((*p)[1] && (*p)[2] && (*p)[3] && (*p)[4]) {
                    unsigned int code = 0;
                    if (sscanf(*p + 1, "%4x", &code) == 1 && code <= 0xFF) {
                        c = (unsigned char) code;
                        *p += 4;
                    }
                }
                break;
            default:
                break;
            }
        }
        if (i + 1 >= out_len) {
            return false;
        }
        out[i++] = (char) c;
        (*p)++;
    }
    if (**p != '"') {
        return false;
    }
    (*p)++;
    out[i] = '\0';
    return true;
}

static bool parse_bool(const char **p, bool *out)
{
    skip_ws(p);
    if (strncmp(*p, "true", 4) == 0) {
        *out = true;
        *p += 4;
        return true;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *out = false;
        *p += 5;
        return true;
    }
    return false;
}

static bool parse_cron_array(const char **p, settings_backup_t *out)
{
    skip_ws(p);
    if (**p != '[') {
        return false;
    }
    (*p)++;
    skip_ws(p);
    out->rotate_cron_count = 0;
    if (**p == ']') {
        (*p)++;
        return true;
    }
    while (**p) {
        char rule[SETTINGS_BACKUP_CRON_RULE_MAX_LEN];
        if (!parse_string(p, rule, sizeof(rule))) {
            return false;
        }
        if (rule[0] != '\0' && out->rotate_cron_count < SETTINGS_BACKUP_MAX_CRON_RULES) {
            memcpy(out->rotate_cron[out->rotate_cron_count], rule, sizeof(rule));
            out->rotate_cron_count++;
        }
        skip_ws(p);
        if (**p == ',') {
            (*p)++;
            continue;
        }
        if (**p == ']') {
            (*p)++;
            return true;
        }
        return false;
    }
    return false;
}

static bool append_fmt(char **p, const char *end, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t) (end - *p), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int) (end - *p)) {
        return false;
    }
    *p += n;
    return true;
}

static bool append_quoted(char **p, const char *end, const char *s)
{
    if (!append_fmt(p, end, "\"")) {
        return false;
    }
    if (!s) {
        s = "";
    }
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        const char *esc = NULL;
        char extra[8];
        if (c == '"' || c == '\\') {
            extra[0] = '\\';
            extra[1] = (char) c;
            extra[2] = '\0';
            esc = extra;
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        } else if (c < 0x20) {
            snprintf(extra, sizeof(extra), "\\u%04x", c);
            esc = extra;
        }
        if (esc) {
            if (!append_fmt(p, end, "%s", esc)) {
                return false;
            }
        } else {
            if (*p >= end - 1) {
                return false;
            }
            *(*p)++ = (char) c;
            **p = '\0';
        }
    }
    return append_fmt(p, end, "\"");
}

int settings_backup_serialize(const settings_backup_t *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 2) {
        return -1;
    }

    char *p = out;
    const char *end = out + out_len;
    if (!append_fmt(&p, end, "{\n  \"version\": %d,\n", SETTINGS_BACKUP_VERSION)) {
        return -1;
    }
    if (!append_fmt(&p, end, "  \"auto_rotate\": %s,\n", in->auto_rotate ? "true" : "false")) {
        return -1;
    }
    if (!append_fmt(&p, end, "  \"rotate_cron\": [\n")) {
        return -1;
    }
    int cron_n = in->rotate_cron_count;
    if (cron_n < 0) {
        cron_n = 0;
    }
    if (cron_n > SETTINGS_BACKUP_MAX_CRON_RULES) {
        cron_n = SETTINGS_BACKUP_MAX_CRON_RULES;
    }
    for (int i = 0; i < cron_n; i++) {
        if (!append_fmt(&p, end, "    ")) {
            return -1;
        }
        if (!append_quoted(&p, end, in->rotate_cron[i])) {
            return -1;
        }
        if (!append_fmt(&p, end, "%s\n", (i + 1 < cron_n) ? "," : "")) {
            return -1;
        }
    }
    if (!append_fmt(&p, end, "  ],\n  \"rotation_mode\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->rotation_mode[0] ? in->rotation_mode : "storage")) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"image_url\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->image_url)) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"deep_sleep_enabled\": %s,\n",
                    in->deep_sleep_enabled ? "true" : "false")) {
        return -1;
    }
    if (!append_fmt(&p, end, "  \"chime_enabled\": %s,\n", in->chime_enabled ? "true" : "false")) {
        return -1;
    }
    if (!append_fmt(&p, end, "  \"chime_preset\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->chime_preset[0] ? in->chime_preset : "triad")) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"chime_url\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->chime_url)) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"chime_source\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->chime_source[0] ? in->chime_source : "preset")) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"chime_pull_mode\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->chime_pull_mode[0] ? in->chime_pull_mode : "once")) {
        return -1;
    }
    if (!append_fmt(&p, end, ",\n  \"chime_file\": ")) {
        return -1;
    }
    if (!append_quoted(&p, end, in->chime_file)) {
        return -1;
    }
    if (!append_fmt(&p, end, "\n}\n")) {
        return -1;
    }
    return (int) (p - out);
}

bool settings_backup_parse(const char *json, settings_backup_t *out)
{
    if (!json || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *p = json;
    if (!expect_char(&p, '{')) {
        return false;
    }
    skip_ws(&p);
    if (*p == '}') {
        return true;
    }

    while (*p) {
        char key[32];
        if (!parse_string(&p, key, sizeof(key)) || !expect_char(&p, ':')) {
            return false;
        }

        bool ok = true;
        if (strcmp(key, "auto_rotate") == 0) {
            ok = parse_bool(&p, &out->auto_rotate);
            out->has_auto_rotate = ok;
        } else if (strcmp(key, "deep_sleep_enabled") == 0) {
            ok = parse_bool(&p, &out->deep_sleep_enabled);
            out->has_deep_sleep_enabled = ok;
        } else if (strcmp(key, "chime_enabled") == 0) {
            ok = parse_bool(&p, &out->chime_enabled);
            out->has_chime_enabled = ok;
        } else if (strcmp(key, "rotate_cron") == 0) {
            ok = parse_cron_array(&p, out);
            out->has_rotate_cron = ok && out->rotate_cron_count > 0;
        } else if (strcmp(key, "rotation_mode") == 0) {
            char mode[SETTINGS_BACKUP_ENUM_MAX_LEN];
            ok = parse_string(&p, mode, sizeof(mode));
            if (ok && rotation_mode_is_valid(mode)) {
                if (strcmp(mode, "sdcard") == 0) {
                    strncpy(out->rotation_mode, "storage", sizeof(out->rotation_mode) - 1);
                } else {
                    strncpy(out->rotation_mode, mode, sizeof(out->rotation_mode) - 1);
                }
                out->has_rotation_mode = true;
            }
        } else if (strcmp(key, "image_url") == 0) {
            ok = parse_string(&p, out->image_url, sizeof(out->image_url));
            out->has_image_url = ok;
        } else if (strcmp(key, "chime_preset") == 0) {
            char preset[SETTINGS_BACKUP_PRESET_MAX_LEN];
            ok = parse_string(&p, preset, sizeof(preset));
            if (ok && preset_is_valid(preset)) {
                strncpy(out->chime_preset, preset, sizeof(out->chime_preset) - 1);
                out->has_chime_preset = true;
            }
        } else if (strcmp(key, "chime_url") == 0) {
            ok = parse_string(&p, out->chime_url, sizeof(out->chime_url));
            out->has_chime_url = ok;
        } else if (strcmp(key, "chime_source") == 0) {
            char source[SETTINGS_BACKUP_ENUM_MAX_LEN];
            ok = parse_string(&p, source, sizeof(source));
            if (ok && source_is_valid(source)) {
                strncpy(out->chime_source, source, sizeof(out->chime_source) - 1);
                out->has_chime_source = true;
            }
        } else if (strcmp(key, "chime_pull_mode") == 0) {
            char mode[SETTINGS_BACKUP_ENUM_MAX_LEN];
            ok = parse_string(&p, mode, sizeof(mode));
            if (ok && pull_mode_is_valid(mode)) {
                strncpy(out->chime_pull_mode, mode, sizeof(out->chime_pull_mode) - 1);
                out->has_chime_pull_mode = true;
            }
        } else if (strcmp(key, "chime_file") == 0) {
            ok = parse_string(&p, out->chime_file, sizeof(out->chime_file));
            if (ok && out->chime_file[0] != '\0' && !chime_filename_is_valid(out->chime_file)) {
                out->chime_file[0] = '\0';
                out->has_chime_file = false;
            } else {
                out->has_chime_file = ok;
            }
        } else {
            ok = skip_value(&p);
        }

        if (!ok) {
            return false;
        }

        skip_ws(&p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            return true;
        }
        return false;
    }
    return false;
}

bool settings_backup_should_restore(bool nvs_has_backup_keys, bool sd_backup_present)
{
    return sd_backup_present && !nvs_has_backup_keys;
}
