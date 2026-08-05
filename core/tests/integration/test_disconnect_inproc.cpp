/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

/// Initialize a zlink message with a given null-terminated string
#define ZLINK_PREPARE_STRING(msg, data, size)                                                      \
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));                                             \
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, size + 1));                              \
    memcpy (zlink_msg_data (&msg), data, size + 1);

static int publicationsReceived = 0;
static bool isSubscribed = false;

void test_disconnect_inproc ()
{
    void *pub_socket = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub_socket = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub_socket, "foo"));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub_socket, "inproc://someInProcDescriptor"));

    for (int iteration = 0;; ++iteration) {
        zlink_pollitem_t items[] = {
          {sub_socket, 0, ZLINK_POLLIN, 0}, // read publications
          {pub_socket, 0, ZLINK_POLLIN, 0}, // read subscriptions
        };
        int rc = zlink_poll (items, 2, 100, NULL);

        if (items[1].revents & ZLINK_POLLIN) {
            int subscribed = 0;
            char topic[16];
            size_t topic_len = sizeof (topic);
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_subscription_event (pub_socket, NULL, &subscribed, topic, &topic_len, 0));
            TEST_ASSERT_EQUAL_UINT (3, topic_len);
            TEST_ASSERT_EQUAL_MEMORY ("foo", topic, 3);
            if (!subscribed) {
                TEST_ASSERT_TRUE (isSubscribed);
                isSubscribed = false;
            } else {
                TEST_ASSERT_FALSE (isSubscribed);
                isSubscribed = true;
            }
        }

        if (items[0].revents & ZLINK_POLLIN) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            char topic[16];
            size_t topic_len = sizeof (topic);
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_subscribe (sub_socket, &parts, &part_count, 0, topic, &topic_len));
            TEST_ASSERT_EQUAL_UINT (3, topic_len);
            TEST_ASSERT_EQUAL_MEMORY ("foo", topic, 3);
            TEST_ASSERT_EQUAL_UINT (1, part_count);
            TEST_ASSERT_EQUAL_STRING ("this is foo!",
                                      static_cast<const char *> (zlink_msg_data (&parts[0])));
            zlink_multipart_close (parts, part_count);
            publicationsReceived++;
        }
        if (iteration == 1) {
            TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub_socket, "inproc://someInProcDescriptor"));
            msleep (SETTLE_TIME);
        }
        if (iteration == 4) {
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_disconnect (sub_socket, "inproc://someInProcDescriptor"));
        }
        if (iteration > 4 && rc == 0)
            break;

        zlink_msg_t channel_envlp;
        ZLINK_PREPARE_STRING (channel_envlp, "foo", 3);
        TEST_ASSERT_SUCCESS_ERRNO (
          test_send_single_msg (&channel_envlp, pub_socket, ZLINK_SNDMORE));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&channel_envlp));

        zlink_msg_t message;
        ZLINK_PREPARE_STRING (message, "this is foo!", 12);
        TEST_ASSERT_SUCCESS_ERRNO (test_send_single_msg (&message, pub_socket, 0));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&message));
    }
    TEST_ASSERT_EQUAL_INT (3, publicationsReceived);
    TEST_ASSERT_FALSE (isSubscribed);

    test_context_socket_close (pub_socket);
    test_context_socket_close (sub_socket);
}

int main (int, char **)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_disconnect_inproc);
    return UNITY_END ();
}
