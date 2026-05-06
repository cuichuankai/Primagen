#ifndef CHANNEL_COMMON_H
#define CHANNEL_COMMON_H

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../../src/vendor/mongoose/mongoose.h"
#include "../../../src/vendor/cJSON/cJSON.h"

typedef struct {
    char* dns4;
    char* dns6;
    int dns_timeout_ms;
    bool use_system_resolver;
} ChannelDNSConfig;

typedef struct {
    char* memory;
    size_t size;
    bool done;
    int http_status;
    char last_error[256];
} ChannelMemoryStruct;

typedef struct {
    char* type;
    char* path;
    char* url;
    char* cover_path;
    int duration;
} ChannelAttachment;

void channel_memory_init(ChannelMemoryStruct* chunk);
void channel_memory_free(ChannelMemoryStruct* chunk);
size_t channel_memory_append_chunk(void* contents, size_t size, size_t nmemb, void* userp);

void channel_apply_dns_config(struct mg_mgr* mgr, const ChannelDNSConfig* dns);

void channel_http_event_handler(struct mg_connection* c, int ev, void* ev_data, void* fn_data);

char* channel_read_file_bytes(const char* filepath, size_t* out_len);

const char* channel_infer_media_type_from_path(const char* path);
const char* channel_infer_attachment_type(const char* type_str);

bool channel_parse_attachment_spec(const char* spec, ChannelAttachment* out);

const char* channel_file_basename(const char* path);

#endif
