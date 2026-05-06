#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

// Basic data types
typedef struct {
    char* data;
    size_t len;
    size_t capacity;
} String;

typedef struct {
    String* items;
    size_t count;
    size_t capacity;
} StringArray;

typedef struct {
    void* items;
    size_t count;
    size_t capacity;
    size_t item_size;
} DynamicArray;

// Environment variable structures (for MCP server config)
typedef struct {
    char* key;
    char* value;
} EnvVar;

typedef struct {
    EnvVar* items;
    size_t count;
    size_t capacity;
} EnvVarArray;

// Error handling
typedef enum {
    ERR_NONE = 0,
    ERR_MEMORY,
    ERR_FILE,
    ERR_NETWORK,
    ERR_JSON,
    ERR_TOOL,
    ERR_INVALID_PARAM,
    ERR_CONTENT_FILTER,
    ERR_CONNECTION,
    ERR_TIMEOUT
} ErrorCode;

typedef struct {
    ErrorCode code;
    char message[256];
} Error;

// Utility functions
String string_new(const char* str);
void string_free(String* s);
String string_copy(const String* s);
void string_append(String* s, const char* str);
int string_equals(const String* a, const String* b);

StringArray string_array_new();
void string_array_free(StringArray* arr);
void string_array_add(StringArray* arr, const char* str);

DynamicArray dynamic_array_new(size_t item_size);
void dynamic_array_free(DynamicArray* arr);
void* dynamic_array_get(DynamicArray* arr, size_t index);
void dynamic_array_add(DynamicArray* arr, void* item);

Error error_new(ErrorCode code, const char* message);
void error_print(const Error* err);

#endif // COMMON_H
