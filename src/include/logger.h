#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdbool.h>

// Log levels
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3
} LogLevel;

// Initialize logger with file path
void logger_init(const char* log_file_path);

// Set logger configuration
void logger_set_config(const char* level, bool console_output);

// Get current log level
LogLevel logger_get_level(void);

// Cleanup logger resources
void logger_cleanup();

// Log functions with source location
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_debug(const char* fmt, ...);

// Enhanced logging with source location
#define log_info_loc(fmt, ...) log_info_loc_impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define log_warn_loc(fmt, ...) log_warn_loc_impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define log_error_loc(fmt, ...) log_error_loc_impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define log_debug_loc(fmt, ...) log_debug_loc_impl(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

void log_info_loc_impl(const char* file, int line, const char* func, const char* fmt, ...);
void log_warn_loc_impl(const char* file, int line, const char* func, const char* fmt, ...);
void log_error_loc_impl(const char* file, int line, const char* func, const char* fmt, ...);
void log_debug_loc_impl(const char* file, int line, const char* func, const char* fmt, ...);

// Error detail logging (includes errno)
void log_error_errno(const char* fmt, ...);

#endif // LOGGER_H
