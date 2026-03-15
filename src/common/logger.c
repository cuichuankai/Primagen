#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

/* Global logger state */
static FILE* log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_log_level = 1; /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */
static bool console_enabled = true;

/* Forward declaration */
static void log_v_with_loc(const char* level, const char* fmt, va_list args,
                           const char* file, int line, const char* func);

/* Convert log level string to integer */
static int get_level_int(const char* level) {
    if (strcmp(level, "DEBUG") == 0) return 0;
    if (strcmp(level, "INFO") == 0) return 1;
    if (strcmp(level, "WARN") == 0) return 2;
    if (strcmp(level, "ERROR") == 0) return 3;
    return 1; /* Default INFO */
}

/* Initialize logger with file path */
void logger_init(const char* log_file_path) {
    if (log_file) return;

    log_file = fopen(log_file_path, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
        return;
    }
    log_debug("[Logger] Initialized with file: %s", log_file_path);
}

/* Set logger configuration (level and console output) */
void logger_set_config(const char* level, bool console_output) {
    pthread_mutex_lock(&log_mutex);
    current_log_level = get_level_int(level ? level : "INFO");
    console_enabled = console_output;
    pthread_mutex_unlock(&log_mutex);
    log_debug("[Logger] Config updated: level=%s, console=%s", level ? level : "INFO", console_output ? "on" : "off");
}

LogLevel logger_get_level(void) {
    pthread_mutex_lock(&log_mutex);
    LogLevel level = (LogLevel)current_log_level;
    pthread_mutex_unlock(&log_mutex);
    return level;
}

/* Cleanup logger resources */
void logger_cleanup() {
    if (log_file) {
        log_debug("[Logger] Closing log file");
        fclose(log_file);
        log_file = NULL;
    }
}

/* Internal log function without location info */
static void log_v(const char* level, const char* fmt, va_list args) {
    log_v_with_loc(level, fmt, args, NULL, -1, NULL);
}

/* Internal log function with source location (file, line, function) */
static void log_v_with_loc(const char* level, const char* fmt, va_list args,
                           const char* file, int line, const char* func) {
    pthread_mutex_lock(&log_mutex);

    /* Check level - skip if message level is below current threshold */
    int msg_level = get_level_int(level);
    if (msg_level < current_log_level) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    /* Get current timestamp */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);

    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Calculate required buffer size */
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    char* message = malloc(len + 1);
    if (!message) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    vsnprintf(message, len + 1, fmt, args);

    /* Remove trailing newline if present */
    if (len > 0 && message[len-1] == '\n') {
        message[len-1] = '\0';
    }

    /* Console output */
    if (console_enabled) {
        FILE* out_stream = (msg_level >= LOG_LEVEL_ERROR) ? stderr : stdout;
        if (file && line > 0) {
            /* Extract filename from path */
            const char* fname = strrchr(file, '/');
            fname = fname ? fname + 1 : file;
            fprintf(out_stream, "[%s] [%s] [%s:%d %s] %s\n", timestamp, level, fname, line, func ? func : "?", message);
        } else {
            fprintf(out_stream, "[%s] [%s] %s\n", timestamp, level, message);
        }
    }

    /* File output */
    if (log_file) {
        if (file && line > 0) {
            const char* fname = strrchr(file, '/');
            fname = fname ? fname + 1 : file;
            fprintf(log_file, "[%s] [%s] [%s:%d %s] %s\n", timestamp, level, fname, line, func ? func : "?", message);
        } else {
            fprintf(log_file, "[%s] [%s] %s\n", timestamp, level, message);
        }
        fflush(log_file);
    }

    free(message);
    pthread_mutex_unlock(&log_mutex);
}

/* Log INFO level message */
void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("INFO", fmt, args);
    va_end(args);
}

/* Log WARN level message */
void log_warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("WARN", fmt, args);
    va_end(args);
}

/* Log ERROR level message */
void log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("ERROR", fmt, args);
    va_end(args);
}

/* Log DEBUG level message */
void log_debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("DEBUG", fmt, args);
    va_end(args);
}

/* Log ERROR level message with errno details */
void log_error_errno(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    pthread_mutex_lock(&log_mutex);

    /* Get errno message */
    char errno_msg[256];
    strerror_r(errno, errno_msg, sizeof(errno_msg));

    /* Calculate required size for full message */
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len >= 0) {
        char* message = malloc(len + 1 + sizeof(errno_msg) + 20);
        if (message) {
            vsnprintf(message, len + 1, fmt, args);
            /* Append errno info */
            snprintf(message + strlen(message), sizeof(errno_msg) + 20, " [errno=%d: %s]", errno, errno_msg);

            /* Output to console */
            if (console_enabled) {
                fprintf(stderr, "[%s] %s\n", "ERROR", message);
            }

            /* Output to file */
            if (log_file) {
                fprintf(log_file, "[%s] %s\n", "ERROR", message);
                fflush(log_file);
            }

            free(message);
        }
    }

    pthread_mutex_unlock(&log_mutex);
    va_end(args);
}

/* Location-aware logging implementations */
void log_info_loc_impl(const char* file, int line, const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v_with_loc("INFO", fmt, args, file, line, func);
    va_end(args);
}

void log_warn_loc_impl(const char* file, int line, const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v_with_loc("WARN", fmt, args, file, line, func);
    va_end(args);
}

void log_error_loc_impl(const char* file, int line, const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v_with_loc("ERROR", fmt, args, file, line, func);
    va_end(args);
}

void log_debug_loc_impl(const char* file, int line, const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v_with_loc("DEBUG", fmt, args, file, line, func);
    va_end(args);
}
