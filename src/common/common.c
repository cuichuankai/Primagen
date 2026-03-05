#include "common.h"

String string_new(const char* str) {
    String s;
    s.len = strlen(str);
    s.data = malloc(s.len + 1);
    if (!s.data) {
        s.len = 0;
        return s;
    }
    strcpy(s.data, str);
    return s;
}

void string_free(String* s) {
    if (s->data) {
        free(s->data);
        s->data = NULL;
        s->len = 0;
    }
}

String string_copy(const String* s) {
    return string_new(s->data);
}

void string_append(String* s, const char* str) {
    size_t str_len = strlen(str);
    s->data = realloc(s->data, s->len + str_len + 1);
    if (!s->data) return;
    strcpy(s->data + s->len, str);
    s->len += str_len;
}

int string_equals(const String* a, const String* b) {
    return strcmp(a->data, b->data) == 0;
}

StringArray string_array_new() {
    StringArray arr;
    arr.count = 0;
    arr.capacity = 8;
    arr.items = malloc(arr.capacity * sizeof(String));
    return arr;
}

void string_array_free(StringArray* arr) {
    for (size_t i = 0; i < arr->count; i++) {
        string_free(&arr->items[i]);
    }
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void string_array_add(StringArray* arr, const char* str) {
    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        arr->items = realloc(arr->items, arr->capacity * sizeof(String));
    }
    arr->items[arr->count] = string_new(str);
    arr->count++;
}

DynamicArray dynamic_array_new(size_t item_size) {
    DynamicArray arr;
    arr.count = 0;
    arr.capacity = 8;
    arr.item_size = item_size;
    arr.items = malloc(arr.capacity * item_size);
    return arr;
}

void dynamic_array_free(DynamicArray* arr) {
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->item_size = 0;
}

void* dynamic_array_get(DynamicArray* arr, size_t index) {
    if (index >= arr->count) return NULL;
    return (char*)arr->items + index * arr->item_size;
}

void dynamic_array_add(DynamicArray* arr, void* item) {
    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        arr->items = realloc(arr->items, arr->capacity * arr->item_size);
    }
    memcpy((char*)arr->items + arr->count * arr->item_size, item, arr->item_size);
    arr->count++;
}

Error error_new(ErrorCode code, const char* message) {
    Error err;
    err.code = code;
    strncpy(err.message, message, sizeof(err.message) - 1);
    err.message[sizeof(err.message) - 1] = '\0';
    return err;
}

void error_print(const Error* err) {
    fprintf(stderr, "Error %d: %s\n", err->code, err->message);
}