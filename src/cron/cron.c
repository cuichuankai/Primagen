#include "../include/logger.h"
#include "../include/cron.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "../include/common.h"
#include "../vendor/cJSON/cJSON.h"

// Internal Job Structure
typedef struct JobNode {
    CronJob job;
    struct JobNode* next;
} JobNode;

struct CronService {
    char* store_path;
    CronCallback callback;
    bool running;
    pthread_t worker_thread;
    pthread_mutex_t mutex;
    JobNode* jobs;
};

typedef enum {
    CRON_SCHEDULE_INVALID = 0,
    CRON_SCHEDULE_ONE_TIME = 1,
    CRON_SCHEDULE_RECURRING = 2
} CronScheduleType;

static bool parse_daily_schedule(const char* schedule, int* hour, int* minute) {
    if (!schedule || !hour || !minute) return false;
    int parsed_hour = -1;
    int parsed_minute = -1;
    char dom[8] = {0};
    char mon[8] = {0};
    char dow[8] = {0};
    char extra[8] = {0};
    int count = sscanf(schedule, "%d %d %7s %7s %7s %7s",
                       &parsed_minute, &parsed_hour, dom, mon, dow, extra);
    if (count != 5) return false;
    if (parsed_hour < 0 || parsed_hour > 23 || parsed_minute < 0 || parsed_minute > 59) return false;
    if (strcmp(dom, "*") != 0 || strcmp(mon, "*") != 0 || strcmp(dow, "*") != 0) return false;
    *hour = parsed_hour;
    *minute = parsed_minute;
    return true;
}

static time_t next_daily_run(time_t now, int hour, int minute) {
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = 0;
    time_t next = mktime(&local_tm);
    if (next <= now) {
        local_tm.tm_mday += 1;
        next = mktime(&local_tm);
    }
    return next;
}

static CronScheduleType cron_schedule_next_run(const char* schedule, time_t now, time_t* out_next_run) {
    if (!out_next_run) return CRON_SCHEDULE_INVALID;
    if (!schedule || schedule[0] == '\0') {
        *out_next_run = now;
        return CRON_SCHEDULE_ONE_TIME;
    }
    if (strncmp(schedule, "@every ", 7) == 0) {
        int interval = atoi(schedule + 7);
        if (interval <= 0) interval = 60;
        *out_next_run = now + interval;
        return CRON_SCHEDULE_RECURRING;
    }
    if (strncmp(schedule, "@in ", 4) == 0) {
        int delay = atoi(schedule + 4);
        if (delay <= 0) delay = 1;
        *out_next_run = now + delay;
        return CRON_SCHEDULE_ONE_TIME;
    }
    if (strncmp(schedule, "@at ", 4) == 0) {
        long timestamp = atol(schedule + 4);
        *out_next_run = (time_t)timestamp;
        return CRON_SCHEDULE_ONE_TIME;
    }
    int hour = 0;
    int minute = 0;
    if (parse_daily_schedule(schedule, &hour, &minute)) {
        *out_next_run = next_daily_run(now, hour, minute);
        return CRON_SCHEDULE_RECURRING;
    }
    return CRON_SCHEDULE_INVALID;
}

static void free_job(CronJob* job) {
    if (!job) return;
    free(job->id);
    free(job->name);
    free(job->payload_message);
    free(job->channel);
    free(job->to);
    free(job->schedule);
}

static CronJob copy_job(const CronJob* src) {
    CronJob dst;
    dst.id = src->id ? strdup(src->id) : NULL;
    dst.name = src->name ? strdup(src->name) : NULL;
    dst.payload_message = src->payload_message ? strdup(src->payload_message) : NULL;
    dst.channel = src->channel ? strdup(src->channel) : NULL;
    dst.to = src->to ? strdup(src->to) : NULL;
    dst.deliver = src->deliver;
    dst.next_run = src->next_run;
    dst.schedule = src->schedule ? strdup(src->schedule) : NULL;
    return dst;
}

static void save_jobs(CronService* service) {
    if (!service || !service->store_path) return;
    
    // log_debug("[Cron] Saving jobs to %s", service->store_path);

    cJSON* root = cJSON_CreateObject();
    cJSON* jobs_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "jobs", jobs_array);

    JobNode* current = service->jobs;
    while (current) {
        cJSON* job_obj = cJSON_CreateObject();
        if (current->job.id) cJSON_AddStringToObject(job_obj, "id", current->job.id);
        if (current->job.name) cJSON_AddStringToObject(job_obj, "name", current->job.name);
        if (current->job.payload_message) cJSON_AddStringToObject(job_obj, "payload_message", current->job.payload_message);
        if (current->job.channel) cJSON_AddStringToObject(job_obj, "channel", current->job.channel);
        if (current->job.to) cJSON_AddStringToObject(job_obj, "to", current->job.to);
        cJSON_AddBoolToObject(job_obj, "deliver", current->job.deliver);
        cJSON_AddNumberToObject(job_obj, "next_run", (double)current->job.next_run);
        if (current->job.schedule) cJSON_AddStringToObject(job_obj, "schedule", current->job.schedule);
        
        cJSON_AddItemToArray(jobs_array, job_obj);
        current = current->next;
    }

    char* json_str = cJSON_Print(root);
    if (json_str) {
        FILE* fp = fopen(service->store_path, "w");
        if (fp) {
            fputs(json_str, fp);
            fclose(fp);
            log_debug("[Cron] Jobs saved successfully.");
        } else {
            log_error("[Cron] Failed to open store file for writing: %s", service->store_path);
        }
        free(json_str);
    }
    cJSON_Delete(root);
}

static void load_jobs(CronService* service) {
    if (!service || !service->store_path) return;
    
    log_info("[Cron] Loading jobs from %s", service->store_path);

    FILE* fp = fopen(service->store_path, "r");
    if (!fp) {
        log_info("[Cron] No existing job store found.");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (length <= 0) {
        fclose(fp);
        return;
    }

    char* data = malloc(length + 1);
    if (!data) {
        fclose(fp);
        return;
    }

    fread(data, 1, length, fp);
    data[length] = 0;
    fclose(fp);

    cJSON* root = cJSON_Parse(data);
    free(data);

    if (!root) {
        log_error("[Cron] Failed to parse job store JSON.");
        return;
    }

    cJSON* jobs_array = cJSON_GetObjectItem(root, "jobs");
    if (cJSON_IsArray(jobs_array)) {
        int count = 0;
        cJSON* item;
        cJSON_ArrayForEach(item, jobs_array) {
            CronJob job;
            memset(&job, 0, sizeof(job));
            
            cJSON* id = cJSON_GetObjectItem(item, "id");
            if (cJSON_IsString(id)) job.id = strdup(id->valuestring);
            
            cJSON* name = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(name)) job.name = strdup(name->valuestring);
            
            cJSON* payload = cJSON_GetObjectItem(item, "payload_message");
            if (cJSON_IsString(payload)) job.payload_message = strdup(payload->valuestring);
            
            cJSON* channel = cJSON_GetObjectItem(item, "channel");
            if (cJSON_IsString(channel)) job.channel = strdup(channel->valuestring);
            
            cJSON* to = cJSON_GetObjectItem(item, "to");
            if (cJSON_IsString(to)) job.to = strdup(to->valuestring);
            
            cJSON* deliver = cJSON_GetObjectItem(item, "deliver");
            if (cJSON_IsBool(deliver)) job.deliver = cJSON_IsTrue(deliver);
            
            cJSON* next_run = cJSON_GetObjectItem(item, "next_run");
            if (cJSON_IsNumber(next_run)) job.next_run = (time_t)next_run->valuedouble;
            
            cJSON* schedule = cJSON_GetObjectItem(item, "schedule");
            if (cJSON_IsString(schedule)) job.schedule = strdup(schedule->valuestring);

            // Add to list directly (bypass mutex/save for bulk load)
            JobNode* node = malloc(sizeof(JobNode));
            node->job = job; // Takes ownership of allocated strings
            node->next = service->jobs;
            service->jobs = node;
            count++;
        }
        log_info("[Cron] Loaded %d jobs.", count);
    }
    cJSON_Delete(root);
}

static void* cron_worker(void* arg) {
    CronService* service = (CronService*)arg;

    while (service->running) {
        time_t now = time(NULL);
        bool modified = false;
        
        pthread_mutex_lock(&service->mutex);
        JobNode* current = service->jobs;
        JobNode* prev = NULL;
        
        while (current) {
            if (current->job.next_run <= now) {
                if (service->callback) {
                    pthread_mutex_unlock(&service->mutex);
                    service->callback(&current->job);
                    pthread_mutex_lock(&service->mutex);
                }

                bool remove = false;
                time_t next_run = 0;
                CronScheduleType schedule_type = cron_schedule_next_run(current->job.schedule, now, &next_run);
                if (schedule_type == CRON_SCHEDULE_RECURRING) {
                    current->job.next_run = next_run;
                    modified = true;
                } else if (schedule_type == CRON_SCHEDULE_ONE_TIME) {
                    remove = true;
                } else {
                    if (current->job.schedule) {
                        log_error("[Cron] Invalid schedule in runtime, removing job: %s", current->job.schedule);
                    }
                    remove = true;
                }

                if (remove) {
                    JobNode* to_free = current;
                    if (prev) {
                        prev->next = current->next;
                        current = prev->next;
                    } else {
                        service->jobs = current->next;
                        current = service->jobs;
                    }
                    free_job(&to_free->job);
                    free(to_free);
                    modified = true;
                    continue; // Skip prev update
                }
            }
            prev = current;
            if (current) current = current->next;
        }
        
        if (modified) {
            save_jobs(service);
        }
        
        pthread_mutex_unlock(&service->mutex);

        sleep(1); 
    }

    return NULL;
}

CronService* cron_service_create(const char* store_path) {
    CronService* service = malloc(sizeof(CronService));
    if (!service) return NULL;

    service->store_path = strdup(store_path);
    service->callback = NULL;
    service->running = false;
    service->jobs = NULL;

    pthread_mutex_init(&service->mutex, NULL);
    
    // Load jobs
    load_jobs(service);

    return service;
}

void cron_service_destroy(CronService* service) {
    if (!service) return;

    cron_service_stop(service);
    
    JobNode* current = service->jobs;
    while (current) {
        JobNode* next = current->next;
        free_job(&current->job);
        free(current);
        current = next;
    }

    free(service->store_path);
    pthread_mutex_destroy(&service->mutex);
    free(service);
}

bool cron_service_start(CronService* service) {
    if (!service || service->running) return false;

    service->running = true;

    if (pthread_create(&service->worker_thread, NULL, cron_worker, service) != 0) {
        service->running = false;
        return false;
    }

    return true;
}

void cron_service_stop(CronService* service) {
    if (!service || !service->running) return;

    service->running = false;
    pthread_join(service->worker_thread, NULL);
}

void cron_service_set_callback(CronService* service, CronCallback callback) {
    if (!service) return;
    service->callback = callback;
}

char* cron_service_add_job(CronService* service, const CronJob* job) {
    if (!service || !job) return NULL;

    pthread_mutex_lock(&service->mutex);
    
    JobNode* node = malloc(sizeof(JobNode));
    node->job = copy_job(job);
    
    // Generate ID if missing
    if (!node->job.id) {
        char id[32];
        snprintf(id, sizeof(id), "job_%ld", time(NULL));
        node->job.id = strdup(id);
    }
    
    time_t now = time(NULL);
    CronScheduleType schedule_type = cron_schedule_next_run(node->job.schedule, now, &node->job.next_run);
    if (schedule_type == CRON_SCHEDULE_INVALID) {
        log_error("[Cron] Invalid schedule format: %s", node->job.schedule ? node->job.schedule : "(null)");
        free_job(&node->job);
        free(node);
        pthread_mutex_unlock(&service->mutex);
        return NULL;
    }

    node->next = service->jobs;
    service->jobs = node;
    
    char* result_id = strdup(node->job.id);
    
    save_jobs(service); // Save after adding
    
    pthread_mutex_unlock(&service->mutex);
    return result_id;
}

bool cron_service_remove_job(CronService* service, const char* job_id) {
    if (!service || !job_id) return false;

    pthread_mutex_lock(&service->mutex);
    
    JobNode* current = service->jobs;
    JobNode* prev = NULL;
    
    while (current) {
        if (current->job.id && strcmp(current->job.id, job_id) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                service->jobs = current->next;
            }
            free_job(&current->job);
            free(current);
            
            save_jobs(service); // Save after removing
            
            pthread_mutex_unlock(&service->mutex);
            return true;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&service->mutex);
    return false;
}

char* cron_service_status(CronService* service) {
    if (!service) return NULL;

    pthread_mutex_lock(&service->mutex);
    int count = 0;
    JobNode* current = service->jobs;
    while (current) {
        count++;
        current = current->next;
    }
    pthread_mutex_unlock(&service->mutex);

    char* status = malloc(256);
    if (status) {
        snprintf(status, 256, "Cron service: %s, jobs: %d",
                service->running ? "running" : "stopped", count);
    }
    return status;
}
