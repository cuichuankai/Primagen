#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

// Initialize logger with file path
void logger_init(const char* log_file_path);

// Cleanup logger resources
void logger_cleanup();

// Log functions
void log_info(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_debug(const char* fmt, ...);

#endif // LOGGER_H
