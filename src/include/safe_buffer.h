/**
 * Safe buffer operations to prevent overflows
 */

#ifndef SAFE_BUFFER_H
#define SAFE_BUFFER_H

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static inline char* safe_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char* result = malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

static inline int safe_snprintf(char* buf, size_t size, const char* fmt, ...) {
    if (!buf || size == 0) return -1;
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    if (ret < 0 || (size_t)ret >= size) {
        buf[size - 1] = '\0';
        return -1;
    }
    return ret;
}

static inline char* safe_strcat_alloc(const char* s1, const char* s2) {
    if (!s1 && !s2) return strdup("");
    if (!s1) return strdup(s2);
    if (!s2) return strdup(s1);
    
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char* result = malloc(len1 + len2 + 1);
    if (!result) return NULL;
    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2);
    result[len1 + len2] = '\0';
    return result;
}

#endif
