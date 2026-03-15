/**
 * Tool validation and parameter casting utilities
 */

#include "../include/common.h"
#include "../vendor/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>

#define MAX_VALIDATION_ERRORS 16
#define MAX_ERROR_MSG 256

typedef struct {
    char** errors;
    int count;
    int capacity;
} ValidationErrorList;

static ValidationErrorList* validation_list_new() {
    ValidationErrorList* list = malloc(sizeof(ValidationErrorList));
    list->capacity = 8;
    list->count = 0;
    list->errors = malloc(list->capacity * sizeof(char*));
    return list;
}

static void validation_list_add(ValidationErrorList* list, const char* fmt, ...) {
    if (!list || !fmt) return;
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->errors = realloc(list->errors, list->capacity * sizeof(char*));
    }

    char* msg = malloc(MAX_ERROR_MSG);
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, MAX_ERROR_MSG, fmt, args);
    va_end(args);

    list->errors[list->count++] = msg;
}

static void validation_list_free(ValidationErrorList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->errors[i]);
    }
    free(list->errors);
    free(list);
}

static char* validation_list_join(ValidationErrorList* list, const char* separator) {
    if (!list || list->count == 0) return NULL;

    // Calculate total length
    size_t total_len = 0;
    for (int i = 0; i < list->count; i++) {
        total_len += strlen(list->errors[i]) + strlen(separator);
    }

    char* result = malloc(total_len + 1);
    result[0] = '\0';

    for (int i = 0; i < list->count; i++) {
        strcat(result, list->errors[i]);
        if (i < list->count - 1) {
            strcat(result, separator);
        }
    }

    return result;
}

/**
 * Cast a JSON value to string
 */
static char* cast_to_string(cJSON* val) {
    if (!val) return NULL;

    if (cJSON_IsString(val)) {
        return strdup(val->valuestring);
    }

    if (cJSON_IsNumber(val)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", val->valuedouble);
        return strdup(buf);
    }

    if (cJSON_IsBool(val)) {
        return strdup(cJSON_IsTrue(val) ? "true" : "false");
    }

    if (cJSON_IsNull(val)) {
        return strdup("null");
    }

    // For arrays/objects, return JSON string
    char* json = cJSON_PrintUnformatted(val);
    return json;
}

/**
 * Cast a JSON value to integer
 */
static int cast_to_int(cJSON* val, bool* success) {
    *success = false;
    if (!val) return 0;

    if (cJSON_IsNumber(val)) {
        *success = true;
        return val->valueint;
    }

    if (cJSON_IsString(val) && val->valuestring) {
        char* endptr;
        long v = strtol(val->valuestring, &endptr, 10);
        if (*endptr == '\0') {
            *success = true;
            return (int)v;
        }
    }

    return 0;
}

/**
 * Cast a JSON value to number (double)
 */
static double cast_to_number(cJSON* val, bool* success) {
    *success = false;
    if (!val) return 0.0;

    if (cJSON_IsNumber(val)) {
        *success = true;
        return val->valuedouble;
    }

    if (cJSON_IsString(val) && val->valuestring) {
        char* endptr;
        double v = strtod(val->valuestring, &endptr);
        if (*endptr == '\0') {
            *success = true;
            return v;
        }
    }

    return 0.0;
}

/**
 * Cast a JSON value to boolean
 */
static bool cast_to_bool(cJSON* val, bool* success) {
    *success = false;
    if (!val) return false;

    if (cJSON_IsBool(val)) {
        *success = true;
        return cJSON_IsTrue(val);
    }

    if (cJSON_IsString(val) && val->valuestring) {
        if (strcasecmp(val->valuestring, "true") == 0 ||
            strcasecmp(val->valuestring, "yes") == 0 ||
            strcmp(val->valuestring, "1") == 0) {
            *success = true;
            return true;
        }
        if (strcasecmp(val->valuestring, "false") == 0 ||
            strcasecmp(val->valuestring, "no") == 0 ||
            strcmp(val->valuestring, "0") == 0) {
            *success = true;
            return false;
        }
    }

    if (cJSON_IsNumber(val)) {
        *success = true;
        return val->valueint != 0;
    }

    return false;
}

/**
 * Validate and cast tool parameters
 * Returns casted JSON object or NULL on error
 * Sets error_msg if validation fails
 */
char* tool_validate_and_cast_params(const char* args_json, const char* schema_json, char** error_msg) {
    if (error_msg) *error_msg = NULL;

    cJSON* args = cJSON_Parse(args_json);
    if (!args) {
        if (error_msg) *error_msg = strdup("Invalid JSON arguments");
        return NULL;
    }

    if (!schema_json || strlen(schema_json) == 0) {
        // No schema, return args as-is
        char* result = cJSON_PrintUnformatted(args);
        cJSON_Delete(args);
        return result;
    }

    cJSON* schema = cJSON_Parse(schema_json);
    if (!schema) {
        cJSON_Delete(args);
        if (error_msg) *error_msg = strdup("Invalid schema JSON");
        return NULL;
    }

    ValidationErrorList* errors = validation_list_new();
    cJSON* result = cJSON_CreateObject();

    // Get properties from schema
    cJSON* properties = cJSON_GetObjectItem(schema, "properties");
    cJSON* required = cJSON_GetObjectItem(schema, "required");

    // Check required fields
    if (cJSON_IsArray(required)) {
        cJSON* req_item;
        cJSON_ArrayForEach(req_item, required) {
            if (cJSON_IsString(req_item)) {
                const char* req_name = req_item->valuestring;
                cJSON* arg_val = cJSON_GetObjectItem(args, req_name);
                if (!arg_val) {
                    validation_list_add(errors, "missing required parameter: %s", req_name);
                }
            }
        }
    }

    // Process each property in schema
    if (cJSON_IsObject(properties)) {
        cJSON* prop_item;
        cJSON_ArrayForEach(prop_item, properties) {
            const char* prop_name = prop_item->string;
            cJSON* prop_schema = prop_item;

            cJSON* arg_val = cJSON_GetObjectItem(args, prop_name);
            cJSON* type_item = cJSON_GetObjectItem(prop_schema, "type");

            if (!arg_val) {
                // Optional parameter not provided, skip
                continue;
            }

            const char* expected_type = type_item ? type_item->valuestring : NULL;

            // Cast and validate based on expected type
            if (expected_type && strcmp(expected_type, "string") == 0) {
                char* str_val = cast_to_string(arg_val);
                if (str_val) {
                    cJSON_AddStringToObject(result, prop_name, str_val);

                    // Check minLength
                    cJSON* min_len = cJSON_GetObjectItem(prop_schema, "minLength");
                    if (min_len && cJSON_IsNumber(min_len)) {
                        if ((int)strlen(str_val) < min_len->valueint) {
                            validation_list_add(errors, "%s must be at least %d characters", prop_name, min_len->valueint);
                        }
                    }

                    // Check maxLength
                    cJSON* max_len = cJSON_GetObjectItem(prop_schema, "maxLength");
                    if (max_len && cJSON_IsNumber(max_len)) {
                        if ((int)strlen(str_val) > max_len->valueint) {
                            validation_list_add(errors, "%s must be at most %d characters", prop_name, max_len->valueint);
                        }
                    }

                    // Check enum
                    cJSON* enum_item = cJSON_GetObjectItem(prop_schema, "enum");
                    if (cJSON_IsArray(enum_item)) {
                        bool found = false;
                        cJSON* enum_val;
                        cJSON_ArrayForEach(enum_val, enum_item) {
                            if (cJSON_IsString(enum_val) && strcmp(enum_val->valuestring, str_val) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            validation_list_add(errors, "%s must be one of the allowed values", prop_name);
                        }
                    }

                    free(str_val);
                }
            } else if (expected_type && strcmp(expected_type, "integer") == 0) {
                bool success = false;
                int int_val = cast_to_int(arg_val, &success);
                if (success) {
                    cJSON_AddNumberToObject(result, prop_name, int_val);

                    // Check minimum
                    cJSON* min_item = cJSON_GetObjectItem(prop_schema, "minimum");
                    if (min_item && cJSON_IsNumber(min_item)) {
                        if (int_val < min_item->valueint) {
                            validation_list_add(errors, "%s must be >= %d", prop_name, min_item->valueint);
                        }
                    }

                    // Check maximum
                    cJSON* max_item = cJSON_GetObjectItem(prop_schema, "maximum");
                    if (max_item && cJSON_IsNumber(max_item)) {
                        if (int_val > max_item->valueint) {
                            validation_list_add(errors, "%s must be <= %d", prop_name, max_item->valueint);
                        }
                    }
                } else {
                    validation_list_add(errors, "%s must be an integer", prop_name);
                }
            } else if (expected_type && (strcmp(expected_type, "number") == 0 || strcmp(expected_type, "float") == 0)) {
                bool success = false;
                double num_val = cast_to_number(arg_val, &success);
                if (success) {
                    cJSON_AddNumberToObject(result, prop_name, num_val);
                } else {
                    validation_list_add(errors, "%s must be a number", prop_name);
                }
            } else if (expected_type && strcmp(expected_type, "boolean") == 0) {
                bool success = false;
                bool bool_val = cast_to_bool(arg_val, &success);
                if (success) {
                    cJSON_AddBoolToObject(result, prop_name, bool_val);
                } else {
                    validation_list_add(errors, "%s must be a boolean", prop_name);
                }
            } else if (expected_type && strcmp(expected_type, "array") == 0) {
                if (cJSON_IsArray(arg_val)) {
                    cJSON* arr = cJSON_CreateArray();
                    cJSON* arr_item;
                    cJSON_ArrayForEach(arr_item, arg_val) {
                        // Simple pass-through for array items
                        cJSON* copy = cJSON_Duplicate(arr_item, 1);
                        if (copy) cJSON_AddItemToArray(arr, copy);
                    }
                    cJSON_AddItemToObject(result, prop_name, arr);
                } else {
                    validation_list_add(errors, "%s must be an array", prop_name);
                }
            } else if (expected_type && strcmp(expected_type, "object") == 0) {
                if (cJSON_IsObject(arg_val)) {
                    cJSON* copy = cJSON_Duplicate(arg_val, 1);
                    if (copy) cJSON_AddItemToObject(result, prop_name, copy);
                } else {
                    validation_list_add(errors, "%s must be an object", prop_name);
                }
            } else {
                // Unknown or no type specified, pass through
                cJSON* copy = cJSON_Duplicate(arg_val, 1);
                if (copy) cJSON_AddItemToObject(result, prop_name, copy);
            }
        }
    } else {
        // No properties in schema, copy all args
        cJSON* item;
        cJSON_ArrayForEach(item, args) {
            cJSON* copy = cJSON_Duplicate(item, 1);
            if (copy && item->string) {
                cJSON_AddItemToObject(result, item->string, copy);
            }
        }
    }

    // Generate result
    char* result_str = NULL;
    if (errors->count > 0) {
        char* err_str = validation_list_join(errors, "; ");
        if (error_msg) *error_msg = err_str;
        cJSON_Delete(result);
        result_str = NULL;
    } else {
        result_str = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
    }

    validation_list_free(errors);
    cJSON_Delete(args);
    cJSON_Delete(schema);

    return result_str;
}

/**
 * Simple parameter validation (without casting)
 * Returns error message or NULL if valid
 */
char* tool_validate_params(const char* args_json, const char* schema_json) {
    char* error_msg = NULL;
    tool_validate_and_cast_params(args_json, schema_json, &error_msg);
    return error_msg;
}
