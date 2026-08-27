/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/multipart_send_txn.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <string.h>
#include <thread>

#if !defined _WIN32
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
bool should_run_request_reply_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

struct reply_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    zlink_request_result_t result;
    size_t part_count;
    std::string payload;
    size_t callback_count;
    void *progress_handle;

    reply_probe_t () :
        done (false),
        result (ZLINK_REQUEST_PROTOCOL_ERROR),
        part_count (0),
        callback_count (0),
        progress_handle (NULL)
    {
    }
};

struct reply_reentry_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    void *dealer;
    void *different_dealer;
    zlink_submit_result_t same_request;
    zlink_submit_result_t different_request;
    zlink_submit_result_t same_async;
    zlink_submit_result_t different_async;
    int same_request_errno;
    int different_request_errno;
    int same_async_errno;
    int different_async_errno;
    bool done;

    reply_reentry_probe_t () :
        dealer (NULL), different_dealer (NULL),
        same_request (ZLINK_SUBMIT_INTERNAL_ERROR),
        different_request (ZLINK_SUBMIT_INTERNAL_ERROR),
        same_async (ZLINK_SUBMIT_INTERNAL_ERROR),
        different_async (ZLINK_SUBMIT_INTERNAL_ERROR),
        same_request_errno (0), different_request_errno (0),
        same_async_errno (0), different_async_errno (0), done (false)
    {
    }
};

struct completion_owner_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    zlink_request_result_t result;
    size_t callback_count;
    std::string payload;
    std::thread::id callback_thread;

    completion_owner_probe_t () :
        done (false),
        result (ZLINK_REQUEST_PROTOCOL_ERROR),
        callback_count (0)
    {
    }

    void reset ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        done = false;
        result = ZLINK_REQUEST_PROTOCOL_ERROR;
        callback_count = 0;
        payload.clear ();
        callback_thread = std::thread::id ();
    }
};

struct receive_wait_owner_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered;

    receive_wait_owner_probe_t () : entered (false) {}
};

struct routed_self_close_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool closed;
    bool writable;
    zlink_close_result_t result;
    int errnum;

    routed_self_close_probe_t () :
        closed (false), writable (false),
        result (ZLINK_CLOSE_INTERNAL_ERROR), errnum (0)
    {
    }
};

void capture_receive_wait_owner (void *userdata_)
{
    receive_wait_owner_probe_t *probe =
      static_cast<receive_wait_owner_probe_t *> (userdata_);
    if (!probe)
        return;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->entered = true;
    }
    probe->cv.notify_all ();
}

struct request_handler_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool invoked;
    uint64_t request_seq;
    std::string peer_rid;
    std::string request_payload;
    zlink_routing_id_t peer_rid_value;

    request_handler_probe_t () : invoked (false), request_seq (0)
    {
        memset (&peer_rid_value, 0, sizeof (peer_rid_value));
    }
};

struct request_event_t
{
    uint64_t request_seq;
    std::string peer_rid;
    zlink_routing_id_t peer_rid_value;
    std::string request_payload;
};

struct multi_request_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<request_event_t> events;
};

struct blocking_callback_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered;
    bool release;

    blocking_callback_probe_t () : entered (false), release (false) {}
};

void capture_reply (zlink_request_result_t result_,
                    zlink_msg_t *parts_,
                    size_t part_count_,
                    void *userdata_);

void capture_completion_owner_reply (zlink_request_result_t result_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     void *userdata_)
{
    completion_owner_probe_t *probe =
      static_cast<completion_owner_probe_t *> (userdata_);
    TEST_ASSERT_NOT_NULL (probe);
    const std::string payload = part_count_ > 0
                                  ? std::string (static_cast<const char *> (
                                                  zlink_msg_data (&parts_[0])),
                                                zlink_msg_size (&parts_[0]))
                                  : std::string ();
    zlink_multipart_close (parts_, part_count_);

    std::lock_guard<std::mutex> lock (probe->mutex);
    probe->done = true;
    probe->result = result_;
    ++probe->callback_count;
    probe->payload = payload;
    probe->callback_thread = std::this_thread::get_id ();
    probe->cv.notify_all ();
}

void ignore_routed_ready (void *, const zlink_send_complete_event_t *, void *)
{
}

void close_on_routed_terminal (
  void *socket_, const zlink_send_complete_event_t *event_, void *userdata_)
{
    if (!event_)
        return;

    routed_self_close_probe_t *probe =
      static_cast<routed_self_close_probe_t *> (userdata_);
    //  An admitted record is the positive proof the exact target accepted a
    //  send, which is what the readiness edge used to stand in for.
    if (event_->result == ZLINK_SEND_ADMITTED) {
        {
            std::lock_guard<std::mutex> lock (probe->mutex);
            probe->writable = true;
        }
        probe->cv.notify_all ();
        return;
    }
    if (event_->result != ZLINK_SEND_TERMINAL)
        return;
    const zlink_close_result_t result = zlink_close (socket_);
    const int errnum = result == ZLINK_CLOSE_OK ? 0 : zlink_errno ();
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->closed = true;
        probe->result = result;
        probe->errnum = errnum;
    }
    probe->cv.notify_all ();
}

void init_string_part (zlink_msg_t *part_, const char *text_)
{
    const size_t size = strlen (text_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, size));
    memcpy (zlink_msg_data (part_), text_, size);
}

void block_reply_until_released (zlink_request_result_t,
                                 zlink_msg_t *parts_,
                                 size_t part_count_,
                                 void *userdata_)
{
    blocking_callback_probe_t *probe =
      static_cast<blocking_callback_probe_t *> (userdata_);
    zlink_multipart_close (parts_, part_count_);

    std::unique_lock<std::mutex> lock (probe->mutex);
    probe->entered = true;
    probe->cv.notify_all ();
    probe->cv.wait (lock, [probe] { return probe->release; });
}

void send_raw_request_frame (void *dealer_,
                             const void *data_,
                             size_t size_,
                             zlink_part_flag_t part_flag_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, size_));
    if (size_ > 0)
        memcpy (zlink_msg_data (&part), data_, size_);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer_, &part, ZLINK_SEND_FLAGS_NONE, part_flag_));
}

void send_raw_request_envelope (void *dealer_, uint64_t request_seq_)
{
    const unsigned char protocol = zlink::request_reply::protocol_id;
    const unsigned char version = zlink::request_reply::version;
    const unsigned char type = zlink::request_reply::request_type;
    unsigned char sequence[8];
    zlink::request_reply::encode_u64_be (request_seq_, sequence);
    const unsigned char payload = 'r';
    send_raw_request_frame (dealer_, &protocol, sizeof (protocol), ZLINK_PART_MORE);
    send_raw_request_frame (dealer_, &version, sizeof (version), ZLINK_PART_MORE);
    send_raw_request_frame (dealer_, &type, sizeof (type), ZLINK_PART_MORE);
    send_raw_request_frame (dealer_, sequence, sizeof (sequence), ZLINK_PART_MORE);
    send_raw_request_frame (dealer_, &payload, sizeof (payload), ZLINK_PART_FINAL);
}

void configure_submit_retry (void *socket_)
{
    int retry_mode = ZLINK_SUBMIT_RETRY_LOCAL_FAILURE;
    int retry_timeout = 200;
    int retry_attempts = 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_MODE, &retry_mode, sizeof (retry_mode)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_TIMEOUT,
                                                 &retry_timeout, sizeof (retry_timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS,
                                                 &retry_attempts, sizeof (retry_attempts)));
}

std::string part_to_string_and_close (zlink_msg_t *part_)
{
    TEST_ASSERT_NOT_NULL (part_);
    std::string value (static_cast<const char *> (zlink_msg_data (part_)), zlink_msg_size (part_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (part_));
    return value;
}

zlink_recv_result_t recv_dealer_part_with_retry (void *dealer_,
                                                 uint8_t *message_type_out_,
                                                 uint64_t *request_seq_out_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc =
          zlink_dealer_recv_part (dealer_, message_type_out_, request_seq_out_, part_out_,
                                  has_more_out_, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == ZLINK_RECV_OK)
            return rc;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_dealer_recv_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

zlink_recv_result_t recv_generic_part_with_retry (void *dealer_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        const zlink_recv_result_t rc =
          zlink_recv_part (dealer_, &source_rid, part_out_, has_more_out_,
                           static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == ZLINK_RECV_OK) {
            TEST_ASSERT_NULL (source_rid);
            return rc;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_recv_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

void arm_dealer_recv_part (void *dealer_)
{
    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t part;
    zlink_msg_init (&part);
    const zlink_recv_result_t rc =
      zlink_dealer_recv_part (dealer_, &message_type, &request_seq, &part, &has_more,
                              static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void set_routing_id_text (void *handle_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (handle_, text_, strlen (text_)));
}

zlink_routing_id_t get_routing_id_value (void *handle_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_routing_id (handle_, &rid));
    return rid;
}

std::string msg_to_string (const zlink_msg_t *part_)
{
    zlink_msg_t *mutable_part = const_cast<zlink_msg_t *> (part_);
    return std::string (static_cast<const char *> (zlink_msg_data (mutable_part)),
                        zlink_msg_size (part_));
}

int drain_completion_via_poller (void *subject_)
{
    void *poller = zlink_poller_new ();
    if (!poller)
        return -1;
    int rc = -1;
    if (zlink_poller_add (poller, subject_, NULL, ZLINK_POLLCOMPLETION) == ZLINK_CONFIG_OK) {
        zlink_poller_event_t event;
        rc = zlink_poller_wait (poller, &event, 1, 0, NULL);
        (void) zlink_poller_remove (poller, subject_);
    }
    (void) zlink_poller_destroy (&poller);
    return rc;
}

bool wait_for_reply (reply_probe_t *probe_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);
    while (std::chrono::steady_clock::now () < deadline) {
        {
            std::unique_lock<std::mutex> lock (probe_->mutex);
            if (probe_->cv.wait_for (lock, std::chrono::milliseconds (10),
                                     [probe_] () { return probe_->done; }))
                return true;
        }
        if (probe_->progress_handle)
            (void) drain_completion_via_poller (probe_->progress_handle);
    }

    return false;
}

bool wait_for_reply_count (reply_probe_t *probe_, size_t expected_count_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);
    while (std::chrono::steady_clock::now () < deadline) {
        {
            std::unique_lock<std::mutex> lock (probe_->mutex);
            if (probe_->cv.wait_for (lock, std::chrono::milliseconds (10),
                                     [probe_, expected_count_] () {
                                         return probe_->callback_count >= expected_count_;
                                     }))
                return true;
        }
        if (probe_->progress_handle)
            (void) drain_completion_via_poller (probe_->progress_handle);
    }

    return false;
}

zlink_close_result_t close_after_reply_callback (void *socket_)
{
    // reply_probe_t publishes completion from inside the callback.  A waiter
    // can observe it just before the callback scope returns, where close is
    // contractually retryable with CLOSE_BUSY/EBUSY.
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    zlink_close_result_t rc = ZLINK_CLOSE_BUSY;
    do {
        rc = zlink_close (socket_);
        if (rc == ZLINK_CLOSE_OK)
            return rc;
        if (rc != ZLINK_CLOSE_BUSY || zlink_errno () != EBUSY)
            return rc;
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
    return rc;
}

void close_test_socket_after_reply_callback (void *socket_)
{
    const int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           close_after_reply_callback (socket_));
    test_context_socket_mark_closed (socket_);
}

bool wait_for_request_handler (request_handler_probe_t *probe_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (SETTLE_TIME * 4),
                                [probe_] () { return probe_->invoked; });
}

bool wait_for_multi_request_count (multi_request_probe_t *probe_, size_t expected_count_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::milliseconds (SETTLE_TIME * 20),
      [probe_, expected_count_] () { return probe_->events.size () >= expected_count_; });
}

void recv_router_request_into_probe (void *router_, request_handler_probe_t *probe_)
{
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router_, &peer_rid,
                                                  &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_NOT_NULL (probe_);
    TEST_ASSERT_NOT_NULL (peer_rid);
    {
        std::lock_guard<std::mutex> lock (probe_->mutex);
        probe_->invoked = true;
        probe_->request_seq = request_seq;
        probe_->peer_rid_value = *peer_rid;
        probe_->peer_rid.assign (reinterpret_cast<const char *> (peer_rid->data), peer_rid->size);
        probe_->request_payload = part_count > 0 ? msg_to_string (&parts[0]) : std::string ();
    }
    zlink_multipart_close (parts, part_count);
    probe_->cv.notify_all ();
}

void recv_router_request_into_event (void *router_, multi_request_probe_t *probe_)
{
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router_, &peer_rid,
                                                  &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_NOT_NULL (probe_);
    TEST_ASSERT_NOT_NULL (peer_rid);

    request_event_t event;
    event.request_seq = request_seq;
    event.peer_rid_value = *peer_rid;
    event.peer_rid.assign (reinterpret_cast<const char *> (peer_rid->data), peer_rid->size);
    event.request_payload = part_count > 0 ? msg_to_string (&parts[0]) : std::string ();

    {
        std::lock_guard<std::mutex> lock (probe_->mutex);
        probe_->events.push_back (event);
    }
    zlink_multipart_close (parts, part_count);
    probe_->cv.notify_all ();
}

int run_request_reply_exit_child ()
{
    void *ctx = zlink_ctx_new ();
    if (!ctx)
        return 10;

    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    if (!router || !dealer)
        return 11;

    set_routing_id_text (dealer, "rr-exit-dealer");
    if (zlink_bind (router, "inproc://rr-exit-regression") != 0
        || zlink_connect (dealer, "inproc://rr-exit-regression") != 0)
        return 12;

    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "ping");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    if (zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000)
        != ZLINK_SUBMIT_OK)
        return 13;

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    if (zlink_router_recv (router, &source_rid, &request_seq, &parts, &part_count,
                           ZLINK_RECV_FLAGS_NONE)
        != ZLINK_RECV_OK)
        return 14;

    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, "pong");
    if (zlink_router_reply (router, source_rid, request_seq, &reply_part, 1) != ZLINK_SUBMIT_OK)
        return 15;
    zlink_multipart_close (parts, part_count);

    if (!wait_for_reply (&reply_probe))
        return 16;

    if (close_after_reply_callback (dealer) != ZLINK_CLOSE_OK)
        return 17;
    if (zlink_close (router) != 0)
        return 18;
    if (zlink_ctx_term (ctx) != 0)
        return 19;

    return 0;
}

void test_reserved_request_reply_message_type_is_invalid ()
{
    const uint8_t reserved_message_type = 0x04;
    TEST_ASSERT_FALSE (
      zlink::request_reply::is_valid_message_type (reserved_message_type));

    zlink::request_reply::envelope_control_data_t control;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::request_reply::encode_envelope_control_data (
            reserved_message_type, 1, &control));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
}

void test_request_reply_process_exits_cleanly_after_round_trip ()
{
#if defined _WIN32
    TEST_IGNORE_MESSAGE ("POSIX-only subprocess regression test");
#else
    pid_t child = fork ();
    TEST_ASSERT_TRUE (child >= 0);

    if (child == 0) {
        setup_test_environment (5);
        const int rc = run_request_reply_exit_child ();
        fflush (NULL);
        std::_Exit (rc);
    }

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    int status = 0;
    pid_t wait_rc = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        wait_rc = waitpid (child, &status, WNOHANG);
        TEST_ASSERT_TRUE (wait_rc >= 0);
        if (wait_rc == child)
            break;
        msleep (10);
    }

    if (wait_rc != child) {
        kill (child, SIGKILL);
        (void) waitpid (child, &status, 0);
        TEST_FAIL_MESSAGE ("request/reply child process did not exit after round trip");
    }

    TEST_ASSERT_TRUE (WIFEXITED (status));
    TEST_ASSERT_EQUAL_INT (0, WEXITSTATUS (status));
#endif
}

void capture_reply (zlink_request_result_t result_,
                    zlink_msg_t *parts_,
                    size_t part_count_,
                    void *userdata_)
{
    reply_probe_t *probe = static_cast<reply_probe_t *> (userdata_);
    if (!probe)
        return;

    std::lock_guard<std::mutex> lock (probe->mutex);
    probe->done = true;
    probe->result = result_;
    probe->part_count = part_count_;
    probe->payload = part_count_ > 0 ? msg_to_string (&parts_[0]) : std::string ();
    ++probe->callback_count;
    probe->cv.notify_all ();
}

void send_captured_reply (void *router_,
                          request_handler_probe_t *handler_probe_,
                          const char *reply_payload_)
{
    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, reply_payload_);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_reply (router_, &handler_probe_->peer_rid_value,
                                                   handler_probe_->request_seq, &reply_part, 1));
}

void ignore_reentrant_reply (zlink_request_result_t, zlink_msg_t *, size_t, void *)
{
}

zlink_submit_result_t attempt_reentrant_request (void *dealer_, int *errno_out_)
{
    zlink_msg_t part;
    zlink_msg_init (&part);
    init_string_part (&part, "reentrant-request");
    errno = 0;
    const zlink_submit_result_t result = zlink_dealer_request_part (
      dealer_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 1000,
      &ignore_reentrant_reply, NULL);
    *errno_out_ = errno;
    if (result != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return result;
}

zlink_submit_result_t attempt_reentrant_async (void *dealer_, int *errno_out_)
{
    zlink_msg_t part;
    zlink_msg_init (&part);
    init_string_part (&part, "reentrant-async");
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    errno = 0;
    const zlink_submit_result_t result = zlink_send_async (
      dealer_, &part, 1, &options, &op_id);
    *errno_out_ = errno;
    if (result != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return result;
}

void capture_reentrant_reply (zlink_request_result_t,
                              zlink_msg_t *,
                              size_t,
                              void *userdata_)
{
    reply_reentry_probe_t *probe =
      static_cast<reply_reentry_probe_t *> (userdata_);
    probe->same_request = attempt_reentrant_request (
      probe->dealer, &probe->same_request_errno);
    probe->different_request = attempt_reentrant_request (
      probe->different_dealer, &probe->different_request_errno);
    probe->same_async = attempt_reentrant_async (
      probe->dealer, &probe->same_async_errno);
    probe->different_async = attempt_reentrant_async (
      probe->different_dealer, &probe->different_async_errno);
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
    }
    probe->cv.notify_all ();
}

void test_reply_callback_rejects_sync_and_async_submit_on_all_sockets ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *different_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "reply-reentry-dealer", 20));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://reply-completion-reentry"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://reply-completion-reentry"));
    msleep (SETTLE_TIME);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK, zlink_send_complete_handler (dealer, &ignore_routed_ready, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (different_dealer, &ignore_routed_ready, NULL));

    reply_reentry_probe_t probe;
    probe.dealer = dealer;
    probe.different_dealer = different_dealer;
    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "reply-reentry-request");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &request, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_FINAL, 3000,
                                 &capture_reentrant_reply, &probe));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router, &handler_probe);
    send_captured_reply (router, &handler_probe, "reply-reentry-reply");

    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        TEST_ASSERT_TRUE (probe.cv.wait_for (
          lock, std::chrono::seconds (3), [&probe] { return probe.done; }));
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.same_request);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.same_request_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.different_request);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.different_request_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.same_async);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.same_async_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.different_async);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.different_async_errno);

    test_context_socket_close_zero_linger (different_dealer);
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void send_router_reply_to_event (void *router_,
                                 const request_event_t &event_,
                                 const char *reply_payload_)
{
    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, reply_payload_);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_reply (router_, &event_.peer_rid_value, event_.request_seq, &reply_part, 1));
}


void test_dealer_to_router_request_reply_basic ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-A", 8));

    request_handler_probe_t handler_probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://zmp-dealer-router-request-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://zmp-dealer-router-request-reply"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000));

    recv_router_request_into_probe (router, &handler_probe);
    msleep (SETTLE_TIME);
    send_captured_reply (router, &handler_probe, "router-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    {
        std::lock_guard<std::mutex> lock (handler_probe.mutex);
        TEST_ASSERT_TRUE (handler_probe.invoked);
        TEST_ASSERT_TRUE (handler_probe.request_seq != 0);
        TEST_ASSERT_EQUAL_STRING_LEN ("dealer-A", handler_probe.peer_rid.c_str (),
                                      handler_probe.peer_rid.size ());
        TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request", handler_probe.request_payload.c_str (),
                                      handler_probe.request_payload.size ());
    }

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_receives_unsolicited_message_after_request_reply ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-unsolicited", 18));

    request_handler_probe_t handler_probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-dealer-unsolicited-after-request"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-dealer-unsolicited-after-request"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "request");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000));

    recv_router_request_into_probe (router, &handler_probe);
    send_captured_reply (router, &handler_probe, "reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    msleep (SETTLE_TIME);

    zlink_msg_t unsolicited_part;
    zlink_msg_init (&unsolicited_part);
    init_string_part (&unsolicited_part, "unsolicited");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &handler_probe.peer_rid_value, &unsolicited_part,
                           ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received_part;
    zlink_msg_init (&received_part);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (dealer, &message_type, &request_seq, &received_part,
                                   &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, message_type);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("unsolicited", part_to_string_and_close (&received_part).c_str ());

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_concurrent_first_dealer_requests_share_dispatch_install ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-concurrent", 17));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-dealer-concurrent-first-install"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-dealer-concurrent-first-install"));
    msleep (SETTLE_TIME);

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = dealer;
    reply_b.progress_handle = dealer;
    zlink_submit_result_t submit_a = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_submit_result_t submit_b = ZLINK_SUBMIT_INTERNAL_ERROR;
    int errno_a = 0;
    int errno_b = 0;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    int ready_count = 0;
    bool start = false;

    auto submit = [&] (const char *payload_, reply_probe_t *probe_,
                       zlink_submit_result_t *result_out_, int *errno_out_) {
        zlink_msg_t request;
        zlink_msg_init (&request);
        init_string_part (&request, payload_);
        {
            std::unique_lock<std::mutex> lock (gate_mutex);
            ++ready_count;
            gate_cv.notify_all ();
            gate_cv.wait (lock, [&] () { return start; });
        }
        *result_out_ =
          zlink_dealer_request (dealer, &request, 1, &capture_reply, probe_, 0, 3000);
        *errno_out_ = zlink_errno ();
    };

    std::thread thread_a (submit, "request-A", &reply_a, &submit_a, &errno_a);
    std::thread thread_b (submit, "request-B", &reply_b, &submit_b, &errno_b);
    {
        std::unique_lock<std::mutex> lock (gate_mutex);
        gate_cv.wait (lock, [&] () { return ready_count == 2; });
        start = true;
    }
    gate_cv.notify_all ();
    thread_a.join ();
    thread_b.join ();

    if (submit_a != ZLINK_SUBMIT_OK)
        errno = errno_a;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_a);
    if (submit_b != ZLINK_SUBMIT_OK)
        errno = errno_b;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_b);

    multi_request_probe_t requests;
    recv_router_request_into_event (router, &requests);
    recv_router_request_into_event (router, &requests);
    request_event_t first;
    request_event_t second;
    {
        std::lock_guard<std::mutex> lock (requests.mutex);
        first = requests.events[0];
        second = requests.events[1];
    }
    send_router_reply_to_event (router, first, "reply-first");
    send_router_reply_to_event (router, second, "reply-second");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void run_blocking_dealer_receive_ownership_transition (bool use_dealer_receive_)
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, "dealer-recv-transition", 22));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-dealer-recv-ownership-transition"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-dealer-recv-ownership-transition"));
    msleep (SETTLE_TIME);

    const zlink_routing_id_t dealer_rid = get_routing_id_value (dealer);
    std::atomic<bool> recv_started (false);
    zlink_recv_result_t recv_result = ZLINK_RECV_INTERNAL_ERROR;
    uint8_t received_message_type = 0xff;
    uint64_t received_request_seq = std::numeric_limits<uint64_t>::max ();
    zlink_part_flag_t received_has_more = ZLINK_PART_MORE;
    bool generic_source_was_null = false;
    std::string received_payload;

    std::thread recv_thread ([&] () {
        zlink_msg_t received;
        zlink_msg_init (&received);
        recv_started.store (true, std::memory_order_release);
        if (use_dealer_receive_) {
            recv_result = zlink_dealer_recv_part (
              dealer, &received_message_type, &received_request_seq, &received,
              &received_has_more, static_cast<zlink_recv_flags_t> (0));
        } else {
            const zlink_routing_id_t *source_rid = NULL;
            recv_result = zlink_recv_part (dealer, &source_rid, &received, &received_has_more,
                                           static_cast<zlink_recv_flags_t> (0));
            generic_source_was_null = source_rid == NULL;
        }
        if (recv_result == ZLINK_RECV_OK) {
            received_payload.assign (
              static_cast<const char *> (zlink_msg_data (&received)),
              zlink_msg_size (&received));
        }
        (void) zlink_msg_close (&received);
    });

    while (!recv_started.load (std::memory_order_acquire))
        std::this_thread::yield ();

    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> state;
    bool direct_receive_ready = false;
    const auto receive_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < receive_deadline) {
        state = zlink::socket_reqrep_internal::find_request_reply_state (handle);
        if (state)
            direct_receive_ready = true;
        if (direct_receive_ready)
            break;
        msleep (1);
    }
    handle = socket_handle_t ();

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "request-during-blocking-recv");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe, 0, 3000));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router, &handler_probe);

    zlink_msg_t unsolicited;
    zlink_msg_init (&unsolicited);
    init_string_part (&unsolicited, "unsolicited-during-transition");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &unsolicited, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));

    recv_thread.join ();
    send_captured_reply (router, &handler_probe, "reply-after-transition");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    TEST_ASSERT_TRUE_MESSAGE (
      direct_receive_ready,
      "blocking DEALER receive created an internal payload queue");
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, recv_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_has_more);
    TEST_ASSERT_EQUAL_STRING ("unsolicited-during-transition", received_payload.c_str ());
    if (use_dealer_receive_) {
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, received_message_type);
        TEST_ASSERT_EQUAL_UINT64 (0, received_request_seq);
    } else {
        TEST_ASSERT_TRUE (generic_source_was_null);
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_blocking_generic_dealer_recv_remains_on_transport_pipe ()
{
    run_blocking_dealer_receive_ownership_transition (false);
}

void test_blocking_typed_dealer_recv_remains_on_transport_pipe ()
{
    run_blocking_dealer_receive_ownership_transition (true);
}

void run_dealer_no_input_receive_timeout (bool use_dealer_receive_)
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    const int timeout_ms = 50;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));

    zlink_msg_t received;
    zlink_msg_init (&received);
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const auto start = std::chrono::steady_clock::now ();
    zlink_recv_result_t recv_result = ZLINK_RECV_INTERNAL_ERROR;
    if (use_dealer_receive_) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        recv_result =
          zlink_dealer_recv_part (dealer, &message_type, &request_seq, &received, &has_more,
                                  static_cast<zlink_recv_flags_t> (0));
    } else {
        const zlink_routing_id_t *source_rid = NULL;
        recv_result = zlink_recv_part (dealer, &source_rid, &received, &has_more,
                                       static_cast<zlink_recv_flags_t> (0));
        TEST_ASSERT_NULL (source_rid);
    }
    const long elapsed_ms =
      static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - start)
                           .count ());

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_result);
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms >= 20,
                              "configured DEALER receive timeout returned too early");
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 1000,
                              "configured DEALER receive timeout was ignored");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    test_context_socket_close_zero_linger (dealer);
}

void test_generic_dealer_recv_honors_configured_no_input_timeout ()
{
    run_dealer_no_input_receive_timeout (false);
}

void test_typed_dealer_recv_honors_configured_no_input_timeout ()
{
    run_dealer_no_input_receive_timeout (true);
}

void test_direct_dealer_generic_recv_and_poller_preserve_raw_order ()
{
    const char *endpoint = "inproc://zmp-dealer-generic-dispatch-queue";
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-generic", 14));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    // Establish the ROUTER route, then leave a multipart raw message in the
    // Application transport pipe. Starting a request must not move it into an
    // internal payload queue or change FIFO order.
    zlink_msg_t identify;
    zlink_msg_init (&identify);
    init_string_part (&identify, "identify");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &identify, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));

    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t raw_seq = 1;
    zlink_msg_t *identify_parts = NULL;
    size_t identify_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &peer_rid, &raw_seq, &identify_parts, &identify_part_count, 0));
    TEST_ASSERT_NOT_NULL (peer_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, raw_seq);
    zlink_routing_id_t dealer_rid = *peer_rid;
    zlink_multipart_close (identify_parts, identify_part_count);

    zlink_msg_t queued_first;
    zlink_msg_t queued_last;
    zlink_msg_init (&queued_first);
    zlink_msg_init (&queued_last);
    init_string_part (&queued_first, "queued-first");
    init_string_part (&queued_last, "queued-last");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &queued_first, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &queued_last, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
    msleep (SETTLE_TIME);

    zlink_pollitem_t item = {
      dealer, 0, static_cast<short> (ZLINK_POLLIN | ZLINK_POLLOUT), 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, 3000, NULL));
    TEST_ASSERT_TRUE ((item.revents & ZLINK_POLLIN) != 0);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    void *const marker = reinterpret_cast<void *> (0x1234);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, marker,
                        static_cast<short> (ZLINK_POLLIN | ZLINK_POLLOUT)));

    zlink_poller_event_t events[2];
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, events, 2, 3000, NULL));
    TEST_ASSERT_EQUAL_PTR (dealer, events[0].socket);
    TEST_ASSERT_EQUAL_PTR (marker, events[0].user_data);
    TEST_ASSERT_TRUE ((events[0].events & ZLINK_POLLIN) != 0);

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "request");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe, 0, 3000));

    zlink_msg_t received;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, recv_generic_part_with_retry (dealer, &received, &has_more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_STRING ("queued-first", part_to_string_and_close (&received).c_str ());
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, recv_generic_part_with_retry (dealer, &received, &has_more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("queued-last", part_to_string_and_close (&received).c_str ());

    zlink_msg_init (&received);
    const zlink_routing_id_t *source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (dealer, &source_rid, &received, &has_more,
                       static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router, &handler_probe);
    zlink_msg_t ordered_one;
    zlink_msg_t ordered_two;
    zlink_msg_init (&ordered_one);
    zlink_msg_init (&ordered_two);
    init_string_part (&ordered_one, "ordered-one");
    init_string_part (&ordered_two, "ordered-two");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &ordered_one, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &ordered_two, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
    send_captured_reply (router, &handler_probe, "reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, recv_generic_part_with_retry (dealer, &received, &has_more));
    TEST_ASSERT_EQUAL_STRING ("ordered-one", part_to_string_and_close (&received).c_str ());
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, recv_generic_part_with_retry (dealer, &received, &has_more));
    TEST_ASSERT_EQUAL_STRING ("ordered-two", part_to_string_and_close (&received).c_str ());

    // Unread records belong to the transport generation. Disconnecting the
    // pair drops that generation instead of preserving payload in a hidden
    // application queue.
    zlink_msg_t before_disconnect;
    zlink_msg_init (&before_disconnect);
    init_string_part (&before_disconnect, "before-disconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &before_disconnect, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
    zlink_msg_t queued_at_close;
    zlink_msg_init (&queued_at_close);
    init_string_part (&queued_at_close, "queued-at-close");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &queued_at_close, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, events, 2, 3000, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect (dealer, endpoint));
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (dealer, &source_rid, &received, &has_more,
                       static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Keeps the compiler from constant-folding the intentionally impossible
//  allocation size into the memcpy bound (-Wstringop-overflow false positive).
size_t oversized_prefix_size ()
{
    volatile size_t size = std::numeric_limits<size_t>::max ();
    return size;
}

void test_prefixed_multipart_second_prefix_allocation_failure_rolls_back ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (sender);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://zmp-prefixed-allocation-rollback"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://zmp-prefixed-allocation-rollback"));
    msleep (SETTLE_TIME);

    const unsigned char first_prefix = 0x11;
    const unsigned char second_prefix = 0x22;
    zlink_msg_t failed_payload;
    zlink_msg_init (&failed_payload);
    init_string_part (&failed_payload, "failed-payload");
    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_EQUAL_INT (
      -1,
      zlink::logical_multipart_send_prefixed_frames (
        sender_handle.socket, &first_prefix, sizeof (first_prefix), 0, &second_prefix,
        oversized_prefix_size (), 0, &failed_payload, 1, 0));
    sender_handle = socket_handle_t ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed_payload));

    zlink_msg_t after_failure;
    zlink_msg_init (&after_failure);
    init_string_part (&after_failure, "after-failure");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &after_failure, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL));

    zlink_msg_t received;
    zlink_msg_init (&received);
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_generic_part_with_retry (receiver, &received, &has_more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("after-failure", part_to_string_and_close (&received).c_str ());

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_dealer_to_router_request_reply_over_tcp_with_explicit_routing_id ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-tcp", 10));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 50);

    request_handler_probe_t handler_probe;

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request-tcp");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_dealer_request (dealer, &request_part, 1, &capture_reply,
                                                     &reply_probe, ZLINK_SEND_FLAGS_NONE, 5000));

    recv_router_request_into_probe (router, &handler_probe);
    msleep (SETTLE_TIME);
    send_captured_reply (router, &handler_probe, "router-reply-tcp");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    {
        std::lock_guard<std::mutex> lock (handler_probe.mutex);
        TEST_ASSERT_TRUE (handler_probe.invoked);
        TEST_ASSERT_TRUE (handler_probe.request_seq != 0);
        TEST_ASSERT_EQUAL_STRING_LEN ("dealer-tcp", handler_probe.peer_rid.c_str (),
                                      handler_probe.peer_rid.size ());
        TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request-tcp", handler_probe.request_payload.c_str (),
                                      handler_probe.request_payload.size ());
    }

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply-tcp", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Regression for the completion-lane credit recovery defect: replies bypass
//  send()/recv(), so the reply submit entry itself must drain pending socket
//  commands. Before the fix the router kept ZLINK_SUBMIT_BACKPRESSURED forever
//  because the completion pipe's activate-write command was never processed.
namespace completion_backpressure
{
const size_t payload_bytes = 8192;
const size_t batch_size = 8;
const size_t cycle_count = 6;

struct probe_t
{
    std::mutex mutex;
    size_t completed;
    bool payload_mismatch;
    bool result_failure;

    probe_t () :
        completed (0), payload_mismatch (false), result_failure (false)
    {
    }
};

unsigned char payload_byte_at (uint32_t ordinal_, size_t index_)
{
    return static_cast<unsigned char> ((ordinal_ * 31u + index_) & 0xFFu);
}

void fill_payload (zlink_msg_t *part_, uint32_t ordinal_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, payload_bytes));
    unsigned char *data = static_cast<unsigned char *> (zlink_msg_data (part_));
    memcpy (data, &ordinal_, sizeof (ordinal_));
    for (size_t i = sizeof (ordinal_); i < payload_bytes; ++i)
        data[i] = payload_byte_at (ordinal_, i);
}

void capture_completion (zlink_request_result_t result_,
                         zlink_msg_t *parts_,
                         size_t part_count_,
                         void *userdata_)
{
    probe_t *probe = static_cast<probe_t *> (userdata_);
    std::lock_guard<std::mutex> lock (probe->mutex);
    ++probe->completed;
    if (result_ != ZLINK_REQUEST_OK || part_count_ != 1
        || zlink_msg_size (&parts_[0]) != payload_bytes) {
        probe->result_failure = true;
        return;
    }
    const unsigned char *data =
      static_cast<const unsigned char *> (zlink_msg_data (&parts_[0]));
    uint32_t ordinal = 0;
    memcpy (&ordinal, data, sizeof (ordinal));
    for (size_t i = sizeof (ordinal); i < payload_bytes; ++i)
        if (data[i] != payload_byte_at (ordinal, i)) {
            probe->payload_mismatch = true;
            return;
        }
}

uint32_t ordinal_of_request (size_t cycle_, size_t index_)
{
    return static_cast<uint32_t> (cycle_ * batch_size + index_);
}

bool deadline_passed (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}
}

void test_router_reply_completion_backpressure_recovers_over_tcp ()
{
    using namespace completion_backpressure;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    //  A small reply-lane byte HWM makes a burst of replies cross HWM and LWM
    //  on every cycle. Requests keep the default capacity.
    const uint64_t reply_lane_hwm = 4 * payload_bytes;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_SNDHWM, &reply_lane_hwm,
                                                 sizeof (reply_lane_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &reply_lane_hwm,
                                                 sizeof (reply_lane_hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "bp-dealer", 9));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 10);

    //  1. The completion poller is registered before the first request.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, router, NULL, ZLINK_POLLOUT));

    probe_t probe;
    size_t backpressure_hits = 0;
    uint32_t ordinal = 0;
    const std::chrono::steady_clock::time_point test_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (30);

    for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        for (size_t i = 0; i < batch_size; ++i) {
            zlink_msg_t request_part;
            fill_payload (&request_part, ordinal++);
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_dealer_request (dealer, &request_part, 1, &capture_completion, &probe,
                                    ZLINK_SEND_FLAGS_NONE, 30000));
        }

        for (size_t i = 0; i < batch_size; ++i) {
            const zlink_routing_id_t *peer_rid = NULL;
            uint64_t request_seq = 0;
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router, &peer_rid, &request_seq,
                                                          &parts, &part_count, 0));
            TEST_ASSERT_EQUAL_UINT64 (1, part_count);

            //  2. The reply reuses the payload message received on the
            //     application connection. 7. That message still carries the
            //     application connection ID, so this also pins the completion
            //     connection ID rewrite: without it the reply is discarded as
            //     stale and the completion below never arrives.
            zlink_routing_id_t reply_rid = *peer_rid;
            zlink_submit_result_t rc = zlink_router_reply_part (
              router, &reply_rid, request_seq, &parts[0], ZLINK_PART_FINAL);
            zlink_multipart_close (parts, part_count);

            //  The submit entry consumed the message either way, so a retry
            //  rebuilds an equivalent payload instead of reusing it.
            while (rc != ZLINK_SUBMIT_OK) {
                //  6. Only backpressure is acceptable here.
                TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, rc);
                ++backpressure_hits;
                //  5. Consuming completions on the requester returns credit to
                //     the router's completion pipe.
                zlink_poller_event_t event;
                (void) zlink_poller_wait (poller, &event, 1, 10, NULL);
                TEST_ASSERT_FALSE_MESSAGE (
                  deadline_passed (test_deadline),
                  "router reply never recovered from completion backpressure");

                //  The readiness hint that used to gate this retry is gone.
                //  Draining completions above is what actually returns credit,
                //  so the retry is driven by that drain instead.
                zlink_msg_t retry_part;
                fill_payload (&retry_part, ordinal_of_request (cycle, i));
                rc = zlink_router_reply_part (router, &reply_rid, request_seq, &retry_part,
                                              ZLINK_PART_FINAL);
            }
        }

        //  3./4. Drain this cycle's completions before the next burst so the
        //  completion lane crosses HWM and LWM again on every cycle.
        const size_t expected = (cycle + 1) * batch_size;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                if (probe.completed >= expected)
                    break;
            }
            zlink_poller_event_t event;
            (void) zlink_poller_wait (poller, &event, 1, 10, NULL);
            TEST_ASSERT_FALSE_MESSAGE (deadline_passed (test_deadline),
                                       "completions stalled while draining a cycle");
        }
    }

    {
        std::lock_guard<std::mutex> lock (probe.mutex);
        TEST_ASSERT_EQUAL_UINT64 (cycle_count * batch_size, probe.completed);
        TEST_ASSERT_FALSE_MESSAGE (probe.result_failure,
                                   "a completion reported a non-OK result or wrong shape");
        TEST_ASSERT_FALSE_MESSAGE (probe.payload_mismatch,
                                   "a completion payload did not match its request");
    }

    (void) zlink_poller_remove (poller, router);
    (void) zlink_poller_remove (poller, dealer);
    (void) zlink_poller_destroy (&poller);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_poller_combines_input_and_completion_ownership ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (
        poller, router, NULL,
        static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION)));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (router);
}

void test_socket_poller_wakes_after_async_owner_applies_input ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (router, "poller-router");
    set_routing_id_text (dealer, "poller-dealer");

    // Routed readiness installs the async mailbox command owner. POLLIN must
    // still be delivered by a public poller after that owner consumes the
    // activate_read command and makes the application pipe readable.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &ignore_routed_ready, NULL));
    const char endpoint[] = "inproc://async-owner-poller-input";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    const std::chrono::steady_clock::time_point target_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (zlink_select_routed_submit_target (dealer, NULL, &target)
           != ZLINK_SUBMIT_OK) {
        TEST_ASSERT_TRUE_MESSAGE (
          std::chrono::steady_clock::now () < target_deadline,
          "DEALER exact target did not become selectable");
        msleep (1);
    }

    void *poller = zlink_poller_new ();
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_NOT_NULL (timer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, router, router, ZLINK_POLLIN));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add_timer (poller, timer, timer));
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &event, 1, 0, NULL));

    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    uint64_t epoch_before = 0;
    uint64_t public_drains_before = 0;
    uint64_t async_drains_before = 0;
    router_handle.socket->test_receive_owner_snapshot (
      &epoch_before, &public_drains_before, &async_drains_before);

    std::atomic<bool> poll_started (false);
    int poll_result = -1;
    long poll_elapsed_ms = -1;
    std::thread poll_thread ([&] {
        poll_started.store (true, std::memory_order_release);
        const std::chrono::steady_clock::time_point started =
          std::chrono::steady_clock::now ();
        poll_result = zlink_poller_wait (poller, &event, 1, 3000, NULL);
        poll_elapsed_ms = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - started)
            .count ());
    });
    while (!poll_started.load (std::memory_order_acquire))
        std::this_thread::yield ();
    msleep (10);

    zlink_msg_t payload;
    init_string_part (&payload, "async-owner-poller-payload");
    const zlink_submit_result_t send_result =
      zlink_dealer_send_transport_pair_part (
        dealer, &target, &payload, ZLINK_SEND_FLAGS_DONTWAIT,
        ZLINK_PART_FINAL);
    poll_thread.join ();

    uint64_t epoch_after = 0;
    uint64_t public_drains_after = 0;
    uint64_t async_drains_after = 0;
    router_handle.socket->test_receive_owner_snapshot (
      &epoch_after, &public_drains_after, &async_drains_after);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, send_result);
    TEST_ASSERT_TRUE_MESSAGE (
      async_drains_after > async_drains_before,
      "async mailbox owner did not process the input activation");
    TEST_ASSERT_EQUAL_UINT64 (public_drains_before, public_drains_after);
    TEST_ASSERT_TRUE_MESSAGE (
      epoch_after > epoch_before,
      "async mailbox owner did not publish receive progress");
    TEST_ASSERT_EQUAL_INT (1, poll_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, event.source_kind);
    TEST_ASSERT_EQUAL_PTR (router, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);
    TEST_ASSERT_TRUE_MESSAGE (
      poll_elapsed_ms >= 0 && poll_elapsed_ms < 1000,
      "socket poller slept until its timeout after async input progress");

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "async-owner-poller-payload",
      static_cast<const char *> (zlink_msg_data (&parts[0])),
      zlink_msg_size (&parts[0]));
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove_timer (poller, timer));
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &event, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, router));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, router, router, ZLINK_POLLIN));
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &event, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_timer_destroy (&timer));
    router_handle = socket_handle_t ();
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_completion_poller_exclusively_owns_routed_async_completion ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (dealer, &ignore_routed_ready, NULL));
    const uint64_t reply_lane_hwm = 1024u * 1024u;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer, ZLINK_OPT_SNDHWM, &reply_lane_hwm, sizeof (reply_lane_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_RCVHWM, &reply_lane_hwm, sizeof (reply_lane_hwm)));

    const char endpoint[] = "inproc://completion-owner-handoff";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    completion_owner_probe_t probe;
    for (size_t cycle = 0; cycle < 4; ++cycle) {
        // Adding a public completion poller fences the already-running routed
        // mailbox completion owner before add returns.
        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

        probe.reset ();
        zlink_msg_t request;
        init_string_part (&request, "poller-owned-request");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_request (dealer, &request, 1,
                                &capture_completion_owner_reply, &probe,
                                ZLINK_SEND_FLAGS_NONE, 3000));
        request_handler_probe_t request_probe;
        recv_router_request_into_probe (router, &request_probe);
        send_captured_reply (router, &request_probe, "poller-owned-reply");

        const std::thread::id poller_thread = std::this_thread::get_id ();
        const std::chrono::steady_clock::time_point poller_deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::milliseconds (SETTLE_TIME * 10);
        while (true) {
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                if (probe.done)
                    break;
            }
            zlink_poller_event_t event;
            TEST_ASSERT_TRUE (
              zlink_poller_wait (poller, &event, 1, 25, NULL) >= 0);
            TEST_ASSERT_TRUE_MESSAGE (
              std::chrono::steady_clock::now () < poller_deadline,
              "public completion poller did not drain the reply");
        }

        zlink_request_result_t poller_result;
        size_t poller_callbacks;
        std::string poller_payload;
        std::thread::id observed_poller_thread;
        {
            std::lock_guard<std::mutex> lock (probe.mutex);
            poller_result = probe.result;
            poller_callbacks = probe.callback_count;
            poller_payload = probe.payload;
            observed_poller_thread = probe.callback_thread;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, poller_result);
        TEST_ASSERT_EQUAL_UINT64 (1, poller_callbacks);
        TEST_ASSERT_EQUAL_STRING ("poller-owned-reply", poller_payload.c_str ());
        TEST_ASSERT_TRUE (observed_poller_thread == poller_thread);

        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_remove (poller, dealer));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));

        // Removing the last completion poller hands ownership back to the
        // still-attached routed mailbox. Its wake must survive even when the
        // earlier coalesced notification was already consumed.
        probe.reset ();
        zlink_msg_t async_request;
        init_string_part (&async_request, "async-owned-request");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_request (dealer, &async_request, 1,
                                &capture_completion_owner_reply, &probe,
                                ZLINK_SEND_FLAGS_NONE, 3000));
        request_handler_probe_t async_request_probe;
        recv_router_request_into_probe (router, &async_request_probe);
        send_captured_reply (router, &async_request_probe, "async-owned-reply");

        {
            std::unique_lock<std::mutex> lock (probe.mutex);
            TEST_ASSERT_TRUE_MESSAGE (
              probe.cv.wait_for (lock,
                                 std::chrono::milliseconds (SETTLE_TIME * 10),
                                 [&probe] { return probe.done; }),
              "routed mailbox did not resume completion ownership");
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe.result);
            TEST_ASSERT_EQUAL_UINT64 (1, probe.callback_count);
            TEST_ASSERT_EQUAL_STRING ("async-owned-reply",
                                      probe.payload.c_str ());
        }
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_completion_poller_quiesces_callbackless_async_owner ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const char endpoint[] = "inproc://completion-owner-quiesce";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    completion_owner_probe_t probe;
    zlink_msg_t request;
    init_string_part (&request, "quiesce-owner-request");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1,
                            &capture_completion_owner_reply, &probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));

    // Request submission starts the callbackless async mailbox owner. Adding
    // the first public completion poller must quiesce it before returning.
    request_handler_probe_t request_probe;
    recv_router_request_into_probe (router, &request_probe);
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));
    send_captured_reply (router, &request_probe, "quiesce-owner-reply");

    const std::thread::id poller_thread = std::this_thread::get_id ();
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (SETTLE_TIME * 10);
    while (true) {
        {
            std::lock_guard<std::mutex> lock (probe.mutex);
            if (probe.done)
                break;
        }
        zlink_poller_event_t event;
        TEST_ASSERT_TRUE (zlink_poller_wait (poller, &event, 1, 25, NULL) >= 0);
        TEST_ASSERT_TRUE_MESSAGE (
          std::chrono::steady_clock::now () < deadline,
          "public completion poller did not drain the quiesced-owner reply");
    }

    {
        std::lock_guard<std::mutex> lock (probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, probe.callback_count);
        TEST_ASSERT_EQUAL_STRING ("quiesce-owner-reply", probe.payload.c_str ());
        TEST_ASSERT_TRUE (probe.callback_thread == poller_thread);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_last_completion_poller_release_resumes_pending_request ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const char endpoint[] = "inproc://completion-owner-last-release";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    // Register the sole public completion owner before creating the request,
    // so request submission intentionally does not start an async owner.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

    completion_owner_probe_t probe;
    zlink_msg_t request;
    init_string_part (&request, "last-owner-request");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1,
                            &capture_completion_owner_reply, &probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));
    request_handler_probe_t request_probe;
    recv_router_request_into_probe (router, &request_probe);
    send_captured_reply (router, &request_probe, "last-owner-reply");

    // No poller wait is performed. The reply callback must remain pending
    // until the exact 1 -> 0 owner transition resumes async processing.
    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        TEST_ASSERT_FALSE (
          probe.cv.wait_for (lock, std::chrono::milliseconds (50),
                             [&probe] { return probe.done; }));
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));

    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        TEST_ASSERT_TRUE_MESSAGE (
          probe.cv.wait_for (lock, std::chrono::seconds (3),
                             [&probe] { return probe.done; }),
          "last POLLCOMPLETION release did not resume a pending request");
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, probe.callback_count);
        TEST_ASSERT_EQUAL_STRING ("last-owner-reply", probe.payload.c_str ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_application_only_poller_does_not_take_completion_ownership ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const char endpoint[] = "inproc://application-only-poller";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    // POLLIN observes only application messages. Completion callbacks must
    // remain owned by the socket's default async completion executor.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, dealer, NULL, ZLINK_POLLIN));

    reply_probe_t reply_probe;
    zlink_msg_t request;
    init_string_part (&request, "application-only-poller-request");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router, &handler_probe);
    send_captured_reply (
      router, &handler_probe, "application-only-poller-reply");

    // Do not wait on the poller. The callback must still complete.
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_reply (&reply_probe),
      "application-only poller registration stalled async completion");
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
    TEST_ASSERT_EQUAL_STRING (
      "application-only-poller-reply", reply_probe.payload.c_str ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

namespace disconnect_pair
{
size_t drain_connected_events (void *monitor_)
{
    size_t connected = 0;
    for (;;) {
        zlink_monitor_event_t event;
        if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0)
            break;
        if (event.event == ZLINK_EVENT_CONNECTED)
            ++connected;
    }
    return connected;
}
}

//  Disconnecting a paired endpoint has to end both lanes. While the Completion
//  lane was registered under its own endpoint key, disconnect terminated only
//  the Application lane and the surviving Completion session treated that as a
//  transport failure and redialled the removed endpoint every reconnect
//  interval.
void test_disconnect_of_paired_endpoint_stops_reconnecting ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const int reconnect_ivl = 50;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_RECONNECT_IVL,
                                                 &reconnect_ivl, sizeof (reconnect_ivl)));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTED;
    void *dealer_monitor = zlink_socket_monitor_open (dealer, &monitor_opts);
    TEST_ASSERT_NOT_NULL (dealer_monitor);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 20);

    //  Both lanes have connected by now; those events are expected.
    TEST_ASSERT_TRUE (disconnect_pair::drain_connected_events (dealer_monitor) > 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (dealer, endpoint));

    //  The router keeps listening, so a lane that survived the disconnect
    //  reconnects within a few reconnect intervals and reports it.
    msleep (static_cast<int> (reconnect_ivl) * 12);
    TEST_ASSERT_EQUAL_UINT64 (0, disconnect_pair::drain_connected_events (dealer_monitor));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&dealer_monitor));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_disconnect_fails_only_requests_on_that_pipe ()
{
    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router_a);
    TEST_ASSERT_NOT_NULL (router_b);
    TEST_ASSERT_NOT_NULL (dealer);

    const char *endpoint_a = "inproc://zmp-dealer-selective-disconnect-a";
    const char *endpoint_b = "inproc://zmp-dealer-selective-disconnect-b";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router_a, endpoint_a));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router_b, endpoint_b));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_a));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_b));
    msleep (SETTLE_TIME);

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = dealer;
    reply_b.progress_handle = dealer;

    zlink_msg_t request_a;
    zlink_msg_t request_b;
    zlink_msg_init (&request_a);
    zlink_msg_init (&request_b);
    init_string_part (&request_a, "request-a");
    init_string_part (&request_b, "request-b");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_a, 1, &capture_reply, &reply_a, 0, 5000));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_b, 1, &capture_reply, &reply_b, 0, 5000));

    request_handler_probe_t received_a;
    request_handler_probe_t received_b;
    recv_router_request_into_probe (router_a, &received_a);
    recv_router_request_into_probe (router_b, &received_b);

    reply_probe_t *failed_reply =
      received_a.request_payload == "request-a" ? &reply_a : &reply_b;
    reply_probe_t *successful_reply = failed_reply == &reply_a ? &reply_b : &reply_a;

    test_context_socket_close_zero_linger (router_a);
    router_a = NULL;
    send_captured_reply (router_b, &received_b, "reply-from-b");

    TEST_ASSERT_TRUE (wait_for_reply (failed_reply));
    TEST_ASSERT_TRUE (wait_for_reply (successful_reply));
    {
        std::lock_guard<std::mutex> lock (failed_reply->mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED, failed_reply->result);
        TEST_ASSERT_EQUAL_UINT64 (0, failed_reply->part_count);
    }
    {
        std::lock_guard<std::mutex> lock (successful_reply->mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, successful_reply->result);
        TEST_ASSERT_EQUAL_STRING ("reply-from-b", successful_reply->payload.c_str ());
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router_b);
    if (router_a)
        test_context_socket_close_zero_linger (router_a);
}

void test_router_completion_correlation_accepts_settled_peer_and_fences_pair ()
{
    using namespace zlink::socket_reqrep_internal;
    socket_request_reply_state_t state (NULL, ZLINK_CORE_SOCKET_ROUTER);

    pending_key_t expected_key;
    expected_key.peer_rid = "peer-a";
    expected_key.request_seq = 77;
    pending_request_t expected;
    expected.key = expected_key;
    expected.transport_pair_id = 101;
    expected.transport_pair_generation = 9;
    expected.handler = NULL;
    expected.userdata = NULL;
    add_socket_pending_request_locked (&state, expected_key, expected);

    pending_request_t taken;
    pending_key_t wrong_peer_key = expected_key;
    wrong_peer_key.peer_rid = "peer-b";
    //  The peer may settle on a routing ID different from the requested
    //  intent. The per-socket sequence identifies the pending request while
    //  the transport pair still fences stale connections.
    TEST_ASSERT_TRUE (take_pending_reply_from_transport_locked (
      &state, wrong_peer_key, 101, 9, &taken));
    TEST_ASSERT_EQUAL_STRING ("peer-a", taken.key.peer_rid.c_str ());
    TEST_ASSERT_TRUE (state.pending_requests.empty ());

    add_socket_pending_request_locked (&state, expected_key, expected);
    TEST_ASSERT_FALSE (take_pending_reply_from_transport_locked (
      &state, expected_key, 202, 9, &taken));
    TEST_ASSERT_FALSE (take_pending_reply_from_transport_locked (
      &state, expected_key, 101, 10, &taken));
    TEST_ASSERT_TRUE (take_pending_reply_from_transport_locked (
      &state, expected_key, 101, 9, &taken));
    TEST_ASSERT_EQUAL_UINT64 (77, taken.key.request_seq);
    TEST_ASSERT_TRUE (state.pending_requests.empty ());
}

void test_router_to_router_request_reply_basic ()
{
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);

    const char server_rid[] = "router-srv";
    const char client_rid[] = "router-cli";

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server_router, server_rid, strlen (server_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client_router, client_rid, strlen (client_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client_router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_rid, strlen (server_rid)));

    request_handler_probe_t handler_probe;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_router, "inproc://zmp-router-router-request-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_router, "inproc://zmp-router-router-request-reply"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "router-request");

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, server_rid, strlen (server_rid));
    peer_rid.size = strlen (server_rid);

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_part, 1,
                                                     &capture_reply, &reply_probe, 0, 5000));

    recv_router_request_into_probe (server_router, &handler_probe);
    msleep (SETTLE_TIME);
    send_captured_reply (server_router, &handler_probe, "router-router-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    {
        std::lock_guard<std::mutex> lock (handler_probe.mutex);
        TEST_ASSERT_TRUE (handler_probe.invoked);
        TEST_ASSERT_TRUE (handler_probe.request_seq != 0);
        TEST_ASSERT_EQUAL_STRING_LEN (client_rid, handler_probe.peer_rid.c_str (),
                                      handler_probe.peer_rid.size ());
        TEST_ASSERT_EQUAL_STRING_LEN ("router-request", handler_probe.request_payload.c_str (),
                                      handler_probe.request_payload.size ());
    }

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-router-reply", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    close_test_socket_after_reply_callback (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_router_nested_deferred_reply_uses_paired_application_identity ()
{
    void *source = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *forwarder = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *target = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (forwarder);
    TEST_ASSERT_NOT_NULL (target);

    const char source_rid[] = "nested-source";
    const char forwarder_rid[] = "nested-forwarder";
    const char target_rid[] = "nested-target";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (source, source_rid, strlen (source_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (forwarder, forwarder_rid, strlen (forwarder_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (target, target_rid, strlen (target_rid)));

    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      source, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof (handover)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      forwarder, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof (handover)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      target, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof (handover)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (source, "inproc://nested-source"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (forwarder, "inproc://nested-forwarder"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (target, "inproc://nested-target"));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      source, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
      forwarder_rid, strlen (forwarder_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (source, "inproc://nested-forwarder"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      source, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
      target_rid, strlen (target_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (source, "inproc://nested-target"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      forwarder, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
      target_rid, strlen (target_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (forwarder, "inproc://nested-target"));
    msleep (SETTLE_TIME);

    zlink_routing_id_t forwarder_route;
    memset (&forwarder_route, 0, sizeof (forwarder_route));
    memcpy (forwarder_route.data, forwarder_rid, strlen (forwarder_rid));
    forwarder_route.size = strlen (forwarder_rid);
    zlink_msg_t source_request;
    init_string_part (&source_request, "source-request");
    reply_probe_t source_reply;
    source_reply.progress_handle = source;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (
      source, &forwarder_route, &source_request, 1,
      &capture_reply, &source_reply, 0, 5000));

    request_handler_probe_t source_at_forwarder;
    recv_router_request_into_probe (forwarder, &source_at_forwarder);

    zlink_routing_id_t target_route;
    memset (&target_route, 0, sizeof (target_route));
    memcpy (target_route.data, target_rid, strlen (target_rid));
    target_route.size = strlen (target_rid);
    zlink_msg_t forwarded_request;
    init_string_part (&forwarded_request, "forwarded-request");
    reply_probe_t forwarded_reply;
    forwarded_reply.progress_handle = forwarder;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (
      forwarder, &target_route, &forwarded_request, 1,
      &capture_reply, &forwarded_reply, 0, 5000));

    request_handler_probe_t request_at_target;
    recv_router_request_into_probe (target, &request_at_target);
    const zlink_routing_id_t target_reply_route =
      request_at_target.peer_rid_value;
    const uint64_t target_request_seq = request_at_target.request_seq;
    zlink_submit_result_t target_reply_rc = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread target_reply_thread ([&] {
        zlink_msg_t reply;
        const char payload[] = "target-reply";
        if (zlink_msg_init_size (&reply, strlen (payload)) == 0) {
            memcpy (zlink_msg_data (&reply), payload, strlen (payload));
            target_reply_rc = zlink_router_reply (
              target, &target_reply_route, target_request_seq, &reply, 1);
        }
    });
    target_reply_thread.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, target_reply_rc);
    TEST_ASSERT_TRUE (wait_for_reply (&forwarded_reply));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, forwarded_reply.result);

    const zlink_routing_id_t source_reply_route =
      source_at_forwarder.peer_rid_value;
    const uint64_t source_request_seq = source_at_forwarder.request_seq;
    zlink_submit_result_t source_reply_rc = ZLINK_SUBMIT_INTERNAL_ERROR;
    std::thread source_reply_thread ([&] {
        zlink_msg_t reply;
        const char payload[] = "forwarded-terminal";
        if (zlink_msg_init_size (&reply, strlen (payload)) == 0) {
            memcpy (zlink_msg_data (&reply), payload, strlen (payload));
            source_reply_rc = zlink_router_reply (
              forwarder, &source_reply_route, source_request_seq, &reply, 1);
        }
    });
    source_reply_thread.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, source_reply_rc);
    TEST_ASSERT_TRUE (wait_for_reply (&source_reply));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, source_reply.result);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "forwarded-terminal", source_reply.payload.c_str (),
      source_reply.payload.size ());

    close_test_socket_after_reply_callback (source);
    close_test_socket_after_reply_callback (forwarder);
    test_context_socket_close_zero_linger (target);
}

void test_reply_callback_rejects_concurrent_close_until_return ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const char endpoint[] = "inproc://zmp-reply-close-race";
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_bind (router, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME);

    blocking_callback_probe_t probe;
    zlink_msg_t request;
    init_string_part (&request, "reply-close-race");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1, &block_reply_until_released,
                            &probe, ZLINK_SEND_FLAGS_NONE, 3000));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router, &handler_probe);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));
    std::atomic<int> poll_result (-1);
    std::thread poll_thread ([&] {
        zlink_poller_event_t event;
        poll_result.store (
          zlink_poller_wait (poller, &event, 1, 3000, NULL),
          std::memory_order_release);
    });

    send_captured_reply (router, &handler_probe, "reply-close-race-ok");
    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        TEST_ASSERT_TRUE (probe.cv.wait_for (
          lock, std::chrono::milliseconds (3000),
          [&probe] { return probe.entered; }));
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, zlink_close (dealer));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    {
        std::lock_guard<std::mutex> lock (probe.mutex);
        probe.release = true;
    }
    probe.cv.notify_all ();
    poll_thread.join ();
    TEST_ASSERT_EQUAL_INT (1, poll_result.load (std::memory_order_acquire));

    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_close_drain_failure_still_completes_socket_handoff ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t> state =
      zlink::socket_reqrep_internal::find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());
    TEST_ASSERT_TRUE (zlink::request_completion::try_reserve (&state->completion));
    zlink::socket_reqrep_internal::pending_key_t key;
    key.request_seq = 1;
    zlink::socket_reqrep_internal::pending_request_t pending;
    pending.key = key;
    pending.transport_pair_id = 0;
    pending.transport_pair_generation = 0;
    pending.handler = &capture_reply;
    pending.userdata = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        zlink::socket_reqrep_internal::add_socket_pending_request_locked (
          state.get (), key, pending);
    }
    zlink::request_completion::close (&state->completion);
    handle = socket_handle_t ();

    const zlink_close_result_t close_result = zlink_close (dealer);
    const int close_errno = zlink_errno ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INTERNAL_ERROR, close_result);
    TEST_ASSERT_EQUAL_INT (ETERM, close_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx));
}

void test_connect_only_router_requester_receives_reply ()
{
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);

    const char server_rid[] = "connect-only-srv";
    const char client_rid[] = "connect-only-cli";

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server_router, server_rid, strlen (server_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client_router, client_rid, strlen (client_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client_router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_rid, strlen (server_rid)));

    request_handler_probe_t handler_probe;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_router, "inproc://zmp-router-connect-only-requester"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_router, "inproc://zmp-router-connect-only-requester"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "connect-only-request");

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, server_rid, strlen (server_rid));
    peer_rid.size = strlen (server_rid);

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_part, 1,
                                                     &capture_reply, &reply_probe, 0, 5000));

    recv_router_request_into_probe (server_router, &handler_probe);
    msleep (SETTLE_TIME);
    send_captured_reply (server_router, &handler_probe, "connect-only-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    {
        std::lock_guard<std::mutex> lock (handler_probe.mutex);
        TEST_ASSERT_TRUE (handler_probe.invoked);
        TEST_ASSERT_TRUE (handler_probe.request_seq != 0);
        TEST_ASSERT_EQUAL_STRING_LEN (client_rid, handler_probe.peer_rid.c_str (),
                                      handler_probe.peer_rid.size ());
        TEST_ASSERT_EQUAL_STRING_LEN ("connect-only-request", handler_probe.request_payload.c_str (),
                                      handler_probe.request_payload.size ());
    }

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("connect-only-reply", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    close_test_socket_after_reply_callback (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_router_exact_request_to_dealer_completes_on_async_owner ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);

    const char dealer_rid_text[] = "exact-dealer-server";
    set_routing_id_text (dealer, dealer_rid_text);
    set_routing_id_text (router, "exact-router-client");
    //  This test's subject is that the exact ROUTER request completes on the
    //  async owner. Self-close from a terminal completion callback needs a
    //  pending operation to terminate, so that coverage lives in
    //  test_router_mandatory_hwm's terminal-batch self-close test, which
    //  reserves records before driving termination.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &ignore_routed_ready, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (dealer, &ignore_routed_ready, NULL));
    const uint64_t reply_lane_hwm = 1024u * 1024u;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer, ZLINK_OPT_SNDHWM, &reply_lane_hwm, sizeof (reply_lane_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_RCVHWM, &reply_lane_hwm, sizeof (reply_lane_hwm)));
    const int receive_timeout_ms = 1000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer, ZLINK_OPT_RCVTIMEO, &receive_timeout_ms,
      sizeof (receive_timeout_ms)));
    const int zero_linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (dealer, "inproc://zmp-router-exact-request-to-dealer"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (router, "inproc://zmp-router-exact-request-to-dealer"));

    zlink_routing_id_t dealer_rid;
    memset (&dealer_rid, 0, sizeof (dealer_rid));
    memcpy (dealer_rid.data, dealer_rid_text, strlen (dealer_rid_text));
    dealer_rid.size = static_cast<uint8_t> (strlen (dealer_rid_text));

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    const std::chrono::steady_clock::time_point target_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (zlink_select_routed_submit_target (router, &dealer_rid, &target)
           != ZLINK_SUBMIT_OK) {
        TEST_ASSERT_TRUE_MESSAGE (
          std::chrono::steady_clock::now () < target_deadline,
          "ROUTER exact target did not become selectable");
        msleep (1);
    }
    //  The readiness hint that used to gate this point is gone. Selecting the
    //  exact target above already proves the transport pair was admitted, so
    //  no extra barrier is needed here.
    zlink_routed_submit_target_t dealer_target;
    memset (&dealer_target, 0, sizeof (dealer_target));
    while (zlink_select_routed_submit_target (dealer, NULL, &dealer_target)
           != ZLINK_SUBMIT_OK) {
        TEST_ASSERT_TRUE_MESSAGE (
          std::chrono::steady_clock::now () < target_deadline,
          "DEALER exact target did not become selectable");
        msleep (1);
    }

    socket_handle_t dealer_handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (dealer_handle.socket);
    uint64_t epoch_before = 0;
    uint64_t public_drains_before = 0;
    uint64_t async_drains_before = 0;
    dealer_handle.socket->test_receive_owner_snapshot (
      &epoch_before, &public_drains_before, &async_drains_before);

    receive_wait_owner_probe_t wait_probe;
    dealer_handle.socket->test_set_receive_wait_hook (
      &capture_receive_wait_owner, &wait_probe);

    zlink_recv_result_t receive_result = ZLINK_RECV_INTERNAL_ERROR;
    zlink_submit_result_t reply_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    uint8_t received_message_type = 0;
    uint64_t received_request_seq = 0;
    zlink_part_flag_t received_has_more = ZLINK_PART_MORE;
    std::string received_payload;
    std::thread server ([&] () {
        zlink_msg_t received;
        zlink_msg_init (&received);
        receive_result = zlink_dealer_recv_part (
          dealer, &received_message_type, &received_request_seq, &received,
          &received_has_more, static_cast<zlink_recv_flags_t> (0));
        if (receive_result != ZLINK_RECV_OK) {
            zlink_msg_close (&received);
            return;
        }
        received_payload.assign (
          static_cast<const char *> (zlink_msg_data (&received)),
          zlink_msg_size (&received));
        zlink_msg_close (&received);

        zlink_msg_t reply;
        init_string_part (&reply, "exact-reply");
        reply_result = zlink_dealer_reply_part (
          dealer, received_request_seq, &reply, ZLINK_PART_FINAL);
    });

    bool receiver_waiting = false;
    {
        std::unique_lock<std::mutex> lock (wait_probe.mutex);
        receiver_waiting = wait_probe.cv.wait_for (
          lock, std::chrono::seconds (2),
          [&wait_probe] { return wait_probe.entered; });
    }
    size_t idle_reply_target_slots = std::numeric_limits<size_t>::max ();
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      dealer_request_state =
        zlink::socket_reqrep_internal::find_request_reply_state (dealer_handle);
    if (dealer_request_state) {
        std::lock_guard<std::mutex> state_lock (dealer_request_state->mutex);
        idle_reply_target_slots = dealer_request_state->reply_target_slots;
    }

    completion_owner_probe_t probe;
    zlink_msg_t request;
    init_string_part (&request, "exact-request");
    const zlink_submit_result_t request_result =
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &request,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 3000,
        &capture_completion_owner_reply, &probe);

    server.join ();
    dealer_handle.socket->test_set_receive_wait_hook (NULL, NULL);

    uint64_t epoch_after = 0;
    uint64_t public_drains_after = 0;
    uint64_t async_drains_after = 0;
    dealer_handle.socket->test_receive_owner_snapshot (
      &epoch_after, &public_drains_after, &async_drains_after);

    TEST_ASSERT_TRUE_MESSAGE (
      receiver_waiting,
      "blocking DEALER receive did not enter the async-owner progress wait");
    TEST_ASSERT_NOT_NULL (dealer_request_state.get ());
    TEST_ASSERT_EQUAL_UINT64 (0, idle_reply_target_slots);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, receive_result);
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST,
                             received_message_type);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_has_more);
    TEST_ASSERT_EQUAL_STRING ("exact-request", received_payload.c_str ());
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, reply_result);
    TEST_ASSERT_EQUAL_UINT64 (public_drains_before, public_drains_after);
    TEST_ASSERT_TRUE_MESSAGE (
      async_drains_after > async_drains_before,
      "async mailbox owner did not process the request activation");
    TEST_ASSERT_EQUAL_UINT64 (epoch_before + 1, epoch_after);

    std::unique_lock<std::mutex> lock (probe.mutex);
    TEST_ASSERT_TRUE_MESSAGE (
      probe.cv.wait_for (lock, std::chrono::seconds (4),
                         [&probe] { return probe.done; }),
      "async completion owner lost an exact ROUTER request reply");
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, probe.result);
    TEST_ASSERT_EQUAL_UINT64 (1, probe.callback_count);
    TEST_ASSERT_EQUAL_STRING ("exact-reply", probe.payload.c_str ());
    lock.unlock ();

    dealer_handle = socket_handle_t ();
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_multiple_in_flight_requests_complete_independently ()
{
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);

    const char server_rid[] = "router-srv";
    const char client_rid[] = "router-cli";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server_router, server_rid, strlen (server_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client_router, client_rid, strlen (client_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client_router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_rid, strlen (server_rid)));

    multi_request_probe_t request_probe;

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_router, "inproc://zmp-router-multi-inflight"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_router, "inproc://zmp-router-multi-inflight"));
    msleep (SETTLE_TIME);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, server_rid, strlen (server_rid));
    peer_rid.size = strlen (server_rid);

    zlink_msg_t request_a;
    zlink_msg_t request_b;
    zlink_msg_init (&request_a);
    zlink_msg_init (&request_b);
    init_string_part (&request_a, "request-A");
    init_string_part (&request_b, "request-B");

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = client_router;
    reply_b.progress_handle = client_router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_a, 1,
                                                     &capture_reply, &reply_a, 0, 3000));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_b, 1,
                                                     &capture_reply, &reply_b, 0, 3000));

    recv_router_request_into_event (server_router, &request_probe);
    recv_router_request_into_event (server_router, &request_probe);

    request_event_t first;
    request_event_t second;
    {
        std::lock_guard<std::mutex> lock (request_probe.mutex);
        first = request_probe.events[0];
        second = request_probe.events[1];
    }

    TEST_ASSERT_TRUE (first.request_seq != second.request_seq);
    send_router_reply_to_event (server_router, first, "reply-A");
    send_router_reply_to_event (server_router, second, "reply-B");

    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));

    {
        std::lock_guard<std::mutex> lock (reply_a.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_a.result);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-A", reply_a.payload.c_str (), reply_a.payload.size ());
    }
    {
        std::lock_guard<std::mutex> lock (reply_b.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_b.result);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-B", reply_b.payload.c_str (), reply_b.payload.size ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close_zero_linger (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_out_of_order_replies_match_original_request ()
{
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);

    const char server_rid[] = "router-srv";
    const char client_rid[] = "router-cli";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server_router, server_rid, strlen (server_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client_router, client_rid, strlen (client_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client_router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_rid, strlen (server_rid)));

    multi_request_probe_t request_probe;

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_router, "inproc://zmp-router-out-of-order"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_router, "inproc://zmp-router-out-of-order"));
    msleep (SETTLE_TIME);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, server_rid, strlen (server_rid));
    peer_rid.size = strlen (server_rid);

    zlink_msg_t request_a;
    zlink_msg_t request_b;
    zlink_msg_init (&request_a);
    zlink_msg_init (&request_b);
    init_string_part (&request_a, "request-first");
    init_string_part (&request_b, "request-second");

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = client_router;
    reply_b.progress_handle = client_router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_a, 1,
                                                     &capture_reply, &reply_a, 0, 3000));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_b, 1,
                                                     &capture_reply, &reply_b, 0, 3000));

    recv_router_request_into_event (server_router, &request_probe);
    recv_router_request_into_event (server_router, &request_probe);

    request_event_t first;
    request_event_t second;
    {
        std::lock_guard<std::mutex> lock (request_probe.mutex);
        first = request_probe.events[0];
        second = request_probe.events[1];
    }

    send_router_reply_to_event (server_router, second, "reply-second");
    send_router_reply_to_event (server_router, first, "reply-first");

    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));

    {
        std::lock_guard<std::mutex> lock (reply_a.mutex);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-first", reply_a.payload.c_str (),
                                      reply_a.payload.size ());
    }
    {
        std::lock_guard<std::mutex> lock (reply_b.mutex);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-second", reply_b.payload.c_str (),
                                      reply_b.payload.size ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close_zero_linger (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_extra_reply_is_dropped_after_first_completion ()
{
    void *server_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server_router);
    TEST_ASSERT_NOT_NULL (client_router);

    const char server_rid[] = "router-srv";
    const char client_rid[] = "router-cli";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server_router, server_rid, strlen (server_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client_router, client_rid, strlen (client_rid)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client_router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_rid, strlen (server_rid)));

    request_handler_probe_t handler_probe;

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_router, "inproc://zmp-router-extra-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_router, "inproc://zmp-router-extra-reply"));
    msleep (SETTLE_TIME);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, server_rid, strlen (server_rid));
    peer_rid.size = strlen (server_rid);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "request-extra");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client_router, &peer_rid, &request_part, 1,
                                                     &capture_reply, &reply_probe, 0, 3000));

    recv_router_request_into_probe (server_router, &handler_probe);
    TEST_ASSERT_TRUE (wait_for_request_handler (&handler_probe));
    send_captured_reply (server_router, &handler_probe, "reply-first");

    TEST_ASSERT_TRUE (wait_for_reply_count (&reply_probe, 1));
    send_captured_reply (server_router, &handler_probe, "reply-second");
    msleep (SETTLE_TIME * 2);

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-first", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close_zero_linger (client_router);
    test_context_socket_close_zero_linger (server_router);
}

void test_dealer_to_dealer_reply_routes_to_source_peer_and_closes ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_b = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_a);
    TEST_ASSERT_NOT_NULL (client_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_dealer, "inproc://zmp-dealer-dealer-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_a, "inproc://zmp-dealer-dealer-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_b, "inproc://zmp-dealer-dealer-reply"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_a;
    zlink_msg_t request_b;
    zlink_msg_init (&request_a);
    zlink_msg_init (&request_b);
    init_string_part (&request_a, "from-a");
    init_string_part (&request_b, "from-b");

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = client_a;
    reply_b.progress_handle = client_b;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (client_a, &request_a, 1, &capture_reply, &reply_a, 0, 3000));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (client_b, &request_b, 1, &capture_reply, &reply_b, 0, 3000));

    uint64_t seq_a = 0;
    uint64_t seq_b = 0;
    for (int i = 0; i < 2; ++i) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_t part;
        zlink_msg_init (&part);
        TEST_ASSERT_SUCCESS_ERRNO (recv_dealer_part_with_retry (server_dealer, &message_type,
                                                                &request_seq, &part, &has_more));
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
        TEST_ASSERT_TRUE (request_seq != 0);
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
        const std::string payload = part_to_string_and_close (&part);
        if (payload == "from-a")
            seq_a = request_seq;
        else if (payload == "from-b")
            seq_b = request_seq;
        else
            TEST_FAIL_MESSAGE ("unexpected dealer request payload");
    }

    TEST_ASSERT_TRUE (seq_a != 0);
    TEST_ASSERT_TRUE (seq_b != 0);
    TEST_ASSERT_TRUE (seq_a != seq_b);

    zlink_msg_t reply_part_b;
    zlink_msg_t reply_part_a;
    zlink_msg_init (&reply_part_b);
    zlink_msg_init (&reply_part_a);
    init_string_part (&reply_part_b, "reply-b");
    init_string_part (&reply_part_a, "reply-a");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_reply_part (server_dealer, seq_b, &reply_part_b, ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_reply_part (server_dealer, seq_a, &reply_part_a, ZLINK_PART_FINAL));

    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));

    {
        std::lock_guard<std::mutex> lock (reply_a.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_a.result);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-a", reply_a.payload.c_str (), reply_a.payload.size ());
    }
    {
        std::lock_guard<std::mutex> lock (reply_b.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_b.result);
        TEST_ASSERT_EQUAL_STRING_LEN ("reply-b", reply_b.payload.c_str (), reply_b.payload.size ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close (client_a);
    test_context_socket_close (client_b);
    test_context_socket_close (server_dealer);
}

void test_dealer_to_dealer_multipart_reply_preserves_large_first_part ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_dealer, "inproc://zmp-dealer-large-first-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_dealer, "inproc://zmp-dealer-large-first-reply"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "request");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (client_dealer, &request_part, 1, &capture_reply, &reply_probe, 0,
                            3000));

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_SUCCESS_ERRNO (recv_dealer_part_with_retry (
      server_dealer, &message_type, &request_seq, &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("request", part_to_string_and_close (&received).c_str ());

    const std::string large_first = std::string (320, 'x');
    zlink_msg_t reply_first;
    zlink_msg_t reply_second;
    zlink_msg_init (&reply_first);
    zlink_msg_init (&reply_second);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_first, large_first.size ()));
    memcpy (zlink_msg_data (&reply_first), large_first.data (), large_first.size ());
    init_string_part (&reply_second, "reply-body");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_reply_part (server_dealer, request_seq, &reply_first, ZLINK_PART_MORE));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_reply_part (server_dealer, request_seq, &reply_second, ZLINK_PART_FINAL));

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (2, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN (large_first.c_str (), reply_probe.payload.c_str (),
                                      large_first.size ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close (client_dealer);
    test_context_socket_close (server_dealer);
}

void test_request_stage_allocation_failure_consumes_and_aborts_sequence ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    zlink_msg_t first;
    zlink_msg_t failed;
    zlink_msg_init (&first);
    zlink_msg_init (&failed);
    init_string_part (&first, "staged-before-oom");
    init_string_part (&failed, "stage-oom");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &first, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_MORE, 0, NULL, NULL));
    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_stage_payload);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_dealer_request_part (dealer, &failed, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_MORE, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&first));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&failed));
    TEST_ASSERT_FALSE (
      zlink::part_helper_internal::send_sequence_active (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&first));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed));

    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_reply_combined_allocation_failure_preserves_token ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_dealer, "inproc://zmp-dealer-reply-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_dealer, "inproc://zmp-dealer-reply-oom"));
    msleep (SETTLE_TIME);

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "dealer-oom-request");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (client_dealer, &request, 1, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 30000));

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server_dealer, &message_type, &request_seq,
                                   &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_STRING ("dealer-oom-request",
                              part_to_string_and_close (&received).c_str ());

    zlink_msg_t staged[4];
    for (size_t i = 0; i < 4; ++i) {
        zlink_msg_init (&staged[i]);
        init_string_part (&staged[i], "discarded");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_reply_part (server_dealer, request_seq, &staged[i],
                                   ZLINK_PART_MORE));
    }

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_dealer_combined);
    zlink_msg_t failed_final;
    zlink_msg_init (&failed_final);
    init_string_part (&failed_final, "discarded-final");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_dealer_reply_part (server_dealer, request_seq, &failed_final,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&failed_final));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed_final));
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&staged[i]));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&staged[i]));
    }

    zlink_msg_t fresh_reply;
    zlink_msg_init (&fresh_reply);
    init_string_part (&fresh_reply, "fresh-dealer-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server_dealer, request_seq, &fresh_reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING ("fresh-dealer-reply",
                                  reply_probe.payload.c_str ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close (client_dealer);
    test_context_socket_close (server_dealer);
}

void test_router_reply_allocations_preserve_target_until_fresh_reply ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-reply-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-reply-oom"));
    msleep (SETTLE_TIME);

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "router-oom-request");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 30000));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *request_parts = NULL;
    size_t request_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &request_parts,
                         &request_part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    zlink_routing_id_t reply_rid = *source_rid;
    zlink_multipart_close (request_parts, request_part_count);

    zlink_msg_t null_final_head;
    zlink_msg_init (&null_final_head);
    init_string_part (&null_final_head, "discarded-null-final-head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply_part (router, &reply_rid, request_seq, &null_final_head,
                               ZLINK_PART_MORE));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_router_reply_part (router, &reply_rid, request_seq, NULL,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_FALSE (
      zlink::part_helper_internal::send_sequence_active (router));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&null_final_head));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&null_final_head));

    zlink_msg_t invalid_final_head;
    zlink_msg_t invalid_final;
    zlink_msg_init (&invalid_final_head);
    zlink_msg_init (&invalid_final);
    init_string_part (&invalid_final_head, "discarded-invalid-final-head");
    init_string_part (&invalid_final, "invalid-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply_part (router, &reply_rid, request_seq,
                               &invalid_final_head, ZLINK_PART_MORE));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_final));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      zlink_router_reply_part (router, &reply_rid, request_seq, &invalid_final,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_FALSE (
      zlink::part_helper_internal::send_sequence_active (router));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&invalid_final_head));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_final_head));

    zlink_msg_t outer_head;
    zlink_msg_t outer_final;
    zlink_msg_init (&outer_head);
    zlink_msg_init (&outer_final);
    init_string_part (&outer_head, "outer-head");
    init_string_part (&outer_final, "outer-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply_part (router, &reply_rid, request_seq, &outer_head,
                               ZLINK_PART_MORE));
    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_router_payload);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_router_reply_part (router, &reply_rid, request_seq, &outer_final,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&outer_head));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&outer_final));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&outer_head));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&outer_final));

    zlink_msg_t runtime_staged[4];
    for (size_t i = 0; i < 4; ++i) {
        zlink_msg_init (&runtime_staged[i]);
        init_string_part (&runtime_staged[i], "runtime-discarded");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_router_reply_part (router, &reply_rid, request_seq,
                                   &runtime_staged[i], ZLINK_PART_MORE));
    }
    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_runtime_combined);
    zlink_msg_t runtime_final;
    zlink_msg_init (&runtime_final);
    init_string_part (&runtime_final, "runtime-final");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_router_reply_part (router, &reply_rid, request_seq, &runtime_final,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&runtime_final));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&runtime_final));
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&runtime_staged[i]));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&runtime_staged[i]));
    }

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_reply_key);
    zlink_msg_t key_failure;
    zlink_msg_init (&key_failure);
    init_string_part (&key_failure, "key-failure");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_router_reply_part (router, &reply_rid, request_seq, &key_failure,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&key_failure));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&key_failure));

    zlink_msg_t fresh_reply;
    zlink_msg_init (&fresh_reply);
    init_string_part (&fresh_reply, "fresh-router-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply_part (router, &reply_rid, request_seq, &fresh_reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING ("fresh-router-reply",
                                  reply_probe.payload.c_str ());
    }

    msleep (SETTLE_TIME);
    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

void test_dealer_request_receive_without_reply_closes_cleanly ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_dealer, "inproc://zmp-dealer-unreplied-close"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_dealer, "inproc://zmp-dealer-unreplied-close"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "unreplied");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (client_dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 50));

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_SUCCESS_ERRNO (recv_dealer_part_with_retry (server_dealer, &message_type,
                                                            &request_seq, &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    const std::string received_payload = part_to_string_and_close (&received);
    TEST_ASSERT_EQUAL_STRING_LEN ("unreplied", received_payload.c_str (), received_payload.size ());

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, reply_probe.result);
    }

    msleep (SETTLE_TIME);
    test_context_socket_close (client_dealer);
    test_context_socket_close (server_dealer);
}

void test_dealer_close_drains_pending_request_completion ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);

    const char endpoint[] = "inproc://zmp-dealer-pending-close-drain";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_dealer, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client_dealer, endpoint));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "pending-close");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (client_dealer, &request_part, 1, &capture_reply, &reply_probe, 0,
                            3000));

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_SUCCESS_ERRNO (recv_dealer_part_with_retry (server_dealer, &message_type,
                                                            &request_seq, &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("pending-close", part_to_string_and_close (&received).c_str ());

    test_context_socket_close (client_dealer);
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TERMINATED, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (0, reply_probe.part_count);
    }

    test_context_socket_close (server_dealer);
}

void test_router_request_to_dealer_times_out_when_reply_is_missing ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-peer", 11));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router, "router-cli", 10));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (dealer, "inproc://zmp-router-dealer-no-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (router, "inproc://zmp-router-dealer-no-reply"));
    msleep (SETTLE_TIME);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    memcpy (peer_rid.data, "dealer-peer", 11);
    peer_rid.size = 11;

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "router-request");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = router;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (router, &peer_rid, &request_part, 1,
                                                     &capture_reply, &reply_probe, 0, 50));

    uint8_t message_type = ZLINK_DEALER_MESSAGE_RAW;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (
        dealer, &message_type, &request_seq, &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING (
      "router-request", part_to_string_and_close (&received).c_str ());

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (0, reply_probe.part_count);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
    }

    msleep (SETTLE_TIME);
    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_request_uses_socket_default_timeout_when_reply_is_missing ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-A", 8));
    const int default_timeout_ms = 50;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
                                                        &default_timeout_ms,
                                                        sizeof (default_timeout_ms)));
    configure_submit_retry (dealer);

    int observed_timeout_ms = 0;
    size_t observed_timeout_size = sizeof (observed_timeout_ms);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_router_option (
      router, ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, &observed_timeout_ms, &observed_timeout_size));
    TEST_ASSERT_EQUAL_INT (5000, observed_timeout_ms);

    request_handler_probe_t handler_probe;

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://zmp-dealer-default-timeout"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://zmp-dealer-default-timeout"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-timeout-request");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 0));

    recv_router_request_into_probe (router, &handler_probe);
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    const long elapsed_ms =
      static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - start)
                           .count ());

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (0, reply_probe.part_count);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 500,
                                  "submit retry must not retry request completion timeout");
    }

    msleep (SETTLE_TIME);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_reply_timeout_does_not_turn_failed_request_admission_into_success ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    const int send_timeout_ms = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://request-timeout-before-admission"));

    zlink_msg_t request;
    init_string_part (&request, "never-admitted-request");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    errno = 0;
    const zlink_submit_result_t submit = zlink_dealer_request_part (
      dealer, &request, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 10,
      &capture_reply, &reply_probe);
    const int submit_errno = errno;

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, submit);
    TEST_ASSERT_EQUAL_INT (EAGAIN, submit_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&request));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));
    msleep (30);
    (void) drain_completion_via_poller (dealer);
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_FALSE (reply_probe.done);
        TEST_ASSERT_EQUAL_UINT64 (0, reply_probe.callback_count);
    }

    test_context_socket_close_zero_linger (dealer);
}

void test_router_reply_target_slots_are_bounded_and_released ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "bounded-target-peer");

    const uint64_t large_hwm = 128u * 1024u * 1024u;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &large_hwm, sizeof (large_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &large_hwm, sizeof (large_hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://bounded-reply-targets"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://bounded-reply-targets"));

    zlink_routing_id_t first_peer;
    memset (&first_peer, 0, sizeof (first_peer));
    uint64_t first_sequence = 0;
    for (size_t i = 0;
         i < zlink::socket_reqrep_internal::max_reply_target_slots;
         ++i) {
        send_raw_request_envelope (dealer, static_cast<uint64_t> (i + 1));
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (router, &source_rid, &request_seq, &parts,
                             &part_count, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        if (i == 0) {
            first_peer = *source_rid;
            first_sequence = request_seq;
        }
        zlink_multipart_close (parts, part_count);
    }

    send_raw_request_envelope (
      dealer,
      static_cast<uint64_t> (
        zlink::socket_reqrep_internal::max_reply_target_slots + 1));
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    zlink_msg_t reply;
    init_string_part (&reply, "released");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply (router, &first_peer, first_sequence, &reply, 1));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (
      zlink::socket_reqrep_internal::max_reply_target_slots + 1,
      request_seq);
    zlink_multipart_close (parts, part_count);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}
}

int main ()
{
    setup_test_environment (180);

#define RUN_SELECTED(test_)                                                        \
    do {                                                                           \
        if (should_run_request_reply_test (#test_))                                 \
            RUN_TEST (test_);                                                       \
    } while (false)

    UNITY_BEGIN ();
    RUN_SELECTED (test_reserved_request_reply_message_type_is_invalid);
    RUN_SELECTED (test_reply_callback_rejects_sync_and_async_submit_on_all_sockets);
    RUN_SELECTED (test_dealer_to_router_request_reply_basic);
    RUN_SELECTED (test_dealer_receives_unsolicited_message_after_request_reply);
    RUN_SELECTED (test_concurrent_first_dealer_requests_share_dispatch_install);
    RUN_SELECTED (test_blocking_generic_dealer_recv_remains_on_transport_pipe);
    RUN_SELECTED (test_blocking_typed_dealer_recv_remains_on_transport_pipe);
    RUN_SELECTED (test_generic_dealer_recv_honors_configured_no_input_timeout);
    RUN_SELECTED (test_typed_dealer_recv_honors_configured_no_input_timeout);
    RUN_SELECTED (test_direct_dealer_generic_recv_and_poller_preserve_raw_order);
    RUN_SELECTED (test_prefixed_multipart_second_prefix_allocation_failure_rolls_back);
    RUN_SELECTED (test_dealer_to_router_request_reply_over_tcp_with_explicit_routing_id);
    RUN_SELECTED (test_router_reply_completion_backpressure_recovers_over_tcp);
    RUN_SELECTED (test_router_poller_combines_input_and_completion_ownership);
    RUN_SELECTED (test_socket_poller_wakes_after_async_owner_applies_input);
    RUN_SELECTED (test_completion_poller_exclusively_owns_routed_async_completion);
    RUN_SELECTED (test_completion_poller_quiesces_callbackless_async_owner);
    RUN_SELECTED (test_last_completion_poller_release_resumes_pending_request);
    RUN_SELECTED (test_application_only_poller_does_not_take_completion_ownership);
    RUN_SELECTED (test_disconnect_of_paired_endpoint_stops_reconnecting);
    RUN_SELECTED (test_dealer_disconnect_fails_only_requests_on_that_pipe);
    RUN_SELECTED (test_router_completion_correlation_accepts_settled_peer_and_fences_pair);
    RUN_SELECTED (test_router_to_router_request_reply_basic);
    RUN_SELECTED (test_router_nested_deferred_reply_uses_paired_application_identity);
    RUN_SELECTED (test_reply_callback_rejects_concurrent_close_until_return);
    RUN_SELECTED (test_close_drain_failure_still_completes_socket_handoff);
    RUN_SELECTED (test_connect_only_router_requester_receives_reply);
    RUN_SELECTED (test_router_exact_request_to_dealer_completes_on_async_owner);
    RUN_SELECTED (test_multiple_in_flight_requests_complete_independently);
    RUN_SELECTED (test_out_of_order_replies_match_original_request);
    RUN_SELECTED (test_extra_reply_is_dropped_after_first_completion);
    RUN_SELECTED (test_dealer_to_dealer_reply_routes_to_source_peer_and_closes);
    RUN_SELECTED (test_dealer_to_dealer_multipart_reply_preserves_large_first_part);
    RUN_SELECTED (test_request_stage_allocation_failure_consumes_and_aborts_sequence);
    RUN_SELECTED (test_dealer_reply_combined_allocation_failure_preserves_token);
    RUN_SELECTED (test_router_reply_allocations_preserve_target_until_fresh_reply);
    RUN_SELECTED (test_dealer_request_receive_without_reply_closes_cleanly);
    RUN_SELECTED (test_dealer_close_drains_pending_request_completion);
    RUN_SELECTED (test_router_request_to_dealer_times_out_when_reply_is_missing);
    RUN_SELECTED (test_dealer_request_uses_socket_default_timeout_when_reply_is_missing);
    RUN_SELECTED (test_reply_timeout_does_not_turn_failed_request_admission_into_success);
    RUN_SELECTED (test_router_reply_target_slots_are_bounded_and_released);
    RUN_SELECTED (test_request_reply_process_exits_cleanly_after_round_trip);
#undef RUN_SELECTED
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
