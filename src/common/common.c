#include "../include/common.h"
#include <stdint.h>

String string_new(const char* str) {
    String s;
    if (!str) {
        s.len = 0;
        s.capacity = 16;
        s.data = malloc(s.capacity);
        if (s.data) s.data[0] = '\0';
        return s;
    }
    s.len = strlen(str);
    s.capacity = s.len + 1;
    if (s.capacity < 16) s.capacity = 16;
    s.data = malloc(s.capacity);
    if (!s.data) {
        s.len = 0;
        s.capacity = 0;
        return s;
    }
    strcpy(s.data, str);
    return s;
}

void string_free(String* s) {
    if (!s) return;
    if (s->data) {
        free(s->data);
        s->data = NULL;
        s->len = 0;
        s->capacity = 0;
    }
}

String string_copy(const String* s) {
    if (!s || !s->data) return string_new(NULL);
    return string_new(s->data);
}

void string_append(String* s, const char* str) {
    if (!s || !str) return;
    size_t str_len = strlen(str);
    if (str_len > SIZE_MAX - s->len - 1) return;
    size_t new_len = s->len + str_len;
    if (new_len + 1 > s->capacity) {
        size_t new_cap = s->capacity;
        while (new_cap < new_len + 1) {
            new_cap = new_cap * 2;
            if (new_cap < 16) new_cap = 16;
        }
        char* new_data = realloc(s->data, new_cap);
        if (!new_data) {
            new_data = malloc(new_cap);
            if (!new_data) return;
            memcpy(new_data, s->data, s->len);
            new_data[s->len] = '\0';
            free(s->data);
            s->data = new_data;
            s->capacity = new_cap;
        } else {
            s->data = new_data;
            s->capacity = new_cap;
        }
    }
    strcpy(s->data + s->len, str);
    s->len = new_len;
}

int string_equals(const String* a, const String* b) {
    if (!a || !b) return 0;
    if (!a->data && !b->data) return 1;
    if (!a->data || !b->data) return 0;
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
        if (!new_items) {
            new_items = malloc(new_capacity * sizeof(String));
            if (!new_items) return;
            memcpy(new_items, arr->items, arr->count * sizeof(String));
            free(arr->items);
        }
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
    if (!arr) return;
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
        if (!new_items) {
            new_items = malloc(new_capacity * arr->item_size);
            if (!new_items) return;
            memcpy(new_items, arr->items, arr->count * arr->item_size);
            free(arr->items);
        }
        arr->items = new_items;
        arr->capacity = new_capacity;
    }
    memcpy((char*)arr->items + arr->count * arr->item_size, item, arr->item_size);
    arr->count++;
}

Error error_new(ErrorCode code, const char* message) {
    Error err;
    err.code = code;
    if (message) {
        strncpy(err.message, message, sizeof(err.message) - 1);
    } else {
        err.message[0] = '\0';
    }
    err.message[sizeof(err.message) - 1] = '\0';
    return err;
}

void error_print(const Error* err) {
    if (!err) return;
    fprintf(stderr, "Error %d: %s\n", err->code, err->message);
}

void* xmalloc(size_t size) {
    void* p = malloc(size);
    if (!p) {
        fprintf(stderr, "FATAL: Out of memory (requested %zu bytes)\n", size);
        abort();
    }
    return p;
}

char* xstrdup(const char* s) {
    if (!s) {
        fprintf(stderr, "FATAL: xstrdup called with NULL\n");
        abort();
    }
    size_t len = strlen(s);
    char* p = malloc(len + 1);
    if (!p) {
        fprintf(stderr, "FATAL: Out of memory in xstrdup (len=%zu)\n", len);
        abort();
    }
    memcpy(p, s, len + 1);
    return p;
}
