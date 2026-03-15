#include "llm_provider.h"
#include "../include/common.h"
#include "../include/logger.h"
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/mongoose/mongoose.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Response buffer for Mongoose
struct MemoryStruct {
    char *memory;
    size_t size;
    bool done;
    char last_error[256];
};

/**
 * Parse finish_reason from string
 */
static FinishReason parse_finish_reason(const char* reason) {
    if (!reason) return FINISH_REASON_NONE;
    if (strcmp(reason, "stop") == 0) return FINISH_REASON_STOP;
    if (strcmp(reason, "length") == 0) return FINISH_REASON_LENGTH;
    if (strcmp(reason, "tool_calls") == 0) return FINISH_REASON_TOOL_CALLS;
    if (strcmp(reason, "content_filter") == 0) return FINISH_REASON_CONTENT_FILTER;
    return FINISH_REASON_ERROR;
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
  struct MemoryStruct *ms = (struct MemoryStruct *) c->fn_data;
  if (ev == MG_EV_CONNECT) {
    log_debug("[LLM] MG_EV_CONNECT");
  } else if (ev == MG_EV_TLS_HS) {
    log_debug("[LLM] MG_EV_TLS_HS success");
  } else if (ev == MG_EV_HTTP_HDRS) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    log_debug("[LLM] MG_EV_HTTP_HDRS status=%d", mg_http_status(hm));
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    int status = mg_http_status(hm);
    log_debug("[LLM] MG_EV_HTTP_MSG status=%d body_len=%zu", status, hm->body.len);
    // Append body
    size_t new_size = ms->size + hm->body.len;
    ms->memory = realloc(ms->memory, new_size + 1);
    if (!ms->memory) {
      snprintf(ms->last_error, sizeof(ms->last_error), "OOM in HTTP message buffer");
      ms->done = true;
      c->is_closing = 1;
      return;
    }
    memcpy(ms->memory + ms->size, hm->body.buf, hm->body.len);
    ms->size = new_size;
    ms->memory[ms->size] = '\0';
    c->is_closing = 1;
    ms->done = true;
  } else if (ev == MG_EV_CLOSE) {
    log_debug("[LLM] MG_EV_CLOSE size=%zu", ms->size);
    if (ms->size == 0 && !ms->done) ms->done = true;
  } else if (ev == MG_EV_ERROR) {
      const char* err = ev_data ? (const char*) ev_data : "unknown";
      snprintf(ms->last_error, sizeof(ms->last_error), "%s", err);
      log_error("[LLM] MG_EV_ERROR: %s", ms->last_error);
      ms->done = true;
  }
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
    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';
    
    mg_mgr_init(&mgr);
    
    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) {
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_INVALID_PARAM, "API Key not set (config or OPENAI_API_KEY)");
    }

    // Build JSON Request
    cJSON *root = cJSON_CreateObject();
    
    // Model & Params
    const char* model = (config && config->agent.model) ? config->agent.model : "gpt-4-turbo-preview";
    cJSON_AddStringToObject(root, "model", model);
    
    if (config) {
        cJSON_AddNumberToObject(root, "temperature", config->agent.temperature);
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
        size_t max_history = 30; 
        
        if (session->messages.count > max_history) {
            start_idx = session->messages.count - max_history;
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content", "(Note: Older conversation history has been truncated.)");
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
    log_debug("LLM Request Payload: %s", json_str);
    cJSON_Delete(root);
    
    // Endpoint
    const char* api_base = (config && config->agent.api_base && strlen(config->agent.api_base) > 0) 
                           ? config->agent.api_base 
                           : "https://api.openai.com/v1";
    
    char url[512];
    if (api_base[strlen(api_base) - 1] == '/') {
        snprintf(url, sizeof(url), "%schat/completions", api_base);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", api_base);
    }
    
    // struct mg_tls_opts opts = {0};
    // opts.ca = mg_str("ca.pem"); // Use mg_str for struct mg_str assignment
    
    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        free(json_str);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_NETWORK, "Failed to connect to LLM provider");
    }
    
    // if (mg_url_is_ssl(url)) {
        // struct mg_tls_opts opts = {0};
        // opts.ca = mg_str("ca.pem");
    // }
    struct mg_str host = mg_url_host(url);
    
    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;  // Use built-in TLS without CA verification
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c, 
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url), 
        (int)host.len, host.buf,
        (int) strlen(json_str), 
        api_key,
        json_str
    );
    log_debug("[LLM] Request sent url=%s body_len=%zu", url, strlen(json_str));
    {
        uint64_t start_ms = mg_millis();
        uint64_t last_log_ms = start_ms;
        while (!chunk.done && (mg_millis() - start_ms) < 120000) {
            mg_mgr_poll(&mgr, 1000);
            if (mg_millis() - last_log_ms >= 5000) {
                log_debug("[LLM] waiting response... elapsed=%llums, size=%zu, tls_hs=%d, closing=%d",
                          (unsigned long long) (mg_millis() - start_ms), chunk.size,
                          c->is_tls_hs, c->is_closing);
                last_log_ms = mg_millis();
            }
        }
        if (!chunk.done) {
            snprintf(chunk.last_error, sizeof(chunk.last_error), "timeout after %llums",
                     (unsigned long long) (mg_millis() - start_ms));
            log_error("[LLM] request timeout");
        }
    }
    
    free(json_str);
    mg_mgr_free(&mgr);
    
    log_debug("LLM Response Payload(size=%zu,error=%s): %s", chunk.size,
              chunk.last_error[0] ? chunk.last_error : "none", chunk.memory);

    if (chunk.size == 0) {
        char errbuf[320];
        snprintf(errbuf, sizeof(errbuf), "Empty LLM response (%s)",
                 chunk.last_error[0] ? chunk.last_error : "no payload");
        free(chunk.memory);
        return error_new(ERR_NETWORK, errbuf);
    }

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

    return error_new(ERR_NONE, "");
}

/**
 * Extended LLM provider call with finish_reason detection and usage tracking
 */
Error llm_provider_call_extended(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response) {
    if (!llm_response) return error_new(ERR_INVALID_PARAM, "llm_response is NULL");

    // Initialize response
    llm_response->content = string_new("");
    llm_response->tool_calls = NULL;
    llm_response->tool_calls_count = 0;
    llm_response->finish_reason = FINISH_REASON_NONE;
    llm_response->usage_tokens = 0;

    struct mg_mgr mgr;
    struct MemoryStruct chunk = {0};
    chunk.memory = malloc(1);
    chunk.memory[0] = '\0';

    mg_mgr_init(&mgr);

    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) {
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_INVALID_PARAM, "API Key not set (config or OPENAI_API_KEY)");
    }

    // Build JSON Request
    cJSON *root = cJSON_CreateObject();

    // Model & Params
    const char* model = (config && config->agent.model) ? config->agent.model : "gpt-4-turbo-preview";
    cJSON_AddStringToObject(root, "model", model);

    if (config) {
        cJSON_AddNumberToObject(root, "temperature", config->agent.temperature);
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
        size_t max_history = 30;

        if (session->messages.count > max_history) {
            start_idx = session->messages.count - max_history;
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content", "(Note: Older conversation history has been truncated.)");
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
    log_debug("LLM Request Payload: %s", json_str);
    cJSON_Delete(root);

    // Endpoint
    const char* api_base = (config && config->agent.api_base && strlen(config->agent.api_base) > 0)
                           ? config->agent.api_base
                           : "https://api.openai.com/v1";

    char url[512];
    if (api_base[strlen(api_base) - 1] == '/') {
        snprintf(url, sizeof(url), "%schat/completions", api_base);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", api_base);
    }

    struct mg_connection *c = mg_http_connect(&mgr, url, fn, &chunk);
    if (!c) {
        free(json_str);
        mg_mgr_free(&mgr);
        free(chunk.memory);
        return error_new(ERR_NETWORK, "Failed to connect to LLM provider");
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.0\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        (int) strlen(json_str),
        api_key,
        json_str
    );
    log_debug("[LLM] Request sent url=%s body_len=%zu", url, strlen(json_str));
    {
        uint64_t start_ms = mg_millis();
        uint64_t last_log_ms = start_ms;
        while (!chunk.done && (mg_millis() - start_ms) < 120000) {
            mg_mgr_poll(&mgr, 1000);
            if (mg_millis() - last_log_ms >= 5000) {
                log_debug("[LLM] waiting response... elapsed=%llums, size=%zu, tls_hs=%d, closing=%d",
                          (unsigned long long) (mg_millis() - start_ms), chunk.size,
                          c->is_tls_hs, c->is_closing);
                last_log_ms = mg_millis();
            }
        }
        if (!chunk.done) {
            snprintf(chunk.last_error, sizeof(chunk.last_error), "timeout after %llums",
                     (unsigned long long) (mg_millis() - start_ms));
            log_error("[LLM] request timeout");
        }
    }

    free(json_str);
    mg_mgr_free(&mgr);

    log_debug("LLM Response Payload(size=%zu,error=%s): %s", chunk.size,
              chunk.last_error[0] ? chunk.last_error : "none", chunk.memory);

    if (chunk.size == 0) {
        char errbuf[320];
        snprintf(errbuf, sizeof(errbuf), "Empty LLM response (%s)",
                 chunk.last_error[0] ? chunk.last_error : "no payload");
        free(chunk.memory);
        return error_new(ERR_NETWORK, errbuf);
    }

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

    // Parse finish_reason
    cJSON *finish_reason_item = cJSON_GetObjectItem(choice, "finish_reason");
    if (finish_reason_item && cJSON_IsString(finish_reason_item)) {
        llm_response->finish_reason = parse_finish_reason(finish_reason_item->valuestring);
        log_debug("[LLM] finish_reason: %s", finish_reason_item->valuestring);
    }

    // Parse usage
    cJSON *usage = cJSON_GetObjectItem(json_response, "usage");
    if (usage) {
        cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
        if (total_tokens && cJSON_IsNumber(total_tokens)) {
            llm_response->usage_tokens = total_tokens->valueint;
        }
    }

    // Check for error finish reason
    if (llm_response->finish_reason == FINISH_REASON_ERROR) {
        cJSON *content_item = cJSON_GetObjectItem(message, "content");
        char *content_str = content_item ? content_item->valuestring : "";
        log_error("[LLM] Content filter triggered. Content: %s", content_str ? content_str : "(empty)");
        cJSON_Delete(json_response);
        return error_new(ERR_CONTENT_FILTER, "Content filtered by provider");
    }

    // Check for length limit (incomplete response)
    if (llm_response->finish_reason == FINISH_REASON_LENGTH) {
        log_info("[LLM] Response truncated due to max_tokens limit");
        // Response is still valid, just incomplete
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        llm_response->content = string_new(content->valuestring);
    } else {
        llm_response->content = string_new("");
    }

    cJSON *tcs = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tcs)) {
        llm_response->tool_calls_count = cJSON_GetArraySize(tcs);
        if (llm_response->tool_calls_count > 0) {
            llm_response->tool_calls = malloc(llm_response->tool_calls_count * sizeof(ToolCall));
            int idx = 0;
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcs) {
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                llm_response->tool_calls[idx].id = string_new(cJSON_GetObjectItem(tc, "id")->valuestring);
                llm_response->tool_calls[idx].name = string_new(cJSON_GetObjectItem(func, "name")->valuestring);
                llm_response->tool_calls[idx].arguments = string_new(cJSON_GetObjectItem(func, "arguments")->valuestring);
                idx++;
            }
        } else {
            llm_response->tool_calls = NULL;
        }
    } else {
        llm_response->tool_calls = NULL;
        llm_response->tool_calls_count = 0;
    }

    cJSON_Delete(json_response);

    return error_new(ERR_NONE, "");
}

/**
 * SSE event handler for streaming responses
 */
typedef struct {
    LLMResponse* llm_response;
    StreamCallback on_chunk;
    void* user_data;
    bool done;
    char last_error[256];
    String accumulated_content;
    String recv_buf;  // Buffer for receiving data
} StreamContext;

static void fn_stream(struct mg_connection *c, int ev, void *ev_data) {
    StreamContext *ctx = (StreamContext *) c->fn_data;

    if (ev == MG_EV_CONNECT) {
        log_debug("[LLM Stream] MG_EV_CONNECT");
    } else if (ev == MG_EV_TLS_HS) {
        log_debug("[LLM Stream] MG_EV_TLS_HS success");
    } else if (ev == MG_EV_HTTP_HDRS) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        log_debug("[LLM Stream] MG_EV_HTTP_HDRS status=%d", mg_http_status(hm));
    } else if (ev == MG_EV_READ) {
        // Read available data from socket
        char buf[2048];
        long n = mg_io_recv(c, buf, sizeof(buf));

        if (n > 0) {
            // Append to receive buffer
            size_t old_len = ctx->recv_buf.len;
            ctx->recv_buf.data = realloc(ctx->recv_buf.data, old_len + n + 1);
            if (!ctx->recv_buf.data) return;
            memcpy(ctx->recv_buf.data + old_len, buf, n);
            ctx->recv_buf.len = old_len + n;
            ctx->recv_buf.data[ctx->recv_buf.len] = '\0';

            // Process complete SSE events (lines starting with "data: ")
            char* line_start = ctx->recv_buf.data;
            char* newline;

            while ((newline = strchr(line_start, '\n')) != NULL) {
                *newline = '\0';

                // Check if this is a data line
                if (strncmp(line_start, "data: ", 6) == 0) {
                    char* data = line_start + 6;

                    // Trim \r if present
                    size_t data_len = strlen(data);
                    if (data_len > 0 && data[data_len-1] == '\r') {
                        data[data_len-1] = '\0';
                        data_len--;
                    }

                    // Check for [DONE] marker
                    if (strcmp(data, "[DONE]") == 0) {
                        log_debug("[LLM Stream] Received [DONE]");
                        ctx->done = true;
                        if (ctx->on_chunk) {
                            ctx->on_chunk("", 0, true, ctx->user_data);
                        }
                        break;
                    }

                    // Parse JSON chunk
                    cJSON *json = cJSON_Parse(data);
                    if (json) {
                        // Extract delta content
                        cJSON *choices = cJSON_GetObjectItem(json, "choices");
                        if (choices && cJSON_GetArraySize(choices) > 0) {
                            cJSON *delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
                            if (delta) {
                                cJSON *content_item = cJSON_GetObjectItem(delta, "content");
                                if (content_item && cJSON_IsString(content_item) && content_item->valuestring) {
                                    // Append to accumulated content
                                    string_append(&ctx->accumulated_content, content_item->valuestring);

                                    // Call user callback with chunk
                                    if (ctx->on_chunk) {
                                        ctx->on_chunk(content_item->valuestring, strlen(content_item->valuestring), false, ctx->user_data);
                                    }
                                }

                                // Check for tool calls delta
                                cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
                                if (tool_calls && cJSON_GetArraySize(tool_calls) > 0) {
                                    log_debug("[LLM Stream] Tool call delta received");
                                }
                            }

                            // Check finish_reason
                            cJSON *finish_item = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "finish_reason");
                            if (finish_item && cJSON_IsString(finish_item)) {
                                ctx->llm_response->finish_reason = parse_finish_reason(finish_item->valuestring);
                                log_debug("[LLM Stream] finish_reason: %s", finish_item->valuestring);
                            }
                        }

                        // Extract usage if available
                        cJSON *usage = cJSON_GetObjectItem(json, "usage");
                        if (usage) {
                            cJSON *total = cJSON_GetObjectItem(usage, "total_tokens");
                            if (total && cJSON_IsNumber(total)) {
                                ctx->llm_response->usage_tokens = total->valueint;
                            }
                        }

                        cJSON_Delete(json);
                    }
                }

                line_start = newline + 1;
            }

            // Remove processed data from buffer
            if (line_start > ctx->recv_buf.data) {
                size_t remaining = ctx->recv_buf.len - (line_start - ctx->recv_buf.data);
                if (remaining > 0) {
                    memmove(ctx->recv_buf.data, line_start, remaining);
                }
                ctx->recv_buf.len = remaining;
                ctx->recv_buf.data[ctx->recv_buf.len] = '\0';
            }
        }
    } else if (ev == MG_EV_CLOSE) {
        log_debug("[LLM Stream] MG_EV_CLOSE");
        ctx->done = true;
    } else if (ev == MG_EV_ERROR) {
        const char* err = ev_data ? (const char*) ev_data : "unknown";
        snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", err);
        log_error("[LLM Stream] MG_EV_ERROR: %s", ctx->last_error);
        ctx->done = true;
    }
}

/**
 * Streaming LLM provider call with SSE support
 */
Error llm_provider_call_streaming(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, LLMResponse* llm_response, LLMStreamOptions* options) {
    if (!llm_response) return error_new(ERR_INVALID_PARAM, "llm_response is NULL");
    if (!options || !options->on_chunk) return error_new(ERR_INVALID_PARAM, "streaming options/callback required");

    // Initialize response
    llm_response->content = string_new("");
    llm_response->tool_calls = NULL;
    llm_response->tool_calls_count = 0;
    llm_response->finish_reason = FINISH_REASON_NONE;
    llm_response->usage_tokens = 0;

    StreamContext ctx = {0};
    ctx.llm_response = llm_response;
    ctx.on_chunk = options->on_chunk;
    ctx.user_data = options->user_data;
    ctx.done = false;
    ctx.accumulated_content = string_new("");
    ctx.recv_buf = string_new("");

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    const char* api_key = get_api_key(config);
    if (strlen(api_key) == 0) {
        mg_mgr_free(&mgr);
        string_free(&ctx.accumulated_content);
        string_free(&ctx.recv_buf);
        return error_new(ERR_INVALID_PARAM, "API Key not set (config or OPENAI_API_KEY)");
    }

    // Build JSON Request
    cJSON *root = cJSON_CreateObject();

    // Model & Params
    const char* model = (config && config->agent.model) ? config->agent.model : "gpt-4-turbo-preview";
    cJSON_AddStringToObject(root, "model", model);

    if (config) {
        cJSON_AddNumberToObject(root, "temperature", config->agent.temperature);
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
        size_t max_history = 30;

        if (session->messages.count > max_history) {
            start_idx = session->messages.count - max_history;
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content", "(Note: Older conversation history has been truncated.)");
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

    // Add stream: true for SSE
    cJSON_AddBoolToObject(root, "stream", true);

    char *json_str = cJSON_PrintUnformatted(root);
    log_debug("LLM Stream Request Payload: %s", json_str);
    cJSON_Delete(root);

    // Endpoint
    const char* api_base = (config && config->agent.api_base && strlen(config->agent.api_base) > 0)
                           ? config->agent.api_base
                           : "https://api.openai.com/v1";

    char url[512];
    if (api_base[strlen(api_base) - 1] == '/') {
        snprintf(url, sizeof(url), "%schat/completions", api_base);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", api_base);
    }

    struct mg_connection *c = mg_http_connect(&mgr, url, fn_stream, &ctx);
    if (!c) {
        free(json_str);
        mg_mgr_free(&mgr);
        string_free(&ctx.accumulated_content);
        string_free(&ctx.recv_buf);
        return error_new(ERR_NETWORK, "Failed to connect to LLM provider");
    }

    struct mg_str host = mg_url_host(url);

    struct mg_tls_opts opts = {0};
    opts.ca = mg_str("");
    opts.name = host;
    opts.skip_verification = true;
    if (mg_url_is_ssl(url)) {
        mg_tls_init(c, &opts);
    }

    mg_printf(c,
        "POST %s HTTP/1.1\r\n"
        "Host: %.*s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: text/event-stream\r\n"
        "Authorization: Bearer %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        mg_url_uri(url),
        (int)host.len, host.buf,
        api_key,
        json_str
    );
    log_debug("[LLM Stream] Request sent url=%s", url);

    free(json_str);

    // Wait for stream to complete
    {
        uint64_t start_ms = mg_millis();
        while (!ctx.done && (mg_millis() - start_ms) < 120000) {
            mg_mgr_poll(&mgr, 100);
        }
        if (!ctx.done) {
            snprintf(ctx.last_error, sizeof(ctx.last_error), "timeout");
            log_error("[LLM Stream] request timeout");
        }
    }

    mg_mgr_free(&mgr);

    if (ctx.last_error[0]) {
        string_free(&ctx.accumulated_content);
        string_free(&ctx.recv_buf);
        return error_new(ERR_NETWORK, ctx.last_error);
    }

    // Copy accumulated content to response
    llm_response->content = string_new(ctx.accumulated_content.data);
    string_free(&ctx.accumulated_content);
    string_free(&ctx.recv_buf);

    // Check for content filter
    if (llm_response->finish_reason == FINISH_REASON_CONTENT_FILTER) {
        return error_new(ERR_CONTENT_FILTER, "Content filtered by provider");
    }

    return error_new(ERR_NONE, "");
}
