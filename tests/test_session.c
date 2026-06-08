#include "test_framework.h"
#include "../src/session/session.h"
#include "../src/include/message.h"
#include <pthread.h>

TEST(session_manager_create_get) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions");
    ASSERT_NOT_NULL(mgr);
    Session* s = session_manager_create(mgr, "test_key_1");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR("test_key_1", s->key.data);
    session_manager_free(mgr);
}

TEST(session_manager_get_nonexistent) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions2");
    Session* s = session_manager_get(mgr, "no_such_key");
    ASSERT_NULL(s);
    session_manager_free(mgr);
}

TEST(session_ref_unref_basic) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions3");
    Session* s = session_manager_create(mgr, "ref_test");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_INT(1, s->ref_count);
    
    Session* ref1 = session_ref(s);
    ASSERT_NOT_NULL(ref1);
    ASSERT_TRUE(ref1 == s);
    ASSERT_EQ_INT(2, s->ref_count);
    
    session_unref(ref1);
    ASSERT_EQ_INT(1, s->ref_count);
    
    session_manager_free(mgr);
}

TEST(session_ref_from_manager) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions4");
    session_manager_create(mgr, "ref_mgr_test");
    
    Session* s1 = session_manager_get(mgr, "ref_mgr_test");
    ASSERT_NOT_NULL(s1);
    ASSERT_EQ_INT(2, s1->ref_count);
    
    Session* s2 = session_manager_get(mgr, "ref_mgr_test");
    ASSERT_NOT_NULL(s2);
    ASSERT_TRUE(s1 == s2);
    ASSERT_EQ_INT(3, s2->ref_count);
    
    session_unref(s1);
    ASSERT_EQ_INT(2, s2->ref_count);
    session_unref(s2);
    ASSERT_EQ_INT(1, s2->ref_count);
    
    session_manager_free(mgr);
}

TEST(session_add_message) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions5");
    Session* s = session_manager_create(mgr, "msg_test");
    ASSERT_NOT_NULL(s);
    
    Message* m1 = message_new(ROLE_USER, "Hello");
    session_add_message(s, m1);
    ASSERT_EQ_SIZE(1, s->messages.count);
    
    Message* m2 = message_new(ROLE_ASSISTANT, "Hi there");
    session_add_message(s, m2);
    ASSERT_EQ_SIZE(2, s->messages.count);
    
    session_manager_free(mgr);
}

TEST(session_double_get_double_unref) {
    SessionManager* mgr = session_manager_new("/tmp/test_sessions6");
    session_manager_create(mgr, "double_ref");
    
    Session* s1 = session_manager_get(mgr, "double_ref");
    Session* s2 = session_manager_get(mgr, "double_ref");
    ASSERT_NOT_NULL(s1);
    ASSERT_NOT_NULL(s2);
    ASSERT_TRUE(s1 == s2);
    ASSERT_EQ_INT(3, s1->ref_count);
    
    session_unref(s1);
    ASSERT_EQ_INT(2, s2->ref_count);
    session_unref(s2);
    ASSERT_EQ_INT(1, s2->ref_count);
    
    session_manager_free(mgr);
}

TEST(session_unref_null) {
    session_unref(NULL);
}

TEST(session_ref_null) {
    Session* result = session_ref(NULL);
    ASSERT_NULL(result);
}

TEST_SUITE(session) {
    BEGIN_SUITE(session);
    RUN_TEST(session_manager_create_get);
    RUN_TEST(session_manager_get_nonexistent);
    RUN_TEST(session_ref_unref_basic);
    RUN_TEST(session_ref_from_manager);
    RUN_TEST(session_add_message);
    RUN_TEST(session_double_get_double_unref);
    RUN_TEST(session_unref_null);
    RUN_TEST(session_ref_null);
    END_SUITE();
}
