/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil_unity.hpp"
#include "../src/runtime/core/recv_internal.hpp"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#endif

namespace
{
void discard_test_socket_parts (const zlink_routing_id_t *,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                void *)
{
    zlink_multipart_close (parts_, part_count_);
}

void discard_test_spot_parts (
  const zlink_routing_id_t *, const char *, size_t, zlink_msg_t *parts_, size_t part_count_, void *)
{
    zlink_multipart_close (parts_, part_count_);
}
}

int test_attach_discard_handler_for_type (void *socket_, int type_)
{
    switch (static_cast<zlink_socket_type_t> (type_)) {
        case ZLINK_SOCKET_PAIR:
        case ZLINK_SOCKET_DEALER:
        case ZLINK_SOCKET_ROUTER:
        case ZLINK_SOCKET_STREAM:
            return zlink_recv_handler (socket_, &discard_test_socket_parts, NULL);
        case ZLINK_SOCKET_SUB:
        case ZLINK_SOCKET_XSUB:
            return 0;
        case ZLINK_SOCKET_XPUB:
        case ZLINK_SOCKET_PUB:
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

int test_assert_success_message_errno_helper (int rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line_)
{
    if (rc_ == -1) {
        char buffer[512];
        buffer[sizeof (buffer) - 1] = 0; // to ensure defined behavior with VC++ <= 2013
        snprintf (buffer, sizeof (buffer) - 1, "%s failed%s%s%s, errno = %i (%s)", expr_,
                  msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                  zlink_errno (), zlink_strerror (zlink_errno ()));
        UNITY_TEST_FAIL (line_, buffer);
    }
    return rc_;
}

int test_assert_success_message_errno_helper (zlink_submit_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line_)
{
    if (rc_ != ZLINK_SUBMIT_OK) {
        char buffer[512];
        buffer[sizeof (buffer) - 1] = 0;
        snprintf (buffer, sizeof (buffer) - 1, "%s failed%s%s%s, submit = %i, errno = %i (%s)",
                  expr_, msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                  static_cast<int> (rc_), zlink_errno (), zlink_strerror (zlink_errno ()));
        UNITY_TEST_FAIL (line_, buffer);
    }
    return static_cast<int> (rc_);
}

#define DEFINE_RESULT_ASSERT_HELPER(TYPE, OK_VAL, LABEL)                                           \
    int test_assert_success_message_errno_helper (TYPE rc_, const char *msg_, const char *expr_,   \
                                                  int line_)                                       \
    {                                                                                              \
        if (rc_ != OK_VAL) {                                                                       \
            char buffer[512];                                                                      \
            buffer[sizeof (buffer) - 1] = 0;                                                       \
            snprintf (buffer, sizeof (buffer) - 1,                                                 \
                      "%s failed%s%s%s, " LABEL " = %i, errno = %i (%s)", expr_,                   \
                      msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",        \
                      static_cast<int> (rc_), zlink_errno (), zlink_strerror (zlink_errno ()));    \
            UNITY_TEST_FAIL (line_, buffer);                                                       \
        }                                                                                          \
        return static_cast<int> (rc_);                                                             \
    }

DEFINE_RESULT_ASSERT_HELPER (zlink_connect_result_t, ZLINK_CONNECT_OK, "connect")
DEFINE_RESULT_ASSERT_HELPER (zlink_bind_result_t, ZLINK_BIND_OK, "bind")
DEFINE_RESULT_ASSERT_HELPER (zlink_config_result_t, ZLINK_CONFIG_OK, "config")
DEFINE_RESULT_ASSERT_HELPER (zlink_close_result_t, ZLINK_CLOSE_OK, "close")
DEFINE_RESULT_ASSERT_HELPER (zlink_recv_result_t, ZLINK_RECV_OK, "recv")
DEFINE_RESULT_ASSERT_HELPER (zlink_handler_result_t, ZLINK_HANDLER_OK, "handler")

#undef DEFINE_RESULT_ASSERT_HELPER

int test_assert_success_message_raw_errno_helper (
  int rc_, const char *msg_, const char *expr_, int line_, bool zero)
{
    if (rc_ == -1 || (zero && rc_ != 0)) {
#if defined ZLINK_HAVE_WINDOWS
        int current_errno = WSAGetLastError ();
#else
        int current_errno = errno;
#endif

        char buffer[512];
        buffer[sizeof (buffer) - 1] = 0; // to ensure defined behavior with VC++ <= 2013
        snprintf (buffer, sizeof (buffer) - 1, "%s failed%s%s%s with %d, errno = %i/%s", expr_,
                  msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "", rc_,
                  current_errno, strerror (current_errno));
        UNITY_TEST_FAIL (line_, buffer);
    }
    return rc_;
}

int test_assert_success_message_raw_zero_errno_helper (int rc_,
                                                       const char *msg_,
                                                       const char *expr_,
                                                       int line_)
{
    return test_assert_success_message_raw_errno_helper (rc_, msg_, expr_, line_, true);
}

int test_assert_failure_message_raw_errno_helper (
  int rc_, int expected_errno_, const char *msg_, const char *expr_, int line_)
{
    char buffer[512];
    buffer[sizeof (buffer) - 1] = 0; // to ensure defined behavior with VC++ <= 2013
    if (rc_ != -1) {
        snprintf (buffer, sizeof (buffer) - 1,
                  "%s was unexpectedly successful%s%s%s, expected "
                  "errno = %i, actual return value = %i",
                  expr_, msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                  expected_errno_, rc_);
        UNITY_TEST_FAIL (line_, buffer);
    } else {
#if defined ZLINK_HAVE_WINDOWS
        int current_errno = WSAGetLastError ();
#else
        int current_errno = errno;
#endif
        if (current_errno != expected_errno_) {
            snprintf (buffer, sizeof (buffer) - 1,
                      "%s failed with an unexpected error%s%s%s, expected "
                      "errno = %i, actual errno = %i",
                      expr_, msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                      expected_errno_, current_errno);
            UNITY_TEST_FAIL (line_, buffer);
        }
    }
    return rc_;
}

int test_assert_failure_message_raw_errno_helper (
  zlink_submit_result_t rc_, int expected_errno_, const char *msg_, const char *expr_, int line_)
{
    char buffer[512];
    buffer[sizeof (buffer) - 1] = 0;
    if (rc_ == ZLINK_SUBMIT_OK) {
        snprintf (buffer, sizeof (buffer) - 1,
                  "%s was unexpectedly successful%s%s%s, expected errno = %i, "
                  "actual submit result = %i",
                  expr_, msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                  expected_errno_, static_cast<int> (rc_));
        UNITY_TEST_FAIL (line_, buffer);
    } else {
#if defined ZLINK_HAVE_WINDOWS
        int current_errno = WSAGetLastError ();
#else
        int current_errno = errno;
#endif
        if (current_errno != expected_errno_) {
            snprintf (buffer, sizeof (buffer) - 1,
                      "%s failed with an unexpected error%s%s%s, expected "
                      "errno = %i, actual errno = %i",
                      expr_, msg_ ? " (additional info: " : "", msg_ ? msg_ : "", msg_ ? ")" : "",
                      expected_errno_, current_errno);
            UNITY_TEST_FAIL (line_, buffer);
        }
    }
    return static_cast<int> (rc_);
}

void send_string_expect_success (void *socket_, const char *str_, int flags_)
{
    const size_t len = str_ ? strlen (str_) : 0;
    const int rc = zlink_send (socket_, str_, len, flags_);
    TEST_ASSERT_EQUAL_INT ((int) len, rc);
}

void recv_string_expect_success (void *socket_, const char *str_, int flags_)
{
    const size_t len = str_ ? strlen (str_) : 0;
    char buffer[255];
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (sizeof (buffer), len,
                                       "recv_string_expect_success cannot be "
                                       "used for strings longer than 255 "
                                       "characters");

    const int rc = TEST_ASSERT_SUCCESS_ERRNO (
      zlink::recv_buffer_internal (socket_, buffer, sizeof (buffer), flags_));
    TEST_ASSERT_EQUAL_INT ((int) len, rc);
    if (str_)
        TEST_ASSERT_EQUAL_STRING_LEN (str_, buffer, len);
}

static void *internal_manage_test_context (bool init_, bool clear_)
{
    static void *test_context = NULL;
    if (clear_) {
        TEST_ASSERT_NOT_NULL (test_context);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (test_context));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (test_context));
        test_context = NULL;
    } else {
        if (init_) {
            TEST_ASSERT_NULL (test_context);
            test_context = zlink_ctx_new ();
            TEST_ASSERT_NOT_NULL (test_context);
        }
    }
    return test_context;
}

static void internal_manage_test_sockets (void *socket_, bool add_)
{
    static void *test_sockets[MAX_TEST_SOCKETS];
    static size_t test_socket_count = 0;
    if (!socket_) {
        TEST_ASSERT_FALSE (add_);

        // force-close all sockets
        if (test_socket_count) {
            for (size_t i = 0; i < test_socket_count; ++i) {
                close_zero_linger (test_sockets[i]);
            }
            fprintf (stderr,
                     "WARNING: Forced closure of %i sockets, this is an "
                     "implementation error unless the test case failed\n",
                     static_cast<int> (test_socket_count));
            test_socket_count = 0;
        }
    } else {
        if (add_) {
            ++test_socket_count;
            TEST_ASSERT_LESS_THAN_MESSAGE (MAX_TEST_SOCKETS, test_socket_count,
                                           "MAX_TEST_SOCKETS must be "
                                           "increased, or you cannot use the "
                                           "test context");
            test_sockets[test_socket_count - 1] = socket_;
        } else {
            bool found = false;
            for (size_t i = 0; i < test_socket_count; ++i) {
                if (test_sockets[i] == socket_) {
                    found = true;
                }
                if (found && i + 1 < test_socket_count)
                    test_sockets[i] = test_sockets[i + 1];
            }
            TEST_ASSERT_TRUE_MESSAGE (found, "Attempted to close a socket that was "
                                             "not created by test_context_socket");
            --test_socket_count;
            test_sockets[test_socket_count] = NULL;
        }
    }
}

void setup_test_context ()
{
    internal_manage_test_context (true, false);
}

void *get_test_context ()
{
    return internal_manage_test_context (false, false);
}

void teardown_test_context ()
{
    if (get_test_context ()) {
        internal_manage_test_sockets (NULL, false);
        internal_manage_test_context (false, true);
    }
}

void *test_context_socket (int type_)
{
    void *const socket =
      zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    internal_manage_test_sockets (socket, true);
    return socket;
}

void *test_context_socket_close (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
    internal_manage_test_sockets (socket_, false);
    return socket_;
}

void *test_context_socket_close_zero_linger (void *socket_)
{
    const int linger = 0;
    int rc = zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    TEST_ASSERT_TRUE (rc == 0 || zlink_errno () == ETERM);
    return test_context_socket_close (socket_);
}

void test_bind (void *socket_, const char *bind_address_, char *my_endpoint_, size_t len_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (socket_, bind_address_));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket_, ZLINK_OPT_LAST_ENDPOINT, my_endpoint_, &len_));
}

void bind_loopback (void *socket_, int ipv6_, char *my_endpoint_, size_t len_)
{
    if (ipv6_ && !is_ipv6_available ()) {
        TEST_IGNORE_MESSAGE ("ipv6 is not available");
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_IPV6, &ipv6_, sizeof (int)));

    test_bind (socket_, ipv6_ ? "tcp://[::1]:*" : "tcp://127.0.0.1:*", my_endpoint_, len_);
}

void bind_loopback_ipv4 (void *socket_, char *my_endpoint_, size_t len_)
{
    bind_loopback (socket_, false, my_endpoint_, len_);
}

void bind_loopback_ipv6 (void *socket_, char *my_endpoint_, size_t len_)
{
    bind_loopback (socket_, true, my_endpoint_, len_);
}

void bind_loopback_ipc (void *socket_, char *my_endpoint_, size_t len_)
{
    if (!zlink_has ("ipc")) {
        TEST_IGNORE_MESSAGE ("ipc is not available");
    }

    test_bind (socket_, "ipc://*", my_endpoint_, len_);
}

#if defined(ZLINK_HAVE_IPC)
void make_random_ipc_endpoint (char *out_endpoint_, size_t len_)
{
    const std::string ipc_path = make_random_ipc_path ();
    const int rc = snprintf (out_endpoint_, len_, "ipc://%s", ipc_path.c_str ());
    TEST_ASSERT_TRUE (rc > 0);
    TEST_ASSERT_LESS_THAN (len_, static_cast<size_t> (rc));
}

void make_random_ipc_endpoint (char *out_endpoint_)
{
    make_random_ipc_endpoint (out_endpoint_, MAX_SOCKET_STRING);
}
#endif
