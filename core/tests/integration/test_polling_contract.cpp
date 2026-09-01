/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#if !defined(ZLINK_HAVE_WINDOWS)
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const short undefined_event_bit = 64;

void test_poll_timeout_and_socket_mask_validation ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (pair);

    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    zlink_pollitem_t item = {pair, 0, ZLINK_POLLIN, 123};
    errno = 0;
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 5, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (0, item.revents);

    item.events = ZLINK_POLLCOMPLETION;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = undefined_event_bit;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = ZLINK_POLLITEMS_DFLT;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    item.events = ZLINK_POLLPRI;
    TEST_ASSERT_EQUAL_INT (-1, zlink_poll (&item, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_NOT_SUPPORTED, error);
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);

    TEST_ASSERT_EQUAL_INT (0, zlink_poll (NULL, 0, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    test_context_socket_close_zero_linger (pair);
}

void test_poller_socket_registration_error_contracts ()
{
    void *poller = zlink_poller_new ();
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *missing = test_context_socket (ZLINK_SOCKET_PAIR);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (pair);
    TEST_ASSERT_NOT_NULL (missing);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (stream);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, pair, pair, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add (poller, pair, pair, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_modify (poller, missing, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND, zlink_poller_remove (poller, missing));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    // Completion ownership can be added by modify, including as the only bit.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_modify (poller, pair, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_poller_modify (poller, pair, ZLINK_POLLPRI));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, pair));

    // Every completion-bearing socket accepts the bit through both add and
    // modify. Removing and restoring only the completion bit must not require
    // a connected endpoint or an already queued record.
    void *const completion_sockets[] = {pair, dealer, router, stream};
    for (size_t i = 0;
         i != sizeof (completion_sockets) / sizeof (completion_sockets[0]);
         ++i) {
        void *const socket = completion_sockets[i];
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, socket, socket, ZLINK_POLLCOMPLETION));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (
            poller, socket,
            static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (poller, socket, ZLINK_POLLIN));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_modify (poller, socket, ZLINK_POLLCOMPLETION));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller, socket));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (stream);
    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (missing);
    test_context_socket_close_zero_linger (pair);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_poller_fd_mask_and_registration_error_contracts ()
{
    TEST_IGNORE_MESSAGE ("POSIX pipe helper unavailable on Windows");
}
#else
void test_poller_fd_mask_and_registration_error_contracts ()
{
    int fds[2];
    TEST_ASSERT_SUCCESS_ERRNO (pipe (fds));
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_ARGUMENT,
      zlink_poller_add_fd (poller, fds[0], NULL, undefined_event_bit));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add_fd (poller, fds[0], NULL,
                           ZLINK_POLLIN | ZLINK_POLLPRI));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add_fd (poller, fds[0], NULL, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_modify_fd (poller, fds[1], ZLINK_POLLOUT));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND, zlink_poller_remove_fd (poller, fds[1]));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_fd (poller, fds[0]));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[0]));
    TEST_ASSERT_SUCCESS_ERRNO (close (fds[1]));
}
#endif

void test_timer_event_hides_internal_fd_and_reports_registration_errors ()
{
    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);

    int user_tag = 9;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add_timer (poller, timer, &user_tag));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_CONFLICT,
      zlink_poller_add_timer (poller, timer, &user_tag));
    TEST_ASSERT_EQUAL_INT (EEXIST, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_timer_start (timer, 1000000ULL, 1));
    zlink_poller_event_t event;
    memset (&event, 0xff, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, 1000, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_TIMER, event.source_kind);
    TEST_ASSERT_NULL (event.socket);
    TEST_ASSERT_EQUAL_INT (0, event.fd);
    TEST_ASSERT_EQUAL_PTR (timer, event.timer);
    TEST_ASSERT_EQUAL_PTR (&user_tag, event.user_data);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, event.events);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_FOUND,
      zlink_poller_remove_timer (poller, timer));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_destroy (&timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_poll_timeout_and_socket_mask_validation);
    RUN_TEST (test_poller_socket_registration_error_contracts);
    RUN_TEST (test_poller_fd_mask_and_registration_error_contracts);
    RUN_TEST (test_timer_event_hides_internal_fd_and_reports_registration_errors);
    return UNITY_END ();
}
