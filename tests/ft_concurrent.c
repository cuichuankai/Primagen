#include "test_framework.h"
#include "../src/bus/message_bus.h"
#include "../src/session/session.h"
#include "../src/tools/tool.h"
#include "../src/tools/tool_executor.h"
#include "../src/include/message.h"
#include "../src/include/utils.h"
#include <pthread.h>
#include <stdatomic.h>

#define NUM_THREADS 8
#define NUM_MESSAGES_PER_THREAD 100

static MessageBus* g_bus = NULL;
static SessionManager* g_session_mgr = NULL;
static ToolRegistry* g_tool_reg = NULL;
static atomic_int g_producer_count = 0;
static atomic_int g_consumer_count = 0;
static atomic_int g_session_ref_ok = 0;
static atomic_int g_session_ref_fail = 0;

static void* bus_producer_thread(void* arg) {
    int thread_id = *(int*)arg;
    for (int i = 0; i < NUM_MESSAGES_PER_THREAD; i++) {
        char content[64];
        snprintf(content, sizeof(content), "t%d_msg_%d", thread_id, i);
        InboundMessage* msg = inbound_message_new("ch", "chat", content);
        message_bus_send_inbound(g_bus, msg);
        atomic_fetch_add(&g_producer_count, 1);
    }
    return NULL;
}

static void* bus_consumer_thread(void* arg) {
    (void)arg;
    int received = 0;
    while (received < NUM_MESSAGES_PER_THREAD) {
        InboundMessage* msg = message_bus_receive_inbound_timed(g_bus, 100);
        if (msg) {
            inbound_message_free(msg);
            atomic_fetch_add(&g_consumer_count, 1);
            received++;
        } else {
            break;
        }
    }
    return NULL;
}

TEST(ft_bus_concurrent_producers) {
    g_bus = message_bus_new();
    ASSERT_NOT_NULL(g_bus);
    atomic_store(&g_producer_count, 0);
    
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        int rc = pthread_create(&threads[i], NULL, bus_producer_thread, &thread_ids[i]);
        ASSERT_EQ_INT(0, rc);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    int total_sent = atomic_load(&g_producer_count);
    ASSERT_EQ_INT(NUM_THREADS * NUM_MESSAGES_PER_THREAD, total_sent);
    
    int received = 0;
    while (received < total_sent) {
        InboundMessage* msg = message_bus_receive_inbound_timed(g_bus, 500);
        if (msg) {
            inbound_message_free(msg);
            received++;
        } else {
            break;
        }
    }
    ASSERT_EQ_INT(total_sent, received);
    
    message_bus_free(g_bus);
    g_bus = NULL;
}

TEST(ft_bus_concurrent_producer_consumer) {
    g_bus = message_bus_new();
    ASSERT_NOT_NULL(g_bus);
    atomic_store(&g_producer_count, 0);
    atomic_store(&g_consumer_count, 0);
    
    int total_messages = NUM_THREADS * NUM_MESSAGES_PER_THREAD;
    
    pthread_t producers[NUM_THREADS];
    pthread_t consumers[4];
    int thread_ids[NUM_THREADS];
    
    for (int i = 0; i < 4; i++) {
        pthread_create(&consumers[i], NULL, bus_consumer_thread, NULL);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&producers[i], NULL, bus_producer_thread, &thread_ids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(producers[i], NULL);
    }
    
    usleep(100000);
    message_bus_close(g_bus);
    
    for (int i = 0; i < 4; i++) {
        pthread_join(consumers[i], NULL);
    }
    
    int total_produced = atomic_load(&g_producer_count);
    int total_consumed = atomic_load(&g_consumer_count);
    ASSERT_EQ_INT(total_messages, total_produced);
    ASSERT_TRUE(total_consumed > 0);
    
    message_bus_free(g_bus);
    g_bus = NULL;
}

static void* session_get_unref_thread(void* arg) {
    int thread_id = *(int*)arg;
    for (int i = 0; i < 100; i++) {
        Session* s = session_manager_get(g_session_mgr, "concurrent_key");
        if (s) {
            int rc = s->ref_count;
            if (rc >= 1) {
                atomic_fetch_add(&g_session_ref_ok, 1);
            } else {
                atomic_fetch_add(&g_session_ref_fail, 1);
            }
            session_unref(s);
        }
        char key[64];
        snprintf(key, sizeof(key), "key_%d_%d", thread_id, i % 10);
        Session* s2 = session_manager_get(g_session_mgr, key);
        if (s2) {
            session_unref(s2);
        }
    }
    return NULL;
}

TEST(ft_session_concurrent_access) {
    g_session_mgr = session_manager_new("/tmp/ft_concurrent_session");
    ASSERT_NOT_NULL(g_session_mgr);
    atomic_store(&g_session_ref_ok, 0);
    atomic_store(&g_session_ref_fail, 0);
    
    session_manager_create(g_session_mgr, "concurrent_key");
    for (int i = 0; i < 10; i++) {
        char key[64];
        snprintf(key, sizeof(key), "key_0_%d", i);
        session_manager_create(g_session_mgr, key);
    }
    
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        int rc = pthread_create(&threads[i], NULL, session_get_unref_thread, &thread_ids[i]);
        ASSERT_EQ_INT(0, rc);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    int ok = atomic_load(&g_session_ref_ok);
    int fail = atomic_load(&g_session_ref_fail);
    ASSERT_TRUE(ok > 0);
    ASSERT_EQ_INT(0, fail);
    
    session_manager_free(g_session_mgr);
    g_session_mgr = NULL;
}

static Error concurrent_tool_fn(void* user_data, const char* args_json, String* result) {
    (void)user_data; (void)args_json;
    usleep(1000);
    *result = string_new("concurrent_result");
    return error_new(ERR_NONE, "");
}

static atomic_int g_tool_exec_count = 0;

static void tool_async_callback(Error err, const char* result, void* user_data) {
    (void)user_data;
    if (err.code == ERR_NONE && result) {
        atomic_fetch_add(&g_tool_exec_count, 1);
    }
}

TEST(ft_tool_executor_concurrent) {
    g_tool_reg = tool_registry_new();
    ASSERT_NOT_NULL(g_tool_reg);
    
    tool_registry_register(g_tool_reg, "concurrent_tool", "A concurrent test tool", "{}", concurrent_tool_fn, NULL);
    
    ToolExecutor* executor = tool_executor_new(g_tool_reg, 4);
    ASSERT_NOT_NULL(executor);
    
    atomic_store(&g_tool_exec_count, 0);
    
    int num_tasks = 50;
    for (int i = 0; i < num_tasks; i++) {
        tool_executor_submit_async(executor, "concurrent_tool", "{}", tool_async_callback, NULL);
    }
    
    usleep(500000);
    
    int completed = atomic_load(&g_tool_exec_count);
    ASSERT_TRUE(completed > 0);
    
    tool_executor_destroy(executor);
    tool_registry_free(g_tool_reg);
    g_tool_reg = NULL;
}

TEST(ft_generate_id_thread_safety) {
    char* ids[80];
    int count = 80;
    
    for (int i = 0; i < count; i++) {
        ids[i] = generate_id("ft");
        usleep(100);
    }
    
    bool all_valid = true;
    for (int i = 0; i < count; i++) {
        if (!ids[i] || strncmp(ids[i], "ft_", 3) != 0) {
            all_valid = false;
            break;
        }
    }
    ASSERT_TRUE(all_valid);
    
    int unique_count = 0;
    for (int i = 0; i < count; i++) {
        bool is_unique = true;
        for (int j = 0; j < i; j++) {
            if (strcmp(ids[i], ids[j]) == 0) {
                is_unique = false;
                break;
            }
        }
        if (is_unique) unique_count++;
    }
    ASSERT_TRUE(unique_count > count / 2);
    
    for (int i = 0; i < count; i++) {
        free(ids[i]);
    }
}

static void* bus_close_during_send_thread(void* arg) {
    MessageBus* bus = (MessageBus*)arg;
    for (int i = 0; i < 1000; i++) {
        InboundMessage* msg = inbound_message_new("ch", "id", "test");
        message_bus_send_inbound(bus, msg);
    }
    return NULL;
}

TEST(ft_bus_close_during_send) {
    MessageBus* bus = message_bus_new();
    ASSERT_NOT_NULL(bus);
    
    pthread_t sender;
    pthread_create(&sender, NULL, bus_close_during_send_thread, bus);
    
    usleep(10000);
    message_bus_close(bus);
    
    pthread_join(sender, NULL);
    
    message_bus_free(bus);
}

static void* session_concurrent_create_thread(void* arg) {
    SessionManager* mgr = (SessionManager*)arg;
    for (int i = 0; i < 50; i++) {
        char key[64];
        snprintf(key, sizeof(key), "create_%ld_%d", (long)pthread_self(), i);
        Session* s = session_manager_create(mgr, key);
        if (s) {
            Message* m = message_new(ROLE_USER, "test");
            session_add_message(s, m);
        }
    }
    return NULL;
}

TEST(ft_session_concurrent_create) {
    SessionManager* mgr = session_manager_new("/tmp/ft_session_concurrent_create");
    ASSERT_NOT_NULL(mgr);
    
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, session_concurrent_create_thread, mgr);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    ASSERT_TRUE(mgr->count > 0);
    
    session_manager_free(mgr);
}

TEST(ft_bus_outbound_concurrent) {
    MessageBus* bus = message_bus_new();
    ASSERT_NOT_NULL(bus);
    
    for (int i = 0; i < 200; i++) {
        char content[32];
        snprintf(content, sizeof(content), "out_%d", i);
        OutboundMessage* msg = outbound_message_new("ch", "chat", content);
        message_bus_send_outbound(bus, msg);
    }
    
    int received = 0;
    for (int i = 0; i < 200; i++) {
        OutboundMessage* msg = message_bus_receive_outbound_timed(bus, 100);
        if (msg) {
            outbound_message_free(msg);
            received++;
        }
    }
    ASSERT_EQ_INT(200, received);
    
    message_bus_free(bus);
}

TEST_SUITE(concurrent) {
    BEGIN_SUITE(concurrent);
    RUN_TEST(ft_bus_concurrent_producers);
    RUN_TEST(ft_bus_concurrent_producer_consumer);
    RUN_TEST(ft_session_concurrent_access);
    RUN_TEST(ft_tool_executor_concurrent);
    RUN_TEST(ft_generate_id_thread_safety);
    RUN_TEST(ft_bus_close_during_send);
    RUN_TEST(ft_session_concurrent_create);
    RUN_TEST(ft_bus_outbound_concurrent);
    END_SUITE();
}
