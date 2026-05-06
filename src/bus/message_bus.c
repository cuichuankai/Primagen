#include "message_bus.h"
#include "../include/common.h"
#include "../include/message.h"
#include <time.h>
#include <errno.h>

enum {
    MESSAGE_KIND_INBOUND = 1,
    MESSAGE_KIND_OUTBOUND = 2,
    MESSAGE_KIND_INTERNAL = 3
};

static void message_free_by_kind(void* msg, int message_kind) {
    if (!msg) return;
    if (message_kind == MESSAGE_KIND_INBOUND) {
        inbound_message_free((InboundMessage*)msg);
    } else if (message_kind == MESSAGE_KIND_OUTBOUND) {
        outbound_message_free((OutboundMessage*)msg);
    } else if (message_kind == MESSAGE_KIND_INTERNAL) {
        internal_event_free((InternalEvent*)msg);
    } else {
        free(msg);
    }
}

static bool message_queue_init(MessageQueue* q, int message_kind) {
    q->capacity = 16;
    q->items = malloc(q->capacity * sizeof(InboundMessage*));
    if (!q->items) return false;
    q->front = 0;
    q->rear = 0;
    q->message_kind = message_kind;
    q->closed = false;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__) && defined(CLOCK_MONOTONIC)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&q->cond, &attr);
    pthread_condattr_destroy(&attr);
    return true;
}

static void message_queue_free(MessageQueue* q) {
    pthread_mutex_lock(&q->mutex);
    q->closed = true;
    pthread_cond_broadcast(&q->cond);

    struct timespec ts = {0, 10000000};
    for (int i = 0; i < 100; i++) {
        if (q->front == q->rear) break;
        pthread_mutex_unlock(&q->mutex);
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&q->mutex);
    }

    while (q->front != q->rear) {
        void* msg = q->items[q->front];
        q->front = (q->front + 1) % q->capacity;
        message_free_by_kind(msg, q->message_kind);
    }
    free(q->items);
    q->items = NULL;
    q->capacity = 0;
    q->front = 0;
    q->rear = 0;
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void message_queue_close(MessageQueue* q) {
    pthread_mutex_lock(&q->mutex);
    q->closed = true;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void message_queue_send(MessageQueue* q, void* msg) {
    pthread_mutex_lock(&q->mutex);
    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        message_free_by_kind(msg, q->message_kind);
        return;
    }
    size_t next_rear = (q->rear + 1) % q->capacity;
    if (next_rear == q->front) {
        size_t new_cap = q->capacity * 2;
        void** new_items = malloc(new_cap * sizeof(void*));
        if (!new_items) {
            pthread_mutex_unlock(&q->mutex);
            message_free_by_kind(msg, q->message_kind);
            return;
        }

        size_t j = 0;
        for (size_t i = q->front; i != q->rear; i = (i + 1) % q->capacity) {
            new_items[j++] = q->items[i];
        }

        free(q->items);
        q->items = (InboundMessage**)new_items;
        q->front = 0;
        q->rear = j;
        q->capacity = new_cap;
        next_rear = (q->rear + 1) % q->capacity;
    }

    q->items[q->rear] = msg;
    q->rear = next_rear;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void* message_queue_receive(MessageQueue* q) {
    pthread_mutex_lock(&q->mutex);
    while (q->front == q->rear && !q->closed) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    if (q->front == q->rear) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    void* msg = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    pthread_mutex_unlock(&q->mutex);
    return msg;
}

static void* message_queue_receive_timed(MessageQueue* q, int timeout_ms) {
    if (timeout_ms <= 0) return NULL;

    struct timespec ts;
    
    pthread_mutex_lock(&q->mutex);
    while (q->front == q->rear && !q->closed) {
        int rc;
#if defined(__APPLE__)
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        rc = pthread_cond_timedwait_relative_np(&q->cond, &q->mutex, &ts);
#else
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
#endif
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return NULL;
        }
    }
    if (q->front == q->rear) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    void* msg = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    pthread_mutex_unlock(&q->mutex);
    return msg;
}

MessageBus* message_bus_new() {
    MessageBus* bus = malloc(sizeof(MessageBus));
    if (!bus) return NULL;
    if (!message_queue_init(&bus->inbound, MESSAGE_KIND_INBOUND)) {
        free(bus);
        return NULL;
    }
    if (!message_queue_init(&bus->outbound, MESSAGE_KIND_OUTBOUND)) {
        free(bus->inbound.items);
        pthread_mutex_destroy(&bus->inbound.mutex);
        pthread_cond_destroy(&bus->inbound.cond);
        free(bus);
        return NULL;
    }
    if (!message_queue_init(&bus->internal, MESSAGE_KIND_INTERNAL)) {
        free(bus->inbound.items);
        pthread_mutex_destroy(&bus->inbound.mutex);
        pthread_cond_destroy(&bus->inbound.cond);
        free(bus->outbound.items);
        pthread_mutex_destroy(&bus->outbound.mutex);
        pthread_cond_destroy(&bus->outbound.cond);
        free(bus);
        return NULL;
    }
    return bus;
}

void message_bus_close(MessageBus* bus) {
    if (!bus) return;
    message_queue_close(&bus->inbound);
    message_queue_close(&bus->outbound);
    message_queue_close(&bus->internal);
}

void message_bus_free(MessageBus* bus) {
    if (!bus) return;
    message_queue_free(&bus->inbound);
    message_queue_free(&bus->outbound);
    message_queue_free(&bus->internal);
    free(bus);
}

void message_bus_send_inbound(MessageBus* bus, InboundMessage* msg) {
    if (!bus) {
        inbound_message_free(msg);
        return;
    }
    message_queue_send(&bus->inbound, msg);
}

InboundMessage* message_bus_receive_inbound(MessageBus* bus) {
    if (!bus) return NULL;
    return (InboundMessage*)message_queue_receive(&bus->inbound);
}

InboundMessage* message_bus_receive_inbound_timed(MessageBus* bus, int timeout_ms) {
    if (!bus) return NULL;
    return (InboundMessage*)message_queue_receive_timed(&bus->inbound, timeout_ms);
}

void message_bus_send_outbound(MessageBus* bus, OutboundMessage* msg) {
    if (!bus) {
        outbound_message_free(msg);
        return;
    }
    message_queue_send(&bus->outbound, msg);
}

OutboundMessage* message_bus_receive_outbound(MessageBus* bus) {
    if (!bus) return NULL;
    return (OutboundMessage*)message_queue_receive(&bus->outbound);
}

OutboundMessage* message_bus_receive_outbound_timed(MessageBus* bus, int timeout_ms) {
    if (!bus) return NULL;
    return (OutboundMessage*)message_queue_receive_timed(&bus->outbound, timeout_ms);
}

bool message_bus_is_outbound_closed(MessageBus* bus) {
    if (!bus) return true;
    pthread_mutex_lock(&bus->outbound.mutex);
    bool closed = bus->outbound.closed;
    pthread_mutex_unlock(&bus->outbound.mutex);
    return closed;
}

void message_bus_send_internal(MessageBus* bus, InternalEvent* event) {
    if (!bus || !event) return;
    message_queue_send(&bus->internal, event);
}

InternalEvent* message_bus_receive_internal_timed(MessageBus* bus, int timeout_ms) {
    if (!bus) return NULL;
    return (InternalEvent*)message_queue_receive_timed(&bus->internal, timeout_ms);
}
