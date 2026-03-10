#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

static FILE* log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_log_level = 1; // 0=DEBUG, 1=INFO, 2=ERROR
static bool console_enabled = true;

static int get_level_int(const char* level) {
    if (strcmp(level, "DEBUG") == 0) return 0;
    if (strcmp(level, "INFO") == 0) return 1;
    if (strcmp(level, "ERROR") == 0) return 2;
    return 1; // Default INFO
}

void logger_init(const char* log_file_path) {
    if (log_file) return;
    
    log_file = fopen(log_file_path, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
    }
}

void logger_set_config(const char* level, bool console_output) {
    pthread_mutex_lock(&log_mutex);
    current_log_level = get_level_int(level ? level : "INFO");
    console_enabled = console_output;
    pthread_mutex_unlock(&log_mutex);
}

void logger_cleanup() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

static void log_v(const char* level, const char* fmt, va_list args) {
    pthread_mutex_lock(&log_mutex);
    
    // Check level
    int msg_level = get_level_int(level);
    if (msg_level < current_log_level) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }
    
    // Timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Calculate required size
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
    
    // Remove trailing newline if present
    if (len > 0 && message[len-1] == '\n') {
        message[len-1] = '\0';
    }
    
    // Console output
    if (console_enabled) {
        FILE* out_stream = (strcmp(level, "ERROR") == 0) ? stderr : stdout;
        fprintf(out_stream, "[%s] [%s] %s\n", timestamp, level, message);
    }
    
    // File output
    if (log_file) {
        fprintf(log_file, "[%s] [%s] %s\n", timestamp, level, message);
        fflush(log_file);
    }
    
    free(message);
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
