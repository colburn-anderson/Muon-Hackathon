#include "jhelp.h"

#include <string.h>
#include <stdlib.h>

/* Returns a pointer just past the value that starts at P, skipping strings
 * (with backslash escapes), nested objects and arrays, and scalars. */
static const char *skip_value(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p == '"') {
        p++;
        while (*p != '\0') {
            if (*p == '\\' && p[1] != '\0') {
                p += 2;
                continue;
            }
            if (*p == '"') {
                return p + 1;
            }
            p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        const char open = *p;
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p != '\0') {
            if (*p == '"') {
                p = skip_value(p);
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return p;
    }
    /* Scalar: number, true, false, null. */
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

/* Locates the value for KEY at the top level of JSON. Returns NULL when the
 * key is absent, so callers can distinguish "missing" from "empty". */
static const char *find_key(const char *json, const char *key) {
    if (json == NULL || key == NULL) {
        return NULL;
    }
    const char *p = strchr(json, '{');
    if (p == NULL) {
        return NULL;
    }
    p++;

    const size_t keylen = strlen(key);

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') {
            p++;
        }
        if (*p == '\0' || *p == '}') {
            return NULL;
        }
        if (*p != '"') {
            /* Not a key: give up rather than guess. */
            return NULL;
        }

        const char *key_start = p + 1;
        p = skip_value(p);                 /* now just past the closing quote */
        const char *key_end = p - 1;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != ':') {
            return NULL;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }

        const char *value = p;
        p = skip_value(p);

        if ((size_t)(key_end - key_start) == keylen &&
            strncmp(key_start, key, keylen) == 0) {
            return value;
        }
    }
}

bool jh_string(const char *json, const char *key, char *out, size_t outsz) {
    const char *v = find_key(json, key);
    if (v == NULL || *v != '"') {
        return false;
    }
    v++;
    size_t o = 0;
    while (*v != '\0' && *v != '"') {
        char c = *v;
        if (c == '\\' && v[1] != '\0') {
            v++;
            c = (*v == 'n') ? '\n' : (*v == 't') ? '\t' : *v;
        }
        if (o + 1 < outsz) {
            out[o++] = c;
        }
        v++;
    }
    out[o] = '\0';
    return true;
}

bool jh_number(const char *json, const char *key, double *out) {
    const char *v = find_key(json, key);
    if (v == NULL) {
        return false;
    }
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) {
        return false;
    }
    *out = d;
    return true;
}

bool jh_bool(const char *json, const char *key, bool *out) {
    const char *v = find_key(json, key);
    if (v == NULL) {
        return false;
    }
    if (strncmp(v, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (strncmp(v, "null", 4) == 0) {
        *out = false;
        return true;
    }
    return false;
}

const char *jh_object(const char *json, const char *key) {
    const char *v = find_key(json, key);
    return (v != NULL && *v == '{') ? v : NULL;
}

bool jh_has(const char *json, const char *key) { return find_key(json, key) != NULL; }
