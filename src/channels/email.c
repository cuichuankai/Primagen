#include "../include/channel.h"
#include "../include/config.h"
#include "../bus/message_bus.h"
#include "../include/message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <curl/curl.h>
#include <ctype.h>

typedef struct {
    EmailChannelConfig* config;
    MessageBus* bus;
    bool running;
    pthread_t poll_thread;
} EmailChannelData;

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

static char* extract_header(const char* email_content, const char* header_name) {
    char search[256];
    snprintf(search, sizeof(search), "\n%s:", header_name);
    char* start = strstr(email_content, search);
    if (!start) {
        snprintf(search, sizeof(search), "%s:", header_name);
        if (strncmp(email_content, search, strlen(search)) == 0) {
            start = (char*)email_content;
        }
    } else {
        start++;
    }

    if (!start) return NULL;

    start += strlen(header_name) + 1;
    while (*start == ' ') start++;

    char* end = strchr(start, '\n');
    if (!end) return NULL;

    size_t len = end - start;
    char* value = malloc(len + 1);
    strncpy(value, start, len);
    value[len] = '\0';
    
    if (len > 0 && value[len-1] == '\r') {
        value[len-1] = '\0';
    }

    return value;
}

static char* extract_body(const char* email_content) {
    const char* ptr = email_content;
    while ((ptr = strstr(ptr, "\n\n")) || (ptr = strstr(ptr, "\r\n\r\n"))) {
        if (*ptr == '\r') ptr += 4;
        else ptr += 2;
        return strdup(ptr);
    }
    return strdup("");
}

static void email_poll_thread(void* arg) {
    Channel* self = (Channel*)arg;
    EmailChannelData* data = (EmailChannelData*)self->user_data;
    struct MemoryStruct chunk;
    
    char url_base[512];
    const char* protocol = data->config->imap_use_ssl ? "imaps" : "imap";
    snprintf(url_base, sizeof(url_base), "%s://%s:%d", 
             protocol,
             data->config->imap_host,
             data->config->imap_port);

    while (data->running) {
        chunk.memory = malloc(1);
        chunk.size = 0;

        CURL* curl = curl_easy_init();
        if (curl) {
            char url[512];
            snprintf(url, sizeof(url), "%s/INBOX", url_base);
            
            curl_easy_setopt(curl, CURLOPT_USERNAME, data->config->imap_username);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, data->config->imap_password);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "SEARCH UNSEEN");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                if (strncmp(chunk.memory, "* SEARCH", 8) == 0) {
                    char* p = chunk.memory + 8;
                    while (*p) {
                        while (*p && !isdigit(*p)) p++;
                        if (!*p) break;
                        
                        char* end;
                        long id = strtol(p, &end, 10);
                        p = end;

                        if (id > 0) {
                            struct MemoryStruct msg_chunk;
                            msg_chunk.memory = malloc(1);
                            msg_chunk.size = 0;

                            char fetch_url[512];
                            snprintf(fetch_url, sizeof(fetch_url), "%s/INBOX/;UID=%ld", url_base, id);
                            
                            curl_easy_setopt(curl, CURLOPT_URL, fetch_url);
                            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&msg_chunk);
                            
                            res = curl_easy_perform(curl);
                            if (res == CURLE_OK) {
                                char* from = extract_header(msg_chunk.memory, "From");
                                char* subject = extract_header(msg_chunk.memory, "Subject");
                                char* body = extract_body(msg_chunk.memory);
                                
                                if (from && body) {
                                    char* email_start = strchr(from, '<');
                                    char* email_end = strchr(from, '>');
                                    char* sender_id = NULL;
                                    
                                    if (email_start && email_end) {
                                        *email_end = '\0';
                                        sender_id = strdup(email_start + 1);
                                    } else {
                                        sender_id = strdup(from);
                                    }

                                    bool allowed = true;
                                    StringArray* allow = &data->config->allow_from;
                                    if (allow->count > 0) {
                                        allowed = false;
                                        for (size_t i = 0; i < allow->count; i++) {
                                            if (strcmp(allow->items[i].data, sender_id) == 0) {
                                                allowed = true;
                                                break;
                                            }
                                        }
                                    }

                                    if (allowed) {
                                        printf("[Email] Received from %s: %s\n", sender_id, subject ? subject : "(no subject)");
                                        
                                        size_t content_len = strlen(body) + (subject ? strlen(subject) : 0) + 100;
                                        char* full_content = malloc(content_len);
                                        snprintf(full_content, content_len, "Subject: %s\n\n%s", subject ? subject : "", body);

                                        InboundMessage* msg = inbound_message_new("email", sender_id, full_content);
                                        message_bus_send_inbound(data->bus, msg);
                                        
                                        free(full_content);
                                    } else {
                                        printf("[Email] Ignored from %s (not in allow list)\n", sender_id);
                                    }

                                    free(sender_id);
                                }
                                
                                if (from) free(from);
                                if (subject) free(subject);
                                if (body) free(body);
                            }
                            free(msg_chunk.memory);
                        }
                    }
                }
            }
            curl_easy_cleanup(curl);
        }
        free(chunk.memory);

        sleep(10);
    }
}

static void* email_run(void* arg) {
    email_poll_thread(arg);
    return NULL;
}

static bool email_init(Channel* self, Config* config, MessageBus* bus) {
    EmailChannelData* data = malloc(sizeof(EmailChannelData));
    data->config = &config->channels.email;
    data->bus = bus;
    data->running = false;
    self->user_data = data;
    return true;
}

static void email_start(Channel* self) {
    EmailChannelData* data = (EmailChannelData*)self->user_data;
    if (!data->config->enabled) return;
    
    printf("Starting Email Channel (IMAP: %s, SMTP: %s)...\n", 
           data->config->imap_host, data->config->smtp_host);
    
    data->running = true;
    pthread_create(&data->poll_thread, NULL, email_run, self);
}

static void email_stop(Channel* self) {
    EmailChannelData* data = (EmailChannelData*)self->user_data;
    data->running = false;
    if (data->config->enabled) {
        pthread_join(data->poll_thread, NULL);
    }
}

struct upload_status {
    const char* data;
    size_t bytes_read;
};

static size_t payload_source(void *ptr, size_t size, size_t nmemb, void *userp) {
    struct upload_status *upload_ctx = (struct upload_status *)userp;
    const char *data;
    size_t room = size * nmemb;

    if ((size == 0) || (nmemb == 0) || ((size*nmemb) < 1)) {
        return 0;
    }

    data = upload_ctx->data + upload_ctx->bytes_read;
    size_t len = strlen(data);

    if (len > room) {
        len = room;
    }

    memcpy(ptr, data, len);
    upload_ctx->bytes_read += len;

    return len;
}

static void email_send(Channel* self, OutboundMessage* msg) {
    EmailChannelData* data = (EmailChannelData*)self->user_data;
    if (!data->config->enabled) return;

    CURL *curl;
    CURLcode res = CURLE_OK;
    struct curl_slist *recipients = NULL;
    struct upload_status upload_ctx;

    char* payload_text = malloc(msg->content.len + 512);
    sprintf(payload_text,
            "To: %s\r\n"
            "From: %s\r\n"
            "Subject: Nanobot Reply\r\n"
            "\r\n"
            "%s\r\n",
            msg->chat_id.data,
            data->config->from_address,
            msg->content.data);

    upload_ctx.data = payload_text;
    upload_ctx.bytes_read = 0;

    curl = curl_easy_init();
    if (curl) {
        char url[512];
        const char* protocol = data->config->smtp_use_ssl ? "smtps" : "smtp";
        snprintf(url, sizeof(url), "%s://%s:%d", 
                 protocol,
                 data->config->smtp_host,
                 data->config->smtp_port);

        curl_easy_setopt(curl, CURLOPT_USERNAME, data->config->smtp_username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, data->config->smtp_password);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, data->config->from_address);
        recipients = curl_slist_append(recipients, msg->chat_id.data);
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("[Email] Sent to %s\n", msg->chat_id.data);
        }

        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
    }
    free(payload_text);
}

static void email_destroy(Channel* self) {
    if (self->user_data) {
        free(self->user_data);
    }
    free(self);
}

Channel* channel_create_email() {
    Channel* channel = malloc(sizeof(Channel));
    channel->name = strdup("email");
    channel->init = email_init;
    channel->start = email_start;
    channel->stop = email_stop;
    channel->send = email_send;
    channel->destroy = email_destroy;
    channel->user_data = NULL;
    return channel;
}
