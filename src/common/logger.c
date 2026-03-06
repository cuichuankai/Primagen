#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

static FILE* log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init(const char* log_file_path) {
    if (log_file) return;
    
    log_file = fopen(log_file_path, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
    }
}

void logger_cleanup() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

static void log_v(const char* level, const char* fmt, va_list args) {
    pthread_mutex_lock(&log_mutex);
    
    // Timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Format message
    char message[4096];
    vsnprintf(message, sizeof(message), fmt, args);
    
    // Remove trailing newline if present, as we add one
    size_t len = strlen(message);
    if (len > 0 && message[len-1] == '\n') {
        message[len-1] = '\0';
    }
    
    // Console output
    FILE* out_stream = (strcmp(level, "ERROR") == 0) ? stderr : stdout;
    // Don't print timestamp/level to console if it's already "formatted" by the app?
    // User requested: "log输出要加上时间戳" (log output needs timestamp)
    // So yes, add it.
    fprintf(out_stream, "[%s] [%s] %s\n", timestamp, level, message);
    
    // File output
    if (log_file) {
        fprintf(log_file, "[%s] [%s] %s\n", timestamp, level, message);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("INFO", fmt, args);
    va_end(args);
}

void log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("ERROR", fmt, args);
    va_end(args);
}

void log_debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_v("DEBUG", fmt, args);
    va_end(args);
}
