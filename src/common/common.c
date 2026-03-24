#include "../include/common.h"
#include <stdint.h>

String string_new(const char* str) {
    String s;
    if (!str) {
        s.len = 0;
        s.data = malloc(1);
        if (s.data) s.data[0] = '\0';
        return s;
    }
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
    if (!s || !str) return;
    size_t str_len = strlen(str);
    if (str_len > SIZE_MAX - s->len - 1) return;
    size_t new_size = s->len + str_len + 1;
    char* new_data = realloc(s->data, new_size);
    if (!new_data) return;
    s->data = new_data;
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
    if (!arr) return;
    if (arr->count >= arr->capacity) {
        if (arr->capacity > SIZE_MAX / 2) return;
        size_t new_capacity = arr->capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(String)) return;
        String* new_items = realloc(arr->items, new_capacity * sizeof(String));
        if (!new_items) return;
        arr->items = new_items;
        arr->capacity = new_capacity;
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
    if (!arr || !item) return;
    if (arr->count >= arr->capacity) {
        if (arr->capacity > SIZE_MAX / 2) return;
        size_t new_capacity = arr->capacity * 2;
        if (arr->item_size > 0 && new_capacity > SIZE_MAX / arr->item_size) return;
        void* new_items = realloc(arr->items, new_capacity * arr->item_size);
        if (!new_items) return;
        arr->items = new_items;
        arr->capacity = new_capacity;
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
