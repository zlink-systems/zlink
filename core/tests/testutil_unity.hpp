#pragma once

/* SPDX-License-Identifier: MPL-2.0 */

#include "../include/zlink.h"
#include "../src/runtime/core/internal_defs.hpp"
#include "../src/runtime/utils/precompiled.hpp"
#include "../src/runtime/core/recv_internal.hpp"
#include "../src/runtime/core/send_internal.hpp"
#include "../src/runtime/sockets/stream/stream.hpp"
#include "../src/api/message/recv_result_internal.hpp"
#include "../src/api/message/submit_result_internal.hpp"
#include "../src/api/socket/socket_message_api_internal.hpp"

#include "testutil.hpp"

#include <algorithm>
#include <climits>
#include <chrono>
#include <cstring>
#include <thread>
#include <unity.h>

#ifndef ZLINK_SNDMORE
#define ZLINK_SNDMORE ((zlink_send_flags_t) 0x0002u)
#endif

#ifdef __cplusplus
inline int test_send_single_msg (zlink_msg_t *msg_, void *s_, int flags_)
{
    return zlink::send_msg_internal (s_, msg_, static_cast<zlink_send_flags_t> (flags_));
}

inline int test_recv_single_msg (zlink_msg_t *msg_, void *s_, int flags_)
{
    return zlink::recv_msg_internal (s_, msg_, static_cast<zlink_send_flags_t> (flags_));
}

inline int test_stream_send_bytes (
  void *s_, const zlink_routing_id_t *rid_, const void *data_, size_t size_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, size_) != 0)
        return -1;
    if (size_ > 0 && data_)
        memcpy (zlink_msg_data (&msg), data_, size_);

    const zlink_submit_result_t rc = zlink_send_part_rid (
      s_, rid_, &msg, static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT), ZLINK_PART_FINAL);
    if (rc != ZLINK_SUBMIT_OK) {
        const int err = errno;
        zlink_msg_close (&msg);
        errno = err;
        return -1;
    }
    errno = 0;
    return static_cast<int> (size_ < static_cast<size_t> (INT_MAX) ? size_ : INT_MAX);
}

inline int test_stream_send_single_msg (void *s_,
                                        const zlink_routing_id_t *rid_,
                                        zlink_msg_t *msg_,
                                        int flags_)
{
    if (!msg_) {
        errno = EINVAL;
        return -1;
    }
    const size_t size = zlink_msg_size (msg_);
    const zlink_submit_result_t rc = zlink_send_part_rid (
      s_, rid_, msg_, static_cast<zlink_send_flags_t> (flags_ & ZLINK_DONTWAIT), ZLINK_PART_FINAL);
    if (rc != ZLINK_SUBMIT_OK)
        return -1;
    errno = 0;
    return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
}

inline int zlink_send (void *s_, const void *buf_, size_t len_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, len_) != 0)
        return -1;
    if (len_ > 0 && buf_)
        memcpy (zlink_msg_data (&msg), buf_, len_);

    int type = 0;
    size_t type_size = sizeof (type);
    if (zlink_get_option (s_, ZLINK_OPT_TYPE, &type, &type_size) == ZLINK_CONFIG_OK) {
        const int rc = zlink::send_msg_internal (s_, &msg, flags_);
        if (rc < 0) {
            const int err = errno;
            zlink_msg_close (&msg);
            errno = err;
            return -1;
        }
    } else {
        const zlink_part_flag_t pf =
          (flags_ & static_cast<int> (ZLINK_SNDMORE)) ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc = zlink_send_part (
          s_, &msg, static_cast<zlink_send_flags_t> (flags_ & ~static_cast<int> (ZLINK_SNDMORE)),
          pf);
        if (rc != ZLINK_SUBMIT_OK) {
            const int err = errno;
            zlink_msg_close (&msg);
            errno = err;
            return -1;
        }
    }
    return static_cast<int> (len_);
}

namespace testutil_agg
{
inline void consume_part (zlink_msg_t *part_)
{
    if (!part_)
        return;

    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (!msg->check ())
        return;

    const int close_rc = zlink_msg_close (part_);
    zlink_assert (close_rc == 0);
    const int init_rc = zlink_msg_init (part_);
    zlink_assert (init_rc == 0);
}

inline void consume_parts (zlink_msg_t *parts_, size_t count_)
{
    if (!parts_)
        return;

    for (size_t i = 0; i < count_; ++i)
        consume_part (&parts_[i]);
}

inline bool should_consume_after_bulk_attempt (int rc_, int err_)
{
    return rc_ == 0 || err_ != EAGAIN;
}

inline zlink_submit_result_t
finish_bulk_attempt (int rc_, int err_, zlink_msg_t *parts_, size_t count_)
{
    if (should_consume_after_bulk_attempt (rc_, err_))
        consume_parts (parts_, count_);
    errno = err_;
    return zlink::submit_result_internal::from_rc (rc_);
}

/* Aggregate send helpers prefer the direct bulk path so tests observe the
 * same public send contract as bindings/c. */
inline zlink_submit_result_t
send_parts (void *s_, zlink_msg_t *parts_, size_t count_, zlink_send_flags_t flags_)
{
    if (!parts_ || count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    const int socket_rc = zlink_socket_send_internal (s_, parts_, count_, flags_);
    return finish_bulk_attempt (socket_rc, errno, parts_, count_);
}

inline zlink_submit_result_t send_parts_rid (void *s_,
                                             const zlink_routing_id_t *rid_,
                                             zlink_msg_t *parts_,
                                             size_t count_,
                                             zlink_send_flags_t flags_)
{
    if (!parts_ || count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    const int socket_rc = zlink_socket_send_rid_internal (s_, rid_, parts_, count_, flags_);
    return finish_bulk_attempt (socket_rc, errno, parts_, count_);
}

inline zlink_submit_result_t publish_parts (void *subject_,
                                            const char *topic_id_,
                                            zlink_msg_t *parts_,
                                            size_t count_,
                                            zlink_send_flags_t flags_)
{
    if (!parts_ && count_ != 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }

    const int socket_rc =
      zlink_socket_publish_internal (subject_, topic_id_, parts_, count_, flags_);
    return finish_bulk_attempt (socket_rc, errno, parts_, count_);
}
}

inline zlink_submit_result_t zlink_send_rid (void *s_,
                                             const zlink_routing_id_t *target_rid_,
                                             zlink_msg_t *parts_,
                                             size_t part_count_,
                                             int flags_)
{
    return testutil_agg::send_parts_rid (s_, target_rid_, parts_, part_count_,
                                         static_cast<zlink_send_flags_t> (flags_));
}

inline zlink_submit_result_t
zlink_send (void *s_, zlink_msg_t *parts_, size_t part_count_, int flags_)
{
    return testutil_agg::send_parts (s_, parts_, part_count_,
                                     static_cast<zlink_send_flags_t> (flags_));
}

inline zlink_submit_result_t zlink_publish (void *subject_,
                                            const char *topic_id_,
                                            zlink_msg_t *parts_,
                                            size_t part_count_,
                                            zlink_send_flags_t flags_)
{
    const zlink_submit_result_t bulk_rc =
      testutil_agg::publish_parts (subject_, topic_id_, parts_, part_count_, flags_);
    if (bulk_rc != ZLINK_SUBMIT_INVALID_HANDLE || part_count_ == 0)
        return bulk_rc;

    for (size_t i = 0; i < part_count_; ++i) {
        const zlink_part_flag_t f = i + 1 < part_count_ ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc =
          zlink_publish_part (subject_, topic_id_, &parts_[i], flags_, f);
        if (rc != ZLINK_SUBMIT_OK) {
            for (size_t j = i + 1; j < part_count_; ++j)
                zlink_msg_close (&parts_[j]);
            return rc;
        }
    }
    return ZLINK_SUBMIT_OK;
}
inline zlink_submit_result_t zlink_publish (
  void *subject_, const char *topic_id_, zlink_msg_t *parts_, size_t part_count_, int flags_)
{
    return zlink_publish (subject_, topic_id_, parts_, part_count_,
                          static_cast<zlink_send_flags_t> (flags_));
}

inline zlink_submit_result_t zlink_dealer_request (void *dealer_,
                                                   zlink_msg_t *parts_,
                                                   size_t part_count_,
                                                   zlink_reply_handler_fn handler_,
                                                   void *userdata_,
                                                   int flags_,
                                                   uint32_t timeout_ms_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        const bool is_final = i + 1 == part_count_;
        const zlink_part_flag_t f = is_final ? ZLINK_PART_FINAL : ZLINK_PART_MORE;
        const zlink_submit_result_t rc = zlink_dealer_request_part (
          dealer_, &parts_[i], static_cast<zlink_send_flags_t> (flags_), f,
          is_final ? timeout_ms_ : 0u, is_final ? handler_ : NULL, is_final ? userdata_ : NULL);
        if (rc != ZLINK_SUBMIT_OK) {
            for (size_t j = i + 1; j < part_count_; ++j)
                zlink_msg_close (&parts_[j]);
            return rc;
        }
    }
    return ZLINK_SUBMIT_OK;
}

inline zlink_submit_result_t zlink_router_request (void *router_,
                                                   const zlink_routing_id_t *peer_rid_,
                                                   zlink_msg_t *parts_,
                                                   size_t part_count_,
                                                   zlink_reply_handler_fn handler_,
                                                   void *userdata_,
                                                   int flags_,
                                                   uint32_t timeout_ms_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        const bool is_final = i + 1 == part_count_;
        const zlink_part_flag_t f = is_final ? ZLINK_PART_FINAL : ZLINK_PART_MORE;
        const zlink_submit_result_t rc = zlink_router_request_part (
          router_, peer_rid_, &parts_[i], static_cast<zlink_send_flags_t> (flags_), f,
          is_final ? timeout_ms_ : 0u, is_final ? handler_ : NULL, is_final ? userdata_ : NULL);
        if (rc != ZLINK_SUBMIT_OK) {
            for (size_t j = i + 1; j < part_count_; ++j)
                zlink_msg_close (&parts_[j]);
            return rc;
        }
    }
    return ZLINK_SUBMIT_OK;
}



namespace testutil_agg
{
static thread_local std::vector<zlink_msg_t> tl_recv_buf;
static thread_local zlink_routing_id_t tl_source_node_rid;

inline void copy_routing_id_view (const zlink_routing_id_t *src_, zlink_routing_id_t *dest_)
{
    if (!dest_)
        return;

    memset (dest_, 0, sizeof (*dest_));
    if (!src_)
        return;

    memcpy (dest_, src_, sizeof (*dest_));
}

template <typename NextPartFn>
inline zlink_recv_result_t collect_more_recv_part_impl (NextPartFn next_part_fn_)
{
    while (true) {
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        zlink_msg_t part;
        zlink_msg_init (&part);
        const zlink_recv_result_t rc = next_part_fn_ (&part, &more);
        if (rc != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            for (auto &p : tl_recv_buf)
                zlink_msg_close (&p);
            tl_recv_buf.clear ();
            return rc;
        }
        tl_recv_buf.push_back (part);
        if (!more)
            break;
    }
    return ZLINK_RECV_OK;
}

/* Collect remaining parts after first has been received. */
inline zlink_recv_result_t collect_more_recv_part (void *s_, zlink_recv_flags_t flags_)
{
    return collect_more_recv_part_impl (
      [s_, flags_] (zlink_msg_t *part_, zlink_part_flag_t *more_) {
          const zlink_routing_id_t *dummy = NULL;
          return zlink_recv_part (s_, &dummy, part_, more_, flags_);
      });
}

inline zlink_recv_result_t collect_more_router_recv_part (void *router_, zlink_recv_flags_t flags_)
{
    return collect_more_recv_part_impl (
      [router_, flags_] (zlink_msg_t *part_, zlink_part_flag_t *more_) {
          const zlink_routing_id_t *source_node_rid = NULL;
          uint64_t request_seq = 0;
          return zlink_router_recv_part (router_, &source_node_rid, &request_seq,
                                         part_, more_, flags_);
      });
}



inline zlink_recv_result_t finalize_recv (zlink_msg_t **parts_out_, size_t *count_out_)
{
    //  Hand out the thread-local buffer directly: callers close the parts via
    //  zlink_multipart_close() before the next aggregate recv reuses it, and
    //  the array itself must not be freed.
    *parts_out_ = tl_recv_buf.data ();
    *count_out_ = tl_recv_buf.size ();
    return ZLINK_RECV_OK;
}
} // namespace testutil_agg

inline zlink_recv_result_t zlink_recv (void *s_,
                                       zlink_routing_id_t *source_rid_out_,
                                       zlink_msg_t **parts_out_,
                                       size_t *part_count_out_,
                                       int flags_)
{
    return zlink::recv_result_internal::from_rc (zlink_socket_recv_internal (
      s_, source_rid_out_, parts_out_, part_count_out_, static_cast<zlink_send_flags_t> (flags_)));
}

inline zlink_recv_result_t zlink_router_recv (void *router_,
                                              const zlink_routing_id_t **source_node_rid_out_,
                                              uint64_t *request_seq_out_,
                                              zlink_msg_t **parts_out_,
                                              size_t *part_count_out_,
                                              int flags_)
{
    testutil_agg::tl_recv_buf.clear ();
    zlink_msg_t first;
    zlink_msg_init (&first);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const zlink_recv_result_t rc =
      zlink_router_recv_part (router_, source_node_rid_out_, request_seq_out_,
                              &first, &has_more, static_cast<zlink_recv_flags_t> (flags_));
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&first);
        return rc;
    }
    testutil_agg::copy_routing_id_view (source_node_rid_out_ ? *source_node_rid_out_ : NULL,
                                        &testutil_agg::tl_source_node_rid);
    testutil_agg::tl_recv_buf.push_back (first);
    if (has_more) {
        const zlink_recv_result_t rc2 = testutil_agg::collect_more_router_recv_part (
          router_, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc2 != ZLINK_RECV_OK)
            return rc2;
    }
    if (source_node_rid_out_) {
        *source_node_rid_out_ =
          testutil_agg::tl_source_node_rid.size > 0 ? &testutil_agg::tl_source_node_rid : NULL;
    }
    return testutil_agg::finalize_recv (parts_out_, part_count_out_);
}


inline int zlink_recv (void *s_, void *buf_, size_t len_, int flags_)
{
    int type = 0;
    size_t type_size = sizeof (type);
    if (zlink_get_option (s_, ZLINK_OPT_TYPE, &type, &type_size) == ZLINK_CONFIG_OK)
        return zlink::recv_buffer_internal (s_, buf_, len_, flags_);

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_recv_result_t rc =
      zlink_recv (s_, NULL, &parts, &part_count, static_cast<zlink_recv_flags_t> (flags_));
    if (rc != ZLINK_RECV_OK)
        return -1;

    const size_t copy_len = part_count > 0 ? std::min (len_, zlink_msg_size (&parts[0])) : 0;
    if (buf_ && copy_len > 0)
        memcpy (buf_, zlink_msg_data (&parts[0]), copy_len);
    const int result = part_count > 0 ? static_cast<int> (zlink_msg_size (&parts[0])) : 0;
    zlink_multipart_close (parts, part_count);
    return result;
}

inline int zlink_subscribe (void *subject_,
                            zlink_msg_t **parts_,
                            size_t *part_count_,
                            int flags_,
                            char *topic_id_out_,
                            size_t *topic_id_len_)
{
    return zlink::recv_result_internal::from_rc (zlink_socket_subscribe_recv_internal (
      subject_, NULL, parts_, part_count_, topic_id_out_, topic_id_len_,
      static_cast<zlink_send_flags_t> (flags_)));
}

inline zlink_recv_result_t zlink_subscribe (void *subject_,
                                            zlink_routing_id_t *source_rid_out_,
                                            zlink_msg_t **parts_out_,
                                            size_t *part_count_out_,
                                            char *topic_id_out_,
                                            size_t *topic_id_len_out_,
                                            int flags_)
{
    return zlink::recv_result_internal::from_rc (zlink_socket_subscribe_recv_internal (
      subject_, source_rid_out_, parts_out_, part_count_out_, topic_id_out_, topic_id_len_out_,
      static_cast<zlink_send_flags_t> (flags_)));
}

inline zlink_recv_result_t zlink_subscription_event (void *subject_,
                                                     zlink_routing_id_t *source_rid_out_,
                                                     int *subscribed_out_,
                                                     char *topic_id_out_,
                                                     size_t *topic_id_len_out_,
                                                     int flags_)
{
    return zlink::recv_result_internal::from_rc (zlink_socket_xpub_recv_internal (
      subject_, source_rid_out_, subscribed_out_, topic_id_out_, topic_id_len_out_,
      static_cast<zlink_send_flags_t> (flags_)));
}

inline zlink_submit_result_t zlink_router_reply (void *router_,
                                                 const zlink_routing_id_t *peer_rid_,
                                                 uint64_t request_seq_,
                                                 zlink_msg_t *parts_,
                                                 size_t part_count_)
{
    if (!parts_ || part_count_ == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    for (size_t i = 0; i < part_count_; ++i) {
        const zlink_part_flag_t f = i + 1 < part_count_ ? ZLINK_PART_MORE : ZLINK_PART_FINAL;
        const zlink_submit_result_t rc =
          zlink_router_reply_part (router_, peer_rid_, request_seq_, &parts_[i], f);
        if (rc != ZLINK_SUBMIT_OK) {
            for (size_t j = i + 1; j < part_count_; ++j)
                zlink_msg_close (&parts_[j]);
            return rc;
        }
    }
    return ZLINK_SUBMIT_OK;
}







inline bool test_msg_has_more (const zlink_msg_t *msg_)
{
    return msg_
           && (reinterpret_cast<const zlink::msg_t *> (msg_)->flags () & zlink::msg_t::more) != 0;
}

inline int test_msg_refcnt (const zlink_msg_t *msg_)
{
    if (!msg_)
        return 0;
    const zlink::msg_t *internal = reinterpret_cast<const zlink::msg_t *> (msg_);
    return static_cast<int> (internal->refcnt_value ());
}
#endif

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
      zlink::recv_buffer_internal (socket_, buffer, sizeof (buffer), flags_));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (SIZE), rc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (array_, buffer, SIZE);
}

// Attaches a discard handler appropriate for the given socket type.
// Returns 0 on success, -1 on failure.
int test_attach_discard_handler_for_type (void *socket_, int type_);

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
#endif
