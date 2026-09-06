#pragma once

/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.h>
#include "testutil.hpp"

#include <algorithm>
#include <climits>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <unity.h>

// Test shorthand for a public MORE part, never passed as a public send flag.
#define ZLINK_SNDMORE ((zlink_send_flags_t) 0x0002u)

inline int test_send_single_msg (zlink_msg_t *msg_, void *s_, int flags_)
{
    const size_t size = msg_ ? zlink_msg_size (msg_) : 0;
    const zlink_submit_result_t rc = zlink_send_part (
      s_, msg_, static_cast<zlink_send_flags_t> (flags_ & ~ZLINK_SNDMORE),
      (flags_ & ZLINK_SNDMORE) ? ZLINK_PART_MORE : ZLINK_PART_FINAL, NULL, NULL);
    return rc == ZLINK_SUBMIT_OK
             ? static_cast<int> (std::min (size, static_cast<size_t> (INT_MAX)))
             : -1;
}

inline int test_recv_single_msg (
  zlink_msg_t *msg_, void *s_, int flags_,
  zlink_part_flag_t *more_out_ = NULL,
  const zlink_routing_id_t **source_rid_out_ = NULL)
{
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    const zlink_routing_id_t *source_rid = NULL;
    const zlink_recv_result_t rc = zlink_recv_part (
      s_, source_rid_out_ ? source_rid_out_ : &source_rid, msg_,
      more_out_ ? more_out_ : &more, static_cast<zlink_recv_flags_t> (flags_));
    return rc == ZLINK_RECV_OK
             ? static_cast<int> (std::min (zlink_msg_size (msg_),
                                          static_cast<size_t> (INT_MAX)))
             : -1;
}

inline int test_stream_send_single_msg (void *s_,
                                        const zlink_routing_id_t *rid_,
                                        zlink_msg_t *msg_, int flags_)
{
    const size_t size = msg_ ? zlink_msg_size (msg_) : 0;
    const zlink_submit_result_t rc = zlink_send_part_rid (
      s_, rid_, msg_, static_cast<zlink_send_flags_t> (flags_),
      ZLINK_PART_FINAL, NULL, NULL);
    return rc == ZLINK_SUBMIT_OK
             ? static_cast<int> (std::min (size, static_cast<size_t> (INT_MAX)))
             : -1;
}

inline int test_stream_send_bytes (
  void *s_, const zlink_routing_id_t *rid_, const void *data_, size_t size_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, size_) != ZLINK_CONFIG_OK)
        return -1;
    if (size_ > 0 && data_)
        memcpy (zlink_msg_data (&msg), data_, size_);
    return test_stream_send_single_msg (s_, rid_, &msg, flags_);
}

inline int zlink_send (void *s_, const void *buf_, size_t len_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, len_) != ZLINK_CONFIG_OK)
        return -1;
    if (len_ > 0 && buf_)
        memcpy (zlink_msg_data (&msg), buf_, len_);
    return test_send_single_msg (&msg, s_, flags_);
}

namespace testutil_agg
{
// Public part calls own admission and consume submitted parts on every result.
// The helper only walks the caller's array and closes unsubmitted parts.
template <typename SendPart>
inline zlink_submit_result_t send_parts (
  zlink_msg_t *parts_, size_t count_, SendPart send_part_)
{
    if (!parts_ || count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    for (size_t i = 0; i < count_; ++i) {
        const zlink_submit_result_t rc = send_part_ (
          &parts_[i], i + 1 < count_ ? ZLINK_PART_MORE : ZLINK_PART_FINAL);
        if (rc != ZLINK_SUBMIT_OK) {
            const int err = errno;
            zlink_multipart_close (parts_ + i + 1, count_ - i - 1);
            errno = err;
            return rc;
        }
    }
    return ZLINK_SUBMIT_OK;
}

// Callers close returned messages before the next aggregate receive. Storage
// belongs to the test helper; only public message ownership operations are used.
static thread_local std::vector<zlink_msg_t> tl_recv_buf;
static thread_local zlink_routing_id_t tl_source_node_rid;

template <typename RecvPart>
inline zlink_recv_result_t recv_parts (
  zlink_routing_id_t *source_rid_out_, zlink_msg_t **parts_out_,
  size_t *count_out_, RecvPart recv_part_)
{
    if (!parts_out_ || !count_out_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }
    *parts_out_ = NULL;
    *count_out_ = 0;
    if (source_rid_out_)
        memset (source_rid_out_, 0, sizeof (*source_rid_out_));
    tl_recv_buf.clear ();
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    do {
        zlink_msg_t part;
        zlink_msg_init (&part);
        const zlink_routing_id_t *source = NULL;
        const zlink_recv_result_t rc = recv_part_ (&source, &part, &more);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            zlink_multipart_close (tl_recv_buf.data (), tl_recv_buf.size ());
            tl_recv_buf.clear ();
            return rc;
        }
        if (tl_recv_buf.empty () && source_rid_out_ && source)
            *source_rid_out_ = *source;
        tl_recv_buf.push_back (part);
    } while (more == ZLINK_PART_MORE);
    *parts_out_ = tl_recv_buf.data ();
    *count_out_ = tl_recv_buf.size ();
    return ZLINK_RECV_OK;
}
} // namespace testutil_agg

inline zlink_submit_result_t zlink_send (
  void *s_, zlink_msg_t *parts_, size_t part_count_, int flags_)
{
    return testutil_agg::send_parts (
      parts_, part_count_, [=] (zlink_msg_t *part_, zlink_part_flag_t more_) {
          return zlink_send_part (s_, part_, static_cast<zlink_send_flags_t> (flags_),
                                  more_, NULL, NULL);
      });
}

inline zlink_submit_result_t zlink_send_rid (
  void *s_, const zlink_routing_id_t *target_rid_,
  zlink_msg_t *parts_, size_t part_count_, int flags_)
{
    return testutil_agg::send_parts (
      parts_, part_count_, [=] (zlink_msg_t *part_, zlink_part_flag_t more_) {
          return zlink_send_part_rid (
            s_, target_rid_, part_, static_cast<zlink_send_flags_t> (flags_),
            more_, NULL, NULL);
      });
}

inline zlink_submit_result_t zlink_publish (
  void *subject_, const char *topic_id_, zlink_msg_t *parts_,
  size_t part_count_, int flags_)
{
    return testutil_agg::send_parts (
      parts_, part_count_, [=] (zlink_msg_t *part_, zlink_part_flag_t more_) {
          return zlink_publish_part (subject_, topic_id_, part_,
                                     static_cast<zlink_send_flags_t> (flags_), more_);
      });
}

inline zlink_recv_result_t zlink_router_recv (
  void *router_, const zlink_routing_id_t **source_rid_out_,
  uint64_t *reply_token_out_, zlink_msg_t **parts_out_,
  size_t *part_count_out_, int flags_)
{
    const zlink_recv_result_t rc = testutil_agg::recv_parts (
      &testutil_agg::tl_source_node_rid, parts_out_, part_count_out_,
      [=] (const zlink_routing_id_t **source_, zlink_msg_t *part_,
           zlink_part_flag_t *more_) {
          return zlink_router_recv_part (
            router_, source_, reply_token_out_, part_, more_,
            static_cast<zlink_recv_flags_t> (flags_));
      });
    if (source_rid_out_)
        *source_rid_out_ = rc == ZLINK_RECV_OK
                            ? &testutil_agg::tl_source_node_rid : NULL;
    return rc;
}

inline zlink_recv_result_t zlink_recv (
  void *s_, zlink_routing_id_t *source_rid_out_, zlink_msg_t **parts_out_,
  size_t *part_count_out_, int flags_)
{
    return testutil_agg::recv_parts (
      source_rid_out_, parts_out_, part_count_out_,
      [=] (const zlink_routing_id_t **source_, zlink_msg_t *part_,
           zlink_part_flag_t *more_) {
          return zlink_recv_part (s_, source_, part_, more_,
                                  static_cast<zlink_recv_flags_t> (flags_));
      });
}

inline zlink_recv_result_t test_recv_router (
  void *s_, zlink_routing_id_t *source_rid_out_, zlink_msg_t **parts_out_,
  size_t *part_count_out_, int flags_)
{
    return testutil_agg::recv_parts (
      source_rid_out_, parts_out_, part_count_out_,
      [=] (const zlink_routing_id_t **source_, zlink_msg_t *part_,
           zlink_part_flag_t *more_) {
          zlink_reply_token_t token = 0;
          return zlink_router_recv_part (s_, source_, &token, part_, more_,
                                         static_cast<zlink_recv_flags_t> (flags_));
      });
}

inline int zlink_recv (void *s_, void *buf_, size_t len_, int flags_)
{
    zlink_msg_t part;
    zlink_msg_init (&part);
    const int rc = test_recv_single_msg (&part, s_, flags_);
    if (rc >= 0 && buf_)
        memcpy (buf_, zlink_msg_data (&part), std::min (len_, zlink_msg_size (&part)));
    const int err = errno;
    zlink_msg_close (&part);
    errno = err;
    return rc;
}

inline int test_recv_router (
  void *socket_, void *buffer_, size_t size_, int flags_,
  zlink_routing_id_t *source_out_ = NULL)
{
    zlink_msg_t part;
    zlink_msg_init (&part);
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more;
    const zlink_recv_result_t rc = zlink_router_recv_part (
      socket_, &source, &token, &part, &more,
      static_cast<zlink_recv_flags_t> (flags_));
    int result = -1;
    if (rc == ZLINK_RECV_OK) {
        const size_t length = zlink_msg_size (&part);
        if (buffer_)
            memcpy (buffer_, zlink_msg_data (&part), std::min (size_, length));
        if (source_out_ && source)
            *source_out_ = *source;
        result = static_cast<int> (std::min (length, static_cast<size_t> (INT_MAX)));
    }
    const int err = errno;
    zlink_msg_close (&part);
    errno = err;
    return result;
}

// ROUTER records carry the source RID as metadata, alongside their payload.
zlink_routing_id_t recv_routed_string_expect_success (
  void *socket_, const char *payload_, const char *expected_rid_ = NULL,
  zlink_part_flag_t expected_more_ = ZLINK_PART_FINAL);
void send_routed_string_expect_success (
  void *socket_, const char *routing_id_, const char *payload_);

inline zlink_recv_result_t zlink_subscribe (
  void *subject_, zlink_routing_id_t *source_rid_out_,
  zlink_msg_t **parts_out_, size_t *part_count_out_,
  char *topic_id_out_, size_t *topic_id_len_out_, int flags_)
{
    const size_t capacity = topic_id_len_out_ ? *topic_id_len_out_ : 0;
    return testutil_agg::recv_parts (
      source_rid_out_, parts_out_, part_count_out_,
      [=] (const zlink_routing_id_t **source_, zlink_msg_t *part_,
           zlink_part_flag_t *more_) {
          return zlink_subscribe_part (
            subject_, source_, topic_id_out_, capacity, topic_id_len_out_,
            part_, more_, static_cast<zlink_recv_flags_t> (flags_));
      });
}

inline int zlink_subscribe (
  void *subject_, zlink_msg_t **parts_, size_t *part_count_, int flags_,
  char *topic_id_out_, size_t *topic_id_len_)
{
    return zlink_subscribe (subject_, NULL, parts_, part_count_,
                            topic_id_out_, topic_id_len_, flags_);
}

inline zlink_recv_result_t zlink_subscription_event (
  void *subject_, zlink_routing_id_t *source_rid_out_, int *subscribed_out_,
  char *topic_id_out_, size_t *topic_id_len_out_, int flags_)
{
    const zlink_routing_id_t *source = NULL;
    const zlink_recv_result_t rc = zlink_xpub_recv_part (
      subject_, &source, subscribed_out_, topic_id_out_,
      topic_id_len_out_ ? *topic_id_len_out_ : 0, topic_id_len_out_,
      static_cast<zlink_recv_flags_t> (flags_));
    if (source_rid_out_) {
        memset (source_rid_out_, 0, sizeof (*source_rid_out_));
        if (rc == ZLINK_RECV_OK && source)
            *source_rid_out_ = *source;
    }
    return rc;
}

// Internal helper functions that are not intended to be directly called from
// tests. They must be declared in the header since they are used by macros.

int test_assert_success_message_errno_helper (int rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_submit_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_connect_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_bind_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_config_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_close_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_recv_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_errno_helper (zlink_handler_result_t rc_,
                                              const char *msg_,
                                              const char *expr_,
                                              int line);

int test_assert_success_message_raw_errno_helper (
  int rc_, const char *msg_, const char *expr_, int line, bool zero_ = false);

int test_assert_success_message_raw_zero_errno_helper (int rc_,
                                                       const char *msg_,
                                                       const char *expr_,
                                                       int line);

int test_assert_failure_message_raw_errno_helper (
  int rc_, int expected_errno_, const char *msg_, const char *expr_, int line);

int test_assert_failure_message_raw_errno_helper (
  zlink_submit_result_t rc_, int expected_errno_, const char *msg_, const char *expr_, int line);

/////////////////////////////////////////////////////////////////////////////
// Macros extending Unity's TEST_ASSERT_* macros in a similar fashion.
/////////////////////////////////////////////////////////////////////////////

// For TEST_ASSERT_SUCCESS_ERRNO, TEST_ASSERT_SUCCESS_MESSAGE_ERRNO and
// TEST_ASSERT_FAILURE_ERRNO, 'expr' must be an expression evaluating
// to a result in the style of a libzlink API function. Most public APIs use
// integer success/failure. Submit APIs may also return zlink_submit_result_t,
// where success is ZLINK_SUBMIT_OK and failure details remain available
// through zlink_errno ().
// TEST_ASSERT_SUCCESS_RAW_ERRNO and TEST_ASSERT_FAILURE_RAW_ERRNO are similar,
// but used with the native socket API functions, and expect that the error
// code can be retrieved in the native way (i.e. WSAGetLastError on Windows,
// and errno otherwise).

// Asserts that the libzlink API 'expr' is successful. In case of a failure, the
// assertion message includes the literal 'expr', the error number as
// determined by zlink_errno(), and the additional 'msg'.
// In case of success, the result of the macro is the result of 'expr'.
#define TEST_ASSERT_SUCCESS_MESSAGE_ERRNO(expr, msg)                                               \
    test_assert_success_message_errno_helper (expr, msg, #expr, __LINE__)

// Asserts that the libzlink API 'expr' is successful. In case of a failure, the
// assertion message includes the literal 'expr' and the error code.
// A typical use would be:
//   TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (socket, endpoint));
// In case of success, the result of the macro is the result of 'expr'.
//
// If an additional message should be displayed in case of a failure, use
// TEST_ASSERT_SUCCESS_MESSAGE_ERRNO.
#define TEST_ASSERT_SUCCESS_ERRNO(expr)                                                            \
    test_assert_success_message_errno_helper (expr, NULL, #expr, __LINE__)

// Asserts that the socket API 'expr' is successful. In case of a failure, the
// assertion message includes the literal 'expr' and the error code.
// A typical use would be:
//   TEST_ASSERT_SUCCESS_RAW_ERRNO (send (fd, buffer, 64, 0));
// In case of success, the result of the macro is the result of 'expr'.
// Success is strictly defined by a return value different from -1, as opposed
// to checking that it is 0, like TEST_ASSERT_FAILURE_RAW_ZERO_ERRNO does.
#define TEST_ASSERT_SUCCESS_RAW_ERRNO(expr)                                                        \
    test_assert_success_message_raw_errno_helper (expr, NULL, #expr, __LINE__)

// Asserts that the socket API 'expr' is successful. In case of a failure, the
// assertion message includes the literal 'expr' and the error code.
// A typical use would be:
//   TEST_ASSERT_SUCCESS_RAW_ZERO_ERRNO (send (fd, buffer, 64, 0));
// In case of success, the result of the macro is the result of 'expr'.
// Success is strictly defined by a return value of 0, as opposed to checking
// that it is not -1, like TEST_ASSERT_FAILURE_RAW_ERRNO does.
#define TEST_ASSERT_SUCCESS_RAW_ZERO_ERRNO(expr)                                                   \
    test_assert_success_message_raw_zero_errno_helper (expr, NULL, #expr, __LINE__)

// Asserts that the socket API 'expr' is not successful, and the error code is
// 'error_code'. In case of an unexpected succces, or a failure with an
// unexpected error code, the assertion message includes the literal 'expr'
// and, in case of a failure, the actual error code.
#define TEST_ASSERT_FAILURE_RAW_ERRNO(error_code, expr)                                            \
    test_assert_failure_message_raw_errno_helper (expr, error_code, NULL, #expr, __LINE__)

// Asserts that the libzlink API 'expr' is not successful, and the error code is
// 'error_code'. In case of an unexpected succces, or a failure with an
// unexpected error code, the assertion message includes the literal 'expr'
// and, in case of a failure, the actual error code.
#define TEST_ASSERT_FAILURE_ERRNO(error_code, expr)                                                \
    {                                                                                              \
        int _rc = (expr);                                                                          \
        TEST_ASSERT_EQUAL_INT (-1, _rc);                                                           \
        TEST_ASSERT_EQUAL_INT (error_code, errno);                                                 \
    }

/////////////////////////////////////////////////////////////////////////////
// Utility functions for testing sending and receiving.
/////////////////////////////////////////////////////////////////////////////

// Sends a string via a libzlink socket, and expects the operation to be
// successful (the meaning of which depends on the socket type and configured
// options, and might include dropping the message). Otherwise, a Unity test
// assertion is triggered.
// 'socket_' must be the libzlink socket to use for sending.
// 'str_' must be a 0-terminated string.
// 'flags_' are as documented by the zlink_send function.
void send_string_expect_success (void *socket_, const char *str_, int flags_);

void send_published_string_expect_success (
  void *publisher_, const char *topic_, const char *payload_);
void recv_subscribed_string_expect_success (
  void *subscriber_, const char *topic_, const char *payload_);

// Receives a message via a libzlink socket, and expects the operation to be
// successful, and the message to be a given string. Otherwise, a Unity test
// assertion is triggered.
// 'socket_' must be the libzlink socket to use for receiving.
// 'str_' must be a 0-terminated string.
// 'flags_' are as documented by the zlink_recv function.
void recv_string_expect_success (void *socket_, const char *str_, int flags_);

// Sends a byte array via a libzlink socket, and expects the operation to be
// successful (the meaning of which depends on the socket type and configured
// options, and might include dropping the message). Otherwise, a Unity test
// assertion is triggered.
// 'socket_' must be the libzlink socket to use for sending.
// 'array_' must be a C uint8_t array. The array size is automatically
// determined via template argument deduction.
// 'flags_' are as documented by the zlink_send function.
template <size_t SIZE>
void send_array_expect_success (void *socket_, const uint8_t (&array_)[SIZE], int flags_)
{
    const int rc = zlink_send (socket_, array_, SIZE, flags_);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (SIZE), rc);
}

// Receives a message via a libzlink socket, and expects the operation to be
// successful, and the message to be a given byte array. Otherwise, a Unity
// test assertion is triggered.
// 'socket_' must be the libzlink socket to use for receiving.
// 'array_' must be a C uint8_t array. The array size is automatically
// determined via template argument deduction.
// 'flags_' are as documented by the zlink_recv function.
template <size_t SIZE>
void recv_array_expect_success (void *socket_, const uint8_t (&array_)[SIZE], int flags_)
{
    char buffer[255];
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (sizeof (buffer), SIZE,
                                       "recv_string_expect_success cannot be "
                                       "used for strings longer than 255 "
                                       "characters");

    const int rc = TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (socket_, buffer, sizeof (buffer), flags_));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (SIZE), rc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (array_, buffer, SIZE);
}

// Attaches a discard handler appropriate for the given socket type.
// Returns 0 on success, -1 on failure.

/////////////////////////////////////////////////////////////////////////////
// Utility function for handling a test libzlink context, that is set up and
// torn down for each Unity test case, such that a clean context is available
// for each test case, and some consistency checks can be performed.
/////////////////////////////////////////////////////////////////////////////

// Use this is an test executable to perform a default setup and teardown of
// the test context, which is appropriate for many libzlink test cases.
#define SETUP_TEARDOWN_TESTCONTEXT                                                                 \
    void setUp ()                                                                                  \
    {                                                                                              \
        setup_test_context ();                                                                     \
    }                                                                                              \
    void tearDown ()                                                                               \
    {                                                                                              \
        teardown_test_context ();                                                                  \
    }

// The maximum number of sockets that can be managed by the test context.
#define MAX_TEST_SOCKETS 128

// Expected to be called during Unity's setUp function.
void setup_test_context ();

// Returns the test context, e.g. to create sockets in another thread using
// zlink_socket, or set context options.
void *get_test_context ();

// Expected to be called during Unity's tearDown function. Checks that all
// sockets created via test_context_socket have been properly closed using
// test_context_socket_close or test_context_socket_close_zero_linger, and generates a warning otherwise.
void teardown_test_context ();

// Creates a libzlink socket on the test context, and tracks its lifecycle.
// You MUST use test_context_socket_close or test_context_socket_close_zero_linger
// to close a socket created via this function, otherwise undefined behaviour
// will result.
// CAUTION: this function is not thread-safe, and may only be used from the
// main thread.
void *test_context_socket (int type_);

// Closes a socket created via test_context_socket.
// CAUTION: this function is not thread-safe, and may only be used from the
// main thread.
void *test_context_socket_close (void *socket_);

// Closes a socket created via test_context_socket after setting its linger
// timeout to 0.
// CAUTION: this function is not thread-safe, and may only be used from the
// main thread.
void *test_context_socket_close_zero_linger (void *socket_);
void test_context_socket_mark_closed (void *socket_);

/////////////////////////////////////////////////////////////////////////////
// Utility function for handling wildcard binds.
/////////////////////////////////////////////////////////////////////////////

// All function binds a socket to some wildcard address, and retrieve the bound
// endpoint via the ZLINK_INTERNAL_OPT_LAST_ENDPOINT socket option to a given buffer.
// Triggers a Unity test assertion in case of a failure (including the buffer
// being too small for the resulting endpoint string).

// Binds to an explicitly given wildcard address. Protocol-specific helpers such
// as bind_loopback_ipv4 should be preferred when a test does not need a custom
// transport URI.
void test_bind (void *socket_, const char *bind_address_, char *my_endpoint_, size_t len_);

// Binds to a tcp endpoint using the ipv4 or ipv6 loopback wildcard address.
void bind_loopback (void *socket_, int ipv6_, char *my_endpoint_, size_t len_);

typedef void (*bind_function_t) (void *socket_, char *my_endpoint_, size_t len_);

// Binds to a tcp endpoint using the ipv4 loopback wildcard address.
void bind_loopback_ipv4 (void *socket_, char *my_endpoint_, size_t len_);

// Binds to a tcp endpoint using the ipv6 loopback wildcard address.
void bind_loopback_ipv6 (void *socket_, char *my_endpoint_, size_t len_);

// Binds to an ipc endpoint using the ipc wildcard address.
// Note that the returned address cannot be reused to bind a second socket.
// If you need to do this, use make_random_ipc_endpoint instead.
void bind_loopback_ipc (void *socket_, char *my_endpoint_, size_t len_);

#if defined(ZLINK_HAVE_IPC)
// utility function to create a random IPC endpoint, similar to what a ipc://*
// wildcard binding does, but in a way it can be reused for multiple binds
void make_random_ipc_endpoint (char *out_endpoint_, size_t len_);
void make_random_ipc_endpoint (char *out_endpoint_);
// Read a versioned public context budget snapshot and assert API success.
zlink_auto_hwm_budget_snapshot_t read_auto_hwm_budget_snapshot (void *ctx_);

#endif
