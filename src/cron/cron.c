#include "../include/cron.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "../include/common.h"

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

static void* cron_worker(void* arg) {
    CronService* service = (CronService*)arg;

    while (service->running) {
        time_t now = time(NULL);
        
        pthread_mutex_lock(&service->mutex);
        JobNode* current = service->jobs;
        while (current) {
            if (current->job.next_run <= now) {
                // Time to run!
                if (service->callback) {
                    // Release lock during callback to avoid deadlock
                    pthread_mutex_unlock(&service->mutex);
                    service->callback(&current->job);
                    pthread_mutex_lock(&service->mutex);
                }
                
                // Update next_run (Simplified: add 60s or parse schedule)
                // For now, assume one-time if schedule is NULL, or repeat if present
                if (current->job.schedule) {
                    // Very simple parsing: "@every 60s" -> 60
                    int interval = 60;
                    if (strncmp(current->job.schedule, "@every ", 7) == 0) {
                        interval = atoi(current->job.schedule + 7);
                        if (interval <= 0) interval = 60;
                    }
                    current->job.next_run = now + interval;
                } else {
                    // One-time job, mark for deletion or just set far future
                    current->job.next_run = now + 31536000; // +1 year
                    // ideally remove it
                }
            }
            current = current->next;
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
    
    // Set next_run if 0
    if (node->job.next_run == 0) {
        node->job.next_run = time(NULL); // Run immediately
    }
    
    node->next = service->jobs;
    service->jobs = node;
    
    char* result_id = strdup(node->job.id);
    
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
