#include "message_bus.h"
#include "../include/common.h"
#include "../include/message.h"

static MessageQueue message_queue_new() {
    MessageQueue q;
    q.capacity = 16;
    q.items = malloc(q.capacity * sizeof(InboundMessage*));
    q.front = 0;
    q.rear = 0;
    pthread_mutex_init(&q.mutex, NULL);
    pthread_cond_init(&q.cond, NULL);
    return q;
}

static void message_queue_free(MessageQueue* q) {
    // Assume queue is empty
    free(q->items);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void message_queue_send(MessageQueue* q, void* msg) {
    pthread_mutex_lock(&q->mutex);
    size_t next_rear = (q->rear + 1) % q->capacity;
    while (next_rear == q->front) {
        // Queue full, wait (simplified, assume not full)
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    q->items[q->rear] = msg;
    q->rear = next_rear;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static void* message_queue_receive(MessageQueue* q) {
    pthread_mutex_lock(&q->mutex);
    while (q->front == q->rear) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    void* msg = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return msg;
}

MessageBus* message_bus_new() {
    MessageBus* bus = malloc(sizeof(MessageBus));
    if (!bus) return NULL;
    bus->inbound = message_queue_new();
    bus->outbound = message_queue_new();
    return bus;
}

void message_bus_free(MessageBus* bus) {
    if (!bus) return;
    message_queue_free(&bus->inbound);
    message_queue_free(&bus->outbound);
    free(bus);
}

void message_bus_send_inbound(MessageBus* bus, InboundMessage* msg) {
    message_queue_send(&bus->inbound, msg);
}

InboundMessage* message_bus_receive_inbound(MessageBus* bus) {
    return (InboundMessage*)message_queue_receive(&bus->inbound);
}

void message_bus_send_outbound(MessageBus* bus, OutboundMessage* msg) {
    message_queue_send(&bus->outbound, msg);
}

OutboundMessage* message_bus_receive_outbound(MessageBus* bus) {
    return (OutboundMessage*)message_queue_receive(&bus->outbound);
}