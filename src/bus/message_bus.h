#ifndef MESSAGE_BUS_H
#define MESSAGE_BUS_H

#include "../include/common.h"
#include "../include/message.h"
#include <pthread.h>

typedef struct {
    InboundMessage** items;
    size_t front;
    size_t rear;
    size_t capacity;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MessageQueue;

typedef struct {
    MessageQueue inbound;
    MessageQueue outbound;
} MessageBus;

// Functions
MessageBus* message_bus_new();
void message_bus_free(MessageBus* bus);
void message_bus_send_inbound(MessageBus* bus, InboundMessage* msg);
InboundMessage* message_bus_receive_inbound(MessageBus* bus);
void message_bus_send_outbound(MessageBus* bus, OutboundMessage* msg);
OutboundMessage* message_bus_receive_outbound(MessageBus* bus);

#endif // MESSAGE_BUS_H