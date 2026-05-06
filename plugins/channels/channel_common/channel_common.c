#include "channel_common.h"
#include <sys/stat.h>

void channel_memory_init(ChannelMemoryStruct* chunk) {
    chunk->memory = malloc(1);
    chunk->memory[0] = '\0';
    chunk->size = 0;
    chunk->done = false;
    chunk->http_status = 0;
    chunk->last_error[0] = '\0';
}

void channel_memory_free(ChannelMemoryStruct* chunk) {
    if (chunk->memory) {
        free(chunk->memory);
        chunk->memory = NULL;
    }
    chunk->size = 0;
}

size_t channel_memory_append_chunk(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    ChannelMemoryStruct* mem = (ChannelMemoryStruct*)userp;

    if (mem->size > 0 && realsize > SIZE_MAX - mem->size) return 0;

    char* ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void channel_apply_dns_config(struct mg_mgr* mgr, const ChannelDNSConfig* dns) {
    if (!mgr || !dns) return;
    mgr->use_system_resolver = dns->use_system_resolver;
    if (dns->use_system_resolver) return;
    if (dns->dns4 && dns->dns4[0]) mgr->dns4.url = dns->dns4;
    if (dns->dns6 && dns->dns6[0]) mgr->dns6.url = dns->dns6;
    if (dns->dns_timeout_ms > 0) mgr->dnstimeout = dns->dns_timeout_ms;
}

void channel_http_event_handler(struct mg_connection* c, int ev, void* ev_data, void* fn_data) {
    ChannelMemoryStruct* chunk = (ChannelMemoryStruct*)fn_data;
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;
        channel_memory_append_chunk(hm->body.buf, hm->body.len, 1, chunk);
        chunk->done = true;
        chunk->http_status = mg_http_status(hm);
        c->is_closing = 1;
    } else if (ev == MG_EV_CLOSE) {
        chunk->done = true;
    }
}

char* channel_read_file_bytes(const char* filepath, size_t* out_len) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    if (length < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = malloc((size_t)length + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    size_t bytes_read = fread(data, 1, (size_t)length, fp);
    data[bytes_read] = '\0';
    fclose(fp);

    if (out_len) *out_len = bytes_read;
    return data;
}

const char* channel_infer_media_type_from_path(const char* path) {
    if (!path) return "application/octet-stream";
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".webp") == 0) return "image/webp";
    if (strcasecmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, ".tiff") == 0 || strcasecmp(ext, ".tif") == 0) return "image/tiff";
    if (strcasecmp(ext, ".heic") == 0) return "image/heic";
    if (strcasecmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(ext, ".amr") == 0) return "audio/amr";
    if (strcasecmp(ext, ".pdf") == 0) return "application/pdf";

    return "application/octet-stream";
}

const char* channel_infer_attachment_type(const char* type_str) {
    if (!type_str) return "file";
    if (strcmp(type_str, "image") == 0) return "image";
    if (strcmp(type_str, "audio") == 0) return "voice";
    if (strcmp(type_str, "video") == 0) return "video";
    if (strcmp(type_str, "file") == 0) return "file";
    return "file";
}

bool channel_parse_attachment_spec(const char* spec, ChannelAttachment* out) {
    if (!spec || !out) return false;

    memset(out, 0, sizeof(ChannelAttachment));

    if (spec[0] == '{') {
        cJSON* json = cJSON_Parse(spec);
        if (!json) return false;

        cJSON* type = cJSON_GetObjectItem(json, "type");
        cJSON* path = cJSON_GetObjectItem(json, "path");
        cJSON* url = cJSON_GetObjectItem(json, "url");
        cJSON* cover = cJSON_GetObjectItem(json, "cover");
        cJSON* duration = cJSON_GetObjectItem(json, "duration");

        if (type && cJSON_IsString(type)) out->type = strdup(type->valuestring);
        if (path && cJSON_IsString(path)) out->path = strdup(path->valuestring);
        if (url && cJSON_IsString(url)) out->url = strdup(url->valuestring);
        if (cover && cJSON_IsString(cover)) out->cover_path = strdup(cover->valuestring);
        if (duration && cJSON_IsNumber(duration)) out->duration = duration->valueint;

        cJSON_Delete(json);
        return true;
    }

    out->path = strdup(spec);
    out->type = strdup("file");
    return true;
}

const char* channel_file_basename(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    if (slash) return slash + 1;
    return path;
}
