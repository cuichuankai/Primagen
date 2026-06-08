#ifndef SESSION_ORCHESTRATOR_H
#define SESSION_ORCHESTRATOR_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "../include/message.h"
#include "../session/session.h"
#include "../tools/tool.h"

struct AgentLoop;

typedef enum {
    SESSION_STATE_IDLE,
    SESSION_STATE_WAITING_LLM,
    SESSION_STATE_WAITING_TOOL
} SessionState;

typedef struct {
    SessionState state;
    int turn;
    char channel[64];
    char chat_id[512];
    char latest_user_content[1024];
    bool no_session_record;
    size_t session_msg_count_before;
} SessionContext;

typedef struct ActiveTask {
    char id[32];
    char session_key[SESSION_KEY_MAX];
    SessionContext ctx;
    pthread_t thread;
    bool cancelled;
    int pending_tool_count;
    ToolCall** tool_calls;
    size_t tool_calls_count;
} ActiveTask;

typedef struct ActiveTaskNode {
    ActiveTask* task;
    struct ActiveTaskNode* next;
} ActiveTaskNode;

typedef struct {
    struct AgentLoop* loop;
    SessionManager* session_mgr;

    pthread_mutex_t task_mutex;
    ActiveTaskNode* active_tasks;
    char current_session_key[SESSION_KEY_MAX];
} SessionOrchestrator;

typedef struct {
    char task_id[32];
    char channel[64];
    char chat_id[512];
    SessionState state;
    int turn;
    int pending_tool_count;
    bool valid;
    bool cancelled;
    bool no_session_record;
    size_t session_msg_count_before;
} TaskSnapshot;

SessionOrchestrator* session_orchestrator_new(struct AgentLoop* loop, SessionManager* session_mgr);
void session_orchestrator_free(SessionOrchestrator* orchestrator);

Session* session_orchestrator_get_session(SessionOrchestrator* orchestrator, const char* channel, const char* chat_id, const char* session_key);
void session_orchestrator_add_message(SessionOrchestrator* orchestrator, Session* session, Message* message);

void session_orchestrator_add_task(SessionOrchestrator* orchestrator, ActiveTask* task);
void session_orchestrator_remove_task(SessionOrchestrator* orchestrator, const char* task_id);
void session_orchestrator_cancel_all_tasks(SessionOrchestrator* orchestrator);
int session_orchestrator_cancel_tasks_by_session(SessionOrchestrator* orchestrator, const char* session_key);
TaskSnapshot session_orchestrator_get_task(SessionOrchestrator* orchestrator, const char* task_id);
TaskSnapshot session_orchestrator_get_task_by_session(SessionOrchestrator* orchestrator, const char* session_key);

TaskSnapshot session_orchestrator_snapshot_task(SessionOrchestrator* orchestrator, const char* session_key);
void session_orchestrator_update_task_state(SessionOrchestrator* orchestrator, const char* session_key, SessionState state);
void session_orchestrator_update_task_pending_tools(SessionOrchestrator* orchestrator, const char* session_key, int count);
int session_orchestrator_decrement_task_pending_tools(SessionOrchestrator* orchestrator, const char* session_key);
void session_orchestrator_increment_task_turn(SessionOrchestrator* orchestrator, const char* session_key);

void session_orchestrator_set_current_session_key(SessionOrchestrator* orchestrator, const char* session_key);
const char* session_orchestrator_get_current_session_key(SessionOrchestrator* orchestrator);

#endif // SESSION_ORCHESTRATOR_H
