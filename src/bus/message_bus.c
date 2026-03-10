#include "message_bus.h"
#include "../include/common.h"
#include "../include/message.h"

static void message_queue_init(MessageQueue* q) {
    q->capacity = 16;
    q->items = malloc(q->capacity * sizeof(InboundMessage*));
    q->front = 0;
    q->rear = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void message_queue_free(MessageQueue* q) {
    // Assume queue is empty or free items if needed
    free(q->items);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static void message_queue_send(MessageQueue* q, void* msg) {
    pthread_mutex_lock(&q->mutex);
    size_t next_rear = (q->rear + 1) % q->capacity;
    
    // Simple blocking if full, but wait on condition?
    // Current implementation waits if full, but cond is for empty?
    // We need a separate cond for "not full" if we want to block on send.
    // Or just realloc.
    // For now, let's just resize if full to be safe, or wait.
    // The previous code had a wait loop but only one condition variable.
    // Reusing the same cond for "not empty" and "not full" is risky if we don't broadcast.
    // But let's stick to the previous logic but fix the copy issue first.
    // Actually, let's improve it to realloc.
    
    if (next_rear == q->front) {
        // Queue full, double capacity
        size_t new_cap = q->capacity * 2;
        void** new_items = malloc(new_cap * sizeof(void*));
        
        // Copy items preserving order
        size_t j = 0;
        for (size_t i = q->front; i != q->rear; i = (i + 1) % q->capacity) {
            new_items[j++] = q->items[i];
        }
        
        free(q->items);
        q->items = (InboundMessage**)new_items; // Cast for warning
        q->front = 0;
        q->rear = j;
        q->capacity = new_cap;
        next_rear = (q->rear + 1) % q->capacity;
    }
    
    q->items[q->rear] = msg;
    q->rear = next_rear;
    pthread_cond_signal(&q->cond); // Signal potentially waiting receiver
    pthread_mutex_unlock(&q->mutex);
}

static void* message_queue_receive(MessageQueue* q) {
    pthread_mutex_lock(&q->mutex);
    while (q->front == q->rear) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    void* msg = q->items[q->front];
    q->front = (q->front + 1) % q->capacity;
    pthread_mutex_unlock(&q->mutex);
    return msg;
}

MessageBus* message_bus_new() {
    MessageBus* bus = malloc(sizeof(MessageBus));
    if (!bus) return NULL;
    message_queue_init(&bus->inbound);
    message_queue_init(&bus->outbound);
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