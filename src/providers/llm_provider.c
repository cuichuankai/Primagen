#include "llm_provider.h"
#include "openai_provider.h"
#include "../include/logger.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static LLMProvider* llm_provider_registry_find_locked(LLMProviderRegistry* registry, const char* name);

// =============================================================================
// Global Registry Singleton
// =============================================================================

static LLMProviderRegistry* g_registry = NULL;
static pthread_once_t g_registry_once = PTHREAD_ONCE_INIT;

static void llm_provider_registry_init_once(void) {
    g_registry = llm_provider_registry_new();
    if (!g_registry) return;

    LLMProvider* openai = llm_provider_new(&OPENAI_PROVIDER_INTERFACE, "openai");
    if (openai) {
        if (openai->iface->init) {
            Error err = openai->iface->init(openai, NULL);
            if (err.code != ERR_NONE) {
                log_error("[LLM Registry] Failed to init OpenAI provider: %s", err.message);
                llm_provider_free(openai);
                return;
            }
        }
        llm_provider_registry_register(g_registry, openai);
        llm_provider_registry_set_active(g_registry, "openai");
    }
}

LLMProviderRegistry* llm_provider_get_registry(void) {
    pthread_once(&g_registry_once, llm_provider_registry_init_once);
    return g_registry;
}

// =============================================================================
// Registry Implementation
// =============================================================================

LLMProviderRegistry* llm_provider_registry_new(void) {
    LLMProviderRegistry* registry = calloc(1, sizeof(LLMProviderRegistry));
    if (!registry) return NULL;

    registry->capacity = 8;
    registry->count = 0;
    registry->providers = calloc(registry->capacity, sizeof(LLMProvider*));
    if (!registry->providers) {
        free(registry);
        return NULL;
    }
    registry->active = NULL;
    pthread_mutex_init(&registry->lock, NULL);

    return registry;
}

void llm_provider_registry_free(LLMProviderRegistry* registry) {
    if (!registry) return;

    pthread_mutex_lock(&registry->lock);
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->providers[i]) {
            if (registry->providers[i]->initialized && registry->providers[i]->iface->shutdown) {
                registry->providers[i]->iface->shutdown(registry->providers[i]);
            }
            llm_provider_free(registry->providers[i]);
        }
    }
    free(registry->providers);
    pthread_mutex_unlock(&registry->lock);
    pthread_mutex_destroy(&registry->lock);
    if (registry == g_registry) {
        g_registry = NULL;
    }
    free(registry);
}

int llm_provider_registry_register(LLMProviderRegistry* registry, LLMProvider* provider) {
    if (!registry || !provider || !provider->iface || !provider->name) return -1;

    pthread_mutex_lock(&registry->lock);

    if (llm_provider_registry_find_locked(registry, provider->name)) {
        pthread_mutex_unlock(&registry->lock);
        if (provider->initialized && provider->iface->shutdown) {
            provider->iface->shutdown(provider);
        }
        llm_provider_free(provider);
        return 0;
    }

    if (registry->count >= registry->capacity) {
        size_t new_cap = registry->capacity * 2;
        LLMProvider** new_arr = realloc(registry->providers, new_cap * sizeof(LLMProvider*));
        if (!new_arr) {
            pthread_mutex_unlock(&registry->lock);
            return -1;
        }
        registry->providers = new_arr;
        registry->capacity = new_cap;
    }

    registry->providers[registry->count++] = provider;

    pthread_mutex_unlock(&registry->lock);
    log_debug("[LLM Registry] Registered provider: %s (name=%s)", provider->iface->name, provider->name);
    return 0;
}

size_t llm_provider_registry_unregister_by_plugin(LLMProviderRegistry* registry, void* plugin_ref) {
    if (!registry || !plugin_ref) return 0;

    LLMProvider** removed = NULL;
    size_t removed_count = 0;
    size_t removed_cap = 0;

    pthread_mutex_lock(&registry->lock);
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < registry->count; read_idx++) {
        LLMProvider* provider = registry->providers[read_idx];
        if (provider && provider->plugin_ref == plugin_ref) {
            if (removed_count >= removed_cap) {
                size_t new_cap = removed_cap ? removed_cap * 2 : 4;
                LLMProvider** new_removed = realloc(removed, new_cap * sizeof(LLMProvider*));
                if (!new_removed) {
                    registry->providers[write_idx++] = provider;
                    continue;
                }
                removed = new_removed;
                removed_cap = new_cap;
            }
            removed[removed_count++] = provider;
            if (registry->active == provider) {
                registry->active = NULL;
            }
        } else {
            registry->providers[write_idx++] = provider;
        }
    }
    registry->count = write_idx;
    pthread_mutex_unlock(&registry->lock);

    for (size_t i = 0; i < removed_count; i++) {
        if (removed[i]->initialized && removed[i]->iface->shutdown) {
            removed[i]->iface->shutdown(removed[i]);
        }
        log_debug("[LLM Registry] Unregistered provider: %s", removed[i]->name ? removed[i]->name : "(unnamed)");
        llm_provider_free(removed[i]);
    }
    free(removed);
    return removed_count;
}

static LLMProvider* llm_provider_registry_find_locked(LLMProviderRegistry* registry, const char* name) {
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->providers[i] && registry->providers[i]->name &&
            strcmp(registry->providers[i]->name, name) == 0) {
            return registry->providers[i];
        }
    }
    return NULL;
}

LLMProvider* llm_provider_registry_find(LLMProviderRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;

    pthread_mutex_lock(&registry->lock);
    LLMProvider* found = llm_provider_registry_find_locked(registry, name);
    pthread_mutex_unlock(&registry->lock);
    return found;
}

int llm_provider_registry_set_active(LLMProviderRegistry* registry, const char* name) {
    if (!registry || !name) return -1;

    pthread_mutex_lock(&registry->lock);
    LLMProvider* provider = llm_provider_registry_find_locked(registry, name);
    if (!provider) {
        pthread_mutex_unlock(&registry->lock);
        return -1;
    }
    registry->active = provider;
    pthread_mutex_unlock(&registry->lock);
    log_debug("[LLM Registry] Active provider: %s (name=%s)", provider->iface->name, provider->name);
    return 0;
}

LLMProvider* llm_provider_registry_get_active(LLMProviderRegistry* registry) {
    if (!registry) return NULL;
    pthread_mutex_lock(&registry->lock);
    LLMProvider* active = registry->active;
    pthread_mutex_unlock(&registry->lock);
    return active;
}

LLMProvider* llm_provider_new(const LLMProviderInterface* iface, const char* name) {
    if (!iface) return NULL;
    LLMProvider* provider = calloc(1, sizeof(LLMProvider));
    if (!provider) return NULL;
    provider->iface = iface;
    provider->name = name ? strdup(name) : strdup("");
    provider->state = NULL;
    provider->plugin_ref = NULL;
    provider->initialized = false;
    return provider;
}

void llm_provider_free(LLMProvider* provider) {
    if (!provider) return;
    free(provider->name);
    free(provider);
}
