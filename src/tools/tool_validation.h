/**
 * Tool validation header
 */

#ifndef TOOL_VALIDATION_H
#define TOOL_VALIDATION_H

/**
 * Validate and cast tool parameters according to JSON schema
 *
 * @param args_json JSON string of arguments
 * @param schema_json JSON schema for validation
 * @param error_msg Output parameter for error message (must be freed by caller)
 * @return Casted JSON string or NULL on error
 */
char* tool_validate_and_cast_params(const char* args_json, const char* schema_json, char** error_msg);

/**
 * Validate tool parameters without casting
 *
 * @param args_json JSON string of arguments
 * @param schema_json JSON schema for validation
 * @return Error message or NULL if valid
 */
char* tool_validate_params(const char* args_json, const char* schema_json);

#endif // TOOL_VALIDATION_H
