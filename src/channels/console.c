#include "../include/channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    MessageBus* bus;
    pthread_t thread_id;
    bool running;
} ConsoleData;

static void* console_input_loop(void* arg) {
    Channel* self = (Channel*)arg;
    ConsoleData* data = (ConsoleData*)self->user_data;
    
    printf("Agent is running. Type your message (or 'exit' to quit):\n> ");
    
    char buffer[1024];
    while (data->running && fgets(buffer, sizeof(buffer), stdin)) {
        buffer[strcspn(buffer, "\n")] = 0;
        
        if (strcmp(buffer, "exit") == 0) {
            // Signal exit? For now just break loop, main will handle cleanup
            // Or send a system message
            // exit(0); // Simplified for CLI - DO NOT CALL EXIT in thread, it kills whole process abruptly
            data->running = false; // Stop loop
            // We should signal main thread to stop, but for now we just break and let main join
            // Actually main joins agent_thread, which runs forever. 
            // We need a way to stop agent loop.
            // Sending a special message?
            InboundMessage* msg = inbound_message_new("system", "local_user", "exit");
            message_bus_send_inbound(data->bus, msg);
            break;
        }
        
        if (strlen(buffer) > 0) {
            InboundMessage* msg = inbound_message_new("console", "local_user", buffer);
            message_bus_send_inbound(data->bus, msg);
        }
        
        // Don't print prompt here, output handler will print it
    }
    return NULL;
}

static bool console_init(Channel* self, Config* cfg, MessageBus* bus) {
    (void)cfg;
    ConsoleData* data = malloc(sizeof(ConsoleData));
    data->bus = bus;
    data->running = false;
    self->user_data = data;
    return true;
}

static void console_start(Channel* self) {
    ConsoleData* data = (ConsoleData*)self->user_data;
    data->running = true;
    pthread_create(&data->thread_id, NULL, console_input_loop, self);
}

static void console_stop(Channel* self) {
    ConsoleData* data = (ConsoleData*)self->user_data;
    data->running = false;
    // pthread_cancel(data->thread_id); // Unsafe, better to let it finish or detach
    // Since fgets is blocking, it's hard to stop cleanly without signals or select
}

static void console_send(Channel* self, OutboundMessage* msg) {
    (void)self;
    // Only handle messages for "console" or "cli"
    if (strcmp(msg->channel.data, "console") == 0 || strcmp(msg->channel.data, "cli") == 0) {
        printf("\nAssistant: %s\n> ", msg->content.data);
        fflush(stdout);
    }
}

static void console_destroy(Channel* self) {
    if (self->user_data) free(self->user_data);
    free(self);
}

Channel* channel_create_console() {
    Channel* ch = malloc(sizeof(Channel));
    ch->name = "console";
    ch->init = console_init;
    ch->start = console_start;
    ch->stop = console_stop;
    ch->send = console_send;
    ch->destroy = console_destroy;
    ch->user_data = NULL;
    return ch;
}
