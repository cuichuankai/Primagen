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
