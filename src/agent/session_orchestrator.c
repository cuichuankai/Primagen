#include "session_orchestrator.h"
#include "agent_loop.h"
#include "../include/logger.h"
#include "../include/utils.h"
#include "../session/session.h"
#include "../tools/tool.h"
#include <string.h>
#include <stdio.h>

SessionOrchestrator* session_orchestrator_new(struct AgentLoop* loop, SessionManager* session_mgr) {
    if (!loop || !session_mgr) return NULL;

    SessionOrchestrator* orchestrator = calloc(1, sizeof(SessionOrchestrator));
    if (!orchestrator) return NULL;

    orchestrator->loop = loop;
    orchestrator->session_mgr = session_mgr;
    orchestrator->active_tasks = NULL;
    orchestrator->current_session_key[0] = '\0';
    pthread_mutex_init(&orchestrator->task_mutex, NULL);

    return orchestrator;
}

void session_orchestrator_free(SessionOrchestrator* orchestrator) {
    if (!orchestrator) return;

    session_orchestrator_cancel_all_tasks(orchestrator);

    struct timespec wait_ts = {0, 10000000};
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&orchestrator->task_mutex);
        bool all_idle = true;
        ActiveTaskNode* check = orchestrator->active_tasks;
        while (check) {
            if (check->task && check->task->ctx.state != SESSION_STATE_IDLE && !check->task->cancelled) {
                all_idle = false;
                break;
            }
            check = check->next;
        }
        pthread_mutex_unlock(&orchestrator->task_mutex);
        if (all_idle) break;
        nanosleep(&wait_ts, NULL);
    }

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        ActiveTaskNode* next = current->next;
        if (current->task) {
            if (current->task->tool_calls) {
                for (size_t i = 0; i < current->task->tool_calls_count; i++) {
                    string_free(&current->task->tool_calls[i]->id);
                    string_free(&current->task->tool_calls[i]->name);
                    string_free(&current->task->tool_calls[i]->arguments);
                    free(current->task->tool_calls[i]);
                }
                free(current->task->tool_calls);
            }
            free(current->task);
        }
        free(current);
        current = next;
    }
    orchestrator->active_tasks = NULL;
    pthread_mutex_unlock(&orchestrator->task_mutex);

    pthread_mutex_destroy(&orchestrator->task_mutex);
    free(orchestrator);
}

Session* session_orchestrator_get_session(SessionOrchestrator* orchestrator, const char* channel, const char* chat_id, const char* session_key) {
    if (!orchestrator || !channel || !chat_id || !session_key) return NULL;

    Session* session = session_manager_get(orchestrator->session_mgr, session_key);
    if (!session) {
        session = session_manager_create(orchestrator->session_mgr, session_key);
        if (session) {
            log_debug("[SessionOrchestrator] Created new session: %s", session_key);
        }
    }

    return session;
}

void session_orchestrator_add_message(SessionOrchestrator* orchestrator, Session* session, Message* message) {
    if (!orchestrator || !session || !message) return;

    session_add_message(session, message);
    log_debug("[SessionOrchestrator] Added message to session: %s", session->key.data);
}

void session_orchestrator_add_task(SessionOrchestrator* orchestrator, ActiveTask* task) {
    if (!orchestrator || !task) return;

    pthread_mutex_lock(&orchestrator->task_mutex);

    ActiveTaskNode* node = calloc(1, sizeof(ActiveTaskNode));
    if (!node) {
        pthread_mutex_unlock(&orchestrator->task_mutex);
        return;
    }
    node->task = task;
    node->next = NULL;

    if (!orchestrator->active_tasks) {
        orchestrator->active_tasks = node;
    } else {
        ActiveTaskNode* current = orchestrator->active_tasks;
        while (current->next) {
            current = current->next;
        }
        current->next = node;
    }

    log_debug("[SessionOrchestrator] Added active task: %s", task->id);
    pthread_mutex_unlock(&orchestrator->task_mutex);
}

void session_orchestrator_remove_task(SessionOrchestrator* orchestrator, const char* task_id) {
    if (!orchestrator || !task_id) return;

    pthread_mutex_lock(&orchestrator->task_mutex);

    ActiveTaskNode* current = orchestrator->active_tasks;
    ActiveTaskNode* previous = NULL;

    while (current) {
        if (current->task && strcmp(current->task->id, task_id) == 0) {
            if (previous) {
                previous->next = current->next;
            } else {
                orchestrator->active_tasks = current->next;
            }

            if (current->task->tool_calls) {
                for (size_t i = 0; i < current->task->tool_calls_count; i++) {
                    string_free(&current->task->tool_calls[i]->id);
                    string_free(&current->task->tool_calls[i]->name);
                    string_free(&current->task->tool_calls[i]->arguments);
                    free(current->task->tool_calls[i]);
                }
                free(current->task->tool_calls);
            }
            free(current->task);
            free(current);

            log_debug("[SessionOrchestrator] Removed active task: %s", task_id);
            break;
        }
        previous = current;
        current = current->next;
    }

    pthread_mutex_unlock(&orchestrator->task_mutex);
}

void session_orchestrator_cancel_all_tasks(SessionOrchestrator* orchestrator) {
    if (!orchestrator) return;

    pthread_mutex_lock(&orchestrator->task_mutex);

    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task) {
            current->task->cancelled = true;
            log_debug("[SessionOrchestrator] Cancelled task: %s", current->task->id);
        }
        current = current->next;
    }

    pthread_mutex_unlock(&orchestrator->task_mutex);
}

int session_orchestrator_cancel_tasks_by_session(SessionOrchestrator* orchestrator, const char* session_key) {
    if (!orchestrator || !session_key) return 0;

    int cancelled = 0;
    pthread_mutex_lock(&orchestrator->task_mutex);

    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0 && !current->task->cancelled) {
            current->task->cancelled = true;
            cancelled++;
            log_debug("[SessionOrchestrator] Cancelled task: %s for session: %s", current->task->id, session_key);
        }
        current = current->next;
    }

    pthread_mutex_unlock(&orchestrator->task_mutex);
    return cancelled;
}

TaskSnapshot session_orchestrator_get_task(SessionOrchestrator* orchestrator, const char* task_id) {
    TaskSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.valid = false;
    if (!orchestrator || !task_id) return snap;

    pthread_mutex_lock(&orchestrator->task_mutex);

    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->id, task_id) == 0) {
            strncpy(snap.task_id, current->task->id, sizeof(snap.task_id) - 1);
            strncpy(snap.channel, current->task->ctx.channel, sizeof(snap.channel) - 1);
            strncpy(snap.chat_id, current->task->ctx.chat_id, sizeof(snap.chat_id) - 1);
            snap.state = current->task->ctx.state;
            snap.turn = current->task->ctx.turn;
            snap.pending_tool_count = current->task->pending_tool_count;
            snap.cancelled = current->task->cancelled;
            snap.valid = true;
            pthread_mutex_unlock(&orchestrator->task_mutex);
            return snap;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&orchestrator->task_mutex);
    return snap;
}

TaskSnapshot session_orchestrator_get_task_by_session(SessionOrchestrator* orchestrator, const char* session_key) {
    return session_orchestrator_snapshot_task(orchestrator, session_key);
}

TaskSnapshot session_orchestrator_snapshot_task(SessionOrchestrator* orchestrator, const char* session_key) {
    TaskSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.valid = false;

    if (!orchestrator || !session_key) return snap;

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0) {
            strncpy(snap.task_id, current->task->id, sizeof(snap.task_id) - 1);
            strncpy(snap.channel, current->task->ctx.channel, sizeof(snap.channel) - 1);
            strncpy(snap.chat_id, current->task->ctx.chat_id, sizeof(snap.chat_id) - 1);
            snap.state = current->task->ctx.state;
            snap.turn = current->task->ctx.turn;
            snap.pending_tool_count = current->task->pending_tool_count;
            snap.valid = true;
            pthread_mutex_unlock(&orchestrator->task_mutex);
            return snap;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&orchestrator->task_mutex);
    return snap;
}

void session_orchestrator_update_task_state(SessionOrchestrator* orchestrator, const char* session_key, SessionState state) {
    if (!orchestrator || !session_key) return;

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0) {
            current->task->ctx.state = state;
            break;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&orchestrator->task_mutex);
}

void session_orchestrator_update_task_pending_tools(SessionOrchestrator* orchestrator, const char* session_key, int count) {
    if (!orchestrator || !session_key) return;

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0) {
            current->task->pending_tool_count = count;
            break;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&orchestrator->task_mutex);
}

int session_orchestrator_decrement_task_pending_tools(SessionOrchestrator* orchestrator, const char* session_key) {
    int remaining = -1;
    if (!orchestrator || !session_key) return remaining;

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0) {
            current->task->pending_tool_count--;
            remaining = current->task->pending_tool_count;
            break;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&orchestrator->task_mutex);
    return remaining;
}

void session_orchestrator_increment_task_turn(SessionOrchestrator* orchestrator, const char* session_key) {
    if (!orchestrator || !session_key) return;

    pthread_mutex_lock(&orchestrator->task_mutex);
    ActiveTaskNode* current = orchestrator->active_tasks;
    while (current) {
        if (current->task && strcmp(current->task->session_key, session_key) == 0) {
            current->task->ctx.turn++;
            break;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&orchestrator->task_mutex);
}

void session_orchestrator_set_current_session_key(SessionOrchestrator* orchestrator, const char* session_key) {
    if (!orchestrator || !session_key) return;

    strncpy(orchestrator->current_session_key, session_key, sizeof(orchestrator->current_session_key) - 1);
    orchestrator->current_session_key[sizeof(orchestrator->current_session_key) - 1] = '\0';
    log_debug("[SessionOrchestrator] Set current session key: %s", session_key);
}

const char* session_orchestrator_get_current_session_key(SessionOrchestrator* orchestrator) {
    if (!orchestrator) return NULL;
    return orchestrator->current_session_key;
}
