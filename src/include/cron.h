#ifndef CRON_H
#define CRON_H

#include <stdbool.h>
#include <time.h>

typedef struct CronService CronService;
typedef struct CronJob CronJob;

struct CronJob {
    char* id;
    char* name;
    char* payload_message;
    char* channel;
    char* to;
    bool deliver;
    time_t next_run;
    char* schedule;  // cron expression
};

typedef void (*CronCallback)(CronJob* job);

CronService* cron_service_create(const char* store_path);
void cron_service_destroy(CronService* service);

bool cron_service_start(CronService* service);
void cron_service_stop(CronService* service);

void cron_service_set_callback(CronService* service, CronCallback callback);

// Job management
char* cron_service_add_job(CronService* service, const CronJob* job);
bool cron_service_remove_job(CronService* service, const char* job_id);

// Status
char* cron_service_status(CronService* service);

#endif // CRON_H