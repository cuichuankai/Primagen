#include "llm_provider.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Response buffer for CURL
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        /* out of memory! */
        return 0;
    }
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

static const char* get_api_key(Config* config) {
    if (config && config->agent.api_key && strlen(config->agent.api_key) > 0) {
        return config->agent.api_key;
    }
    const char* key = getenv("OPENAI_API_KEY");
    if (!key) return "";
    return key;
}

Error llm_provider_call(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    // curl_global_init should be called once in main
    curl = curl_easy_init();
    
    if (!curl) {
        free(chunk.memory);
        return error_new(ERR_NETWORK, "Failed to init CURL");
    }

    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) {
        curl_easy_cleanup(curl);
        free(chunk.memory);
        return error_new(ERR_INVALID_PARAM, "API Key not set (config or OPENAI_API_KEY)");
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    
    // Build JSON Request
    cJSON *root = cJSON_CreateObject();
    
    // Model & Params
    const char* model = (config && config->agent.model) ? config->agent.model : "gpt-4-turbo-preview";
    cJSON_AddStringToObject(root, "model", model);
    
    if (config) {
        cJSON_AddNumberToObject(root, "temperature", config->agent.temperature);
        // cJSON_AddNumberToObject(root, "max_tokens", config->agent.max_tokens); // Optional
        if (config->agent.reasoning_effort && strlen(config->agent.reasoning_effort) > 0) {
             cJSON_AddStringToObject(root, "reasoning_effort", config->agent.reasoning_effort);
        }
    }

    // Messages
    cJSON *messages = cJSON_CreateArray();
    
    // 1. System Message
    if (system_prompt && strlen(system_prompt) > 0) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        cJSON_AddItemToArray(messages, sys_msg);
    }
    
    // 2. Session History
    if (session) {
        size_t start_idx = 0;
        // Simple sliding window: keep last 30 messages to manage context
        // In a real implementation, this should be token-based
        size_t max_history = 30; 
        
        if (session->messages.count > max_history) {
            start_idx = session->messages.count - max_history;
            
            // Add a system note about truncation
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content", "(Note: Older conversation history has been truncated. Use the 'memory' tool to access long-term history or consolidate important details.)");
            cJSON_AddItemToArray(messages, note);
        }
        
        for (size_t i = start_idx; i < session->messages.count; i++) {
            Message* msg = *(Message**)dynamic_array_get(&session->messages, i);
            cJSON *json_msg = cJSON_CreateObject();
            
            if (msg->role == ROLE_USER) {
                cJSON_AddStringToObject(json_msg, "role", "user");
                cJSON_AddStringToObject(json_msg, "content", msg->content.data);
            } else if (msg->role == ROLE_ASSISTANT) {
                cJSON_AddStringToObject(json_msg, "role", "assistant");
                if (msg->content.len > 0) {
                    cJSON_AddStringToObject(json_msg, "content", msg->content.data);
                } else {
                    cJSON_AddNullToObject(json_msg, "content");
                }
                
                if (msg->tool_calls_count > 0) {
                    cJSON *tcs = cJSON_CreateArray();
                    for (size_t j = 0; j < msg->tool_calls_count; j++) {
                        cJSON *tc = cJSON_CreateObject();
                        cJSON_AddStringToObject(tc, "id", msg->tool_calls[j].id.data);
                        cJSON_AddStringToObject(tc, "type", "function");
                        
                        cJSON *func = cJSON_CreateObject();
                        cJSON_AddStringToObject(func, "name", msg->tool_calls[j].name.data);
                        cJSON_AddStringToObject(func, "arguments", msg->tool_calls[j].arguments.data);
                        
                        cJSON_AddItemToObject(tc, "function", func);
                        cJSON_AddItemToArray(tcs, tc);
                    }
                    cJSON_AddItemToObject(json_msg, "tool_calls", tcs);
                }
            } else if (msg->role == ROLE_TOOL) {
                cJSON_AddStringToObject(json_msg, "role", "tool");
                cJSON_AddStringToObject(json_msg, "content", msg->content.data);
                if (msg->tool_call_id.len > 0) {
                    cJSON_AddStringToObject(json_msg, "tool_call_id", msg->tool_call_id.data);
                }
                // name is optional in recent OpenAI API for tool role, but good for debugging
                if (msg->name.len > 0) {
                    cJSON_AddStringToObject(json_msg, "name", msg->name.data);
                }
            }
            
            cJSON_AddItemToArray(messages, json_msg);
        }
    }
    
    cJSON_AddItemToObject(root, "messages", messages);
    
    // Tools
    if (tools && tools->count > 0) {
        cJSON *tools_json = cJSON_CreateArray();
        for (size_t i = 0; i < tools->count; i++) {
            cJSON *tool_item = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_item, "type", "function");
            
            cJSON *func = cJSON_CreateObject();
            cJSON_AddStringToObject(func, "name", tools->tools[i].def.name.data);
            cJSON_AddStringToObject(func, "description", tools->tools[i].def.description.data);
            
            cJSON *params = cJSON_Parse(tools->tools[i].def.parameters.data);
            if (params) {
                cJSON_AddItemToObject(func, "parameters", params);
            } else {
                cJSON_AddItemToObject(func, "parameters", cJSON_CreateObject());
            }
            
            cJSON_AddItemToObject(tool_item, "function", func);
            cJSON_AddItemToArray(tools_json, tool_item);
        }
        cJSON_AddItemToObject(root, "tools", tools_json);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    log_info("LLM Request Payload: %s", json_str);
    cJSON_Delete(root);
    
    // Endpoint
    const char* api_base = (config && config->agent.api_base && strlen(config->agent.api_base) > 0) 
                           ? config->agent.api_base 
                           : "https://api.openai.com/v1";
    
    // Construct URL: api_base + "/chat/completions" if api_base doesn't end with it?
    // Usually api_base is "https://api.openai.com/v1" or "http://localhost:8000/v1".
    // We assume api_base is the base URL and we append /chat/completions if it's not a full path.
    // But for simplicity, let's assume api_base DOES NOT include /chat/completions, and we append it.
    // Or we can be smart.
    char url[512];
    // Check if api_base already ends in /chat/completions (LiteLLM sometimes gives full path?)
    // Standard convention: api_base is the root.
    snprintf(url, sizeof(url), "%s/chat/completions", api_base);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    res = curl_easy_perform(curl);
    
    free(json_str);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        free(chunk.memory);
        return error_new(ERR_NETWORK, curl_easy_strerror(res));
    }
    
    log_info("LLM Response Payload: %s", chunk.memory);

    cJSON *json_response = cJSON_Parse(chunk.memory);
    
    if (!json_response) {
        printf("Failed to parse LLM response. Raw: %s\n", chunk.memory);
        free(chunk.memory);
        return error_new(ERR_JSON, "Failed to parse LLM response");
    }
    free(chunk.memory);
    
    // Check for API error
    cJSON *error_obj = cJSON_GetObjectItem(json_response, "error");
    if (error_obj) {
        cJSON *msg_item = cJSON_GetObjectItem(error_obj, "message");
        char *err_msg = msg_item ? msg_item->valuestring : "Unknown API error";
        Error err = error_new(ERR_NETWORK, err_msg);
        cJSON_Delete(json_response);
        return err;
    }
    
    cJSON *choices = cJSON_GetObjectItem(json_response, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(json_response);
        return error_new(ERR_JSON, "No choices in response");
    }
    
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        *response = string_new(content->valuestring);
    } else {
        *response = string_new("");
    }
    
    cJSON *tcs = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tcs)) {
        *tool_calls_count = cJSON_GetArraySize(tcs);
        if (*tool_calls_count > 0) {
            *tool_calls = malloc(*tool_calls_count * sizeof(ToolCall));
            int idx = 0;
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcs) {
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                (*tool_calls)[idx].id = string_new(cJSON_GetObjectItem(tc, "id")->valuestring);
                (*tool_calls)[idx].name = string_new(cJSON_GetObjectItem(func, "name")->valuestring);
                (*tool_calls)[idx].arguments = string_new(cJSON_GetObjectItem(func, "arguments")->valuestring);
                idx++;
            }
        } else {
            *tool_calls = NULL;
        }
    } else {
        *tool_calls = NULL;
        *tool_calls_count = 0;
    }
    
    cJSON_Delete(json_response);
    // curl_global_cleanup(); // Should be done once in main
    
    return error_new(ERR_NONE, "");
}
