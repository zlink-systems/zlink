/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/command.hpp"
#include "core/multipart_send_txn.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <string.h>
#include <thread>
#include <unordered_map>

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

void process_socket_commands_through_public_api (void *socket_)
{
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket_, ZLINK_OPT_EVENTS, &events, &events_size));
}

struct reply_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    zlink_request_result_t result;
    size_t part_count;
    std::string payload;
    std::vector<std::string> parts;
    size_t callback_count;
    void *progress_handle;
    bool metadata_present;

    reply_probe_t () :
        done (false),
        result (ZLINK_REQUEST_PROTOCOL_ERROR),
        part_count (0),
        callback_count (0),
        progress_handle (NULL),
        metadata_present (false)
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

struct async_input_command_probe_t
{
    async_input_command_probe_t () : activate_read_seen (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool activate_read_seen;
};

void capture_async_input_command (void *userdata_, int command_type_, bool,
                                  bool)
{
    async_input_command_probe_t *probe =
      static_cast<async_input_command_probe_t *> (userdata_);
    if (!probe
        || command_type_ != static_cast<int> (zlink::command_t::activate_read))
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->activate_read_seen = true;
    }
    probe->cv.notify_all ();
}

bool wait_for_async_input_command (async_input_command_probe_t *probe_,
                                   int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_),
      [probe_] () { return probe_->activate_read_seen; });
}

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
    bool metadata_present;

    request_handler_probe_t () : invoked (false), request_seq (0),
                                 metadata_present (false)
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

enum reply_ownership_action_t
{
    reply_ownership_leave_untouched,
    reply_ownership_close,
    reply_ownership_move
};

struct reply_ownership_probe_t
{
    explicit reply_ownership_probe_t (reply_ownership_action_t action_) :
        done (false), result (ZLINK_REQUEST_PROTOCOL_ERROR), callback_count (0),
        action (action_)
    {
        zlink_msg_init (&moved);
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    zlink_request_result_t result;
    size_t callback_count;
    reply_ownership_action_t action;
    std::string payload;
    zlink_msg_t moved;
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

void init_bytes_part (zlink_msg_t *part_, const std::string &bytes_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, bytes_.size ()));
    if (!bytes_.empty ())
        memcpy (zlink_msg_data (part_), bytes_.data (), bytes_.size ());
}

void init_filled_part (zlink_msg_t *part_, size_t size_, unsigned char value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, size_));
    memset (zlink_msg_data (part_), value_, size_);
}

zlink_auto_hwm_budget_snapshot_t read_request_reply_hwm_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
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

void count_reply_payload_free (void *, void *hint_)
{
    static_cast<std::atomic<int> *> (hint_)->fetch_add (
      1, std::memory_order_release);
}

void capture_reply_ownership (zlink_request_result_t result_,
                              zlink_msg_t *parts_,
                              size_t part_count_,
                              void *userdata_)
{
    reply_ownership_probe_t *probe =
      static_cast<reply_ownership_probe_t *> (userdata_);
    TEST_ASSERT_NOT_NULL (probe);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count_);

    const std::string payload (
      static_cast<const char *> (zlink_msg_data (&parts_[0])),
      zlink_msg_size (&parts_[0]));
    if (probe->action == reply_ownership_close) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts_[0]));
    } else if (probe->action == reply_ownership_move) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_move (&probe->moved, &parts_[0]));
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
        probe->result = result_;
        ++probe->callback_count;
        probe->payload = payload;
    }
    probe->cv.notify_all ();
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

void send_internal_request_message (void *dealer_, uint64_t request_seq_)
{
    zlink_msg_t payload;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&payload, 1));
    *static_cast<unsigned char *> (zlink_msg_data (&payload)) = 'r';
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&payload)
        ->set_request_reply_metadata (
          zlink::request_reply::request_type, request_seq_));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer_, &payload, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
}

void send_internal_request_multipart_message (void *dealer_,
                                              uint64_t request_seq_,
                                              size_t part_count_)
{
    TEST_ASSERT_TRUE (part_count_ > 1);
    for (size_t i = 0; i < part_count_; ++i) {
        const std::string payload = "request-part-" + std::to_string (i);
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_msg_init_size (&part, payload.size ()));
        memcpy (zlink_msg_data (&part), payload.data (), payload.size ());
        if (i == 0) {
            TEST_ASSERT_SUCCESS_ERRNO (
              reinterpret_cast<zlink::msg_t *> (&part)
                ->set_request_reply_metadata (
                  zlink::request_reply::request_type, request_seq_));
        }
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer_, &part, ZLINK_SEND_FLAGS_NONE,
                           i + 1 == part_count_ ? ZLINK_PART_FINAL
                                                : ZLINK_PART_MORE));
    }
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

bool wait_for_reply_ownership (reply_ownership_probe_t *probe_,
                               void *progress_handle_)
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
        (void) drain_completion_via_poller (progress_handle_);
    }
    return false;
}

bool wait_for_free_count (const std::atomic<int> &free_count_, int expected_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);
    while (std::chrono::steady_clock::now () < deadline) {
        if (free_count_.load (std::memory_order_acquire) == expected_)
            return true;
        msleep (1);
    }
    return free_count_.load (std::memory_order_acquire) == expected_;
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

void store_router_request_in_probe (const zlink_routing_id_t *peer_rid_,
                                    uint64_t request_seq_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    request_handler_probe_t *probe_)
{
    TEST_ASSERT_NOT_NULL (probe_);
    TEST_ASSERT_NOT_NULL (peer_rid_);
    {
        std::lock_guard<std::mutex> lock (probe_->mutex);
        probe_->invoked = true;
        probe_->request_seq = request_seq_;
        probe_->peer_rid_value = *peer_rid_;
        probe_->peer_rid.assign (
          reinterpret_cast<const char *> (peer_rid_->data), peer_rid_->size);
        probe_->request_payload =
          part_count_ > 0 ? msg_to_string (&parts_[0]) : std::string ();
        unsigned char kind = 0;
        uint64_t sequence = 0;
        probe_->metadata_present =
          part_count_ > 0
          && reinterpret_cast<zlink::msg_t *> (&parts_[0])
               ->get_request_reply_metadata (&kind, &sequence);
    }
    zlink_multipart_close (parts_, part_count_);
    probe_->cv.notify_all ();
}

void recv_router_request_into_probe (void *router_, request_handler_probe_t *probe_)
{
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router_, &peer_rid,
                                                  &request_seq, &parts, &part_count, 0));
    store_router_request_in_probe (peer_rid, request_seq, parts, part_count,
                                   probe_);
}

bool try_recv_router_request_into_probe (void *router_,
                                         request_handler_probe_t *probe_)
{
    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_recv_result_t result = zlink_router_recv (
      router_, &peer_rid, &request_seq, &parts, &part_count,
      ZLINK_RECV_FLAGS_DONTWAIT);
    if (result == ZLINK_RECV_NO_DATA) {
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        return false;
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    store_router_request_in_probe (peer_rid, request_seq, parts, part_count,
                                   probe_);
    return true;
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

void test_reserved_zmp_kind_is_not_request_reply ()
{
    const uint8_t reserved_message_type = 0x04;
    TEST_ASSERT_FALSE (
      zlink::zmp_is_request_reply_kind (reserved_message_type));
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
    probe->parts.clear ();
    for (size_t i = 0; i < part_count_; ++i)
        probe->parts.push_back (msg_to_string (&parts_[i]));
    unsigned char kind = 0;
    uint64_t sequence = 0;
    probe->metadata_present =
      part_count_ > 0
      && reinterpret_cast<zlink::msg_t *> (&parts_[0])
           ->get_request_reply_metadata (&kind, &sequence);
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

void run_reply_callback_ownership_case (void *router_,
                                        void *dealer_,
                                        reply_ownership_action_t action_)
{
    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "ownership-request");

    reply_ownership_probe_t reply_probe (action_);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer_, &request_part, 1,
                            &capture_reply_ownership, &reply_probe, 0, 3000));

    request_handler_probe_t handler_probe;
    recv_router_request_into_probe (router_, &handler_probe);

    char reply_payload[] = "ownership-reply";
    std::atomic<int> free_count (0);
    zlink_msg_t reply_part;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_data (&reply_part, reply_payload,
                           sizeof (reply_payload) - 1,
                           &count_reply_payload_free, &free_count));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_reply (router_, &handler_probe.peer_rid_value,
                          handler_probe.request_seq, &reply_part, 1));

    TEST_ASSERT_TRUE (wait_for_reply_ownership (&reply_probe, dealer_));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_STRING_LEN (
          reply_payload, reply_probe.payload.c_str (), reply_probe.payload.size ());
    }

    if (action_ == reply_ownership_move) {
        TEST_ASSERT_EQUAL_INT (0, free_count.load (std::memory_order_acquire));
        TEST_ASSERT_EQUAL_STRING_LEN (
          reply_payload,
          static_cast<const char *> (zlink_msg_data (&reply_probe.moved)),
          zlink_msg_size (&reply_probe.moved));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&reply_probe.moved));
    } else {
        TEST_ASSERT_TRUE (wait_for_free_count (free_count, 1));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&reply_probe.moved));
    }

    TEST_ASSERT_TRUE (wait_for_free_count (free_count, 1));
    TEST_ASSERT_EQUAL_INT (1, free_count.load (std::memory_order_acquire));
}

void test_reply_callback_cleanup_preserves_ownership_actions ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-reply-callback-ownership"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-reply-callback-ownership"));
    msleep (SETTLE_TIME);

    run_reply_callback_ownership_case (
      router, dealer, reply_ownership_leave_untouched);
    run_reply_callback_ownership_case (router, dealer, reply_ownership_close);
    run_reply_callback_ownership_case (router, dealer, reply_ownership_move);

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

void test_generic_dealer_receive_clears_request_reply_metadata ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (sender);
    TEST_ASSERT_NOT_NULL (receiver);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://zmp-generic-recv-clears-metadata"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://zmp-generic-recv-clears-metadata"));
    msleep (SETTLE_TIME);

    const uint8_t kinds[] = {
      zlink::request_reply::request_type,
      zlink::request_reply::reply_type,
      zlink::request_reply::error_reply_type};
    for (size_t i = 0; i < sizeof (kinds) / sizeof (kinds[0]); ++i) {
        const std::string payload = "raw-kind-" + std::to_string (i);
        zlink_msg_t sent;
        zlink_msg_init (&sent);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_msg_init_size (&sent, payload.size ()));
        memcpy (zlink_msg_data (&sent), payload.data (), payload.size ());
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&sent)
            ->set_request_reply_metadata (kinds[i], 100 + i));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &sent, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));

        zlink_msg_t received;
        zlink_msg_init (&received);
        zlink_part_flag_t has_more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          recv_generic_part_with_retry (receiver, &received, &has_more));
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
        TEST_ASSERT_EQUAL_STRING (payload.c_str (),
                                  msg_to_string (&received).c_str ());
        unsigned char retained_kind = 0xff;
        uint64_t retained_sequence = std::numeric_limits<uint64_t>::max ();
        TEST_ASSERT_FALSE (
          reinterpret_cast<zlink::msg_t *> (&received)
            ->get_request_reply_metadata (&retained_kind,
                                          &retained_sequence));

        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (receiver, &received, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

        uint8_t echoed_type = 0xff;
        uint64_t echoed_sequence = std::numeric_limits<uint64_t>::max ();
        zlink_msg_init (&received);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          recv_dealer_part_with_retry (
            sender, &echoed_type, &echoed_sequence, &received, &has_more));
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, echoed_type);
        TEST_ASSERT_EQUAL_UINT64 (0, echoed_sequence);
        TEST_ASSERT_EQUAL_STRING (
          payload.c_str (), part_to_string_and_close (&received).c_str ());
    }

    {
        const socket_handle_t receiver_handle = as_socket_handle (receiver);
        TEST_ASSERT_NOT_NULL (receiver_handle.socket);
        TEST_ASSERT_FALSE (receiver_handle.socket->has_request_reply_state ());
    }

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "typed-after-raw");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = sender;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (sender, &request, 1, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 3000));

    uint8_t request_type = ZLINK_DEALER_MESSAGE_RAW;
    uint64_t request_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received_request;
    zlink_msg_init (&received_request);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (
        receiver, &request_type, &request_token, &received_request,
        &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, request_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_STRING (
      "typed-after-raw",
      part_to_string_and_close (&received_request).c_str ());

    zlink_msg_t reply;
    zlink_msg_init (&reply);
    init_string_part (&reply, "typed-after-raw-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (receiver, request_token, &reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("typed-after-raw-reply",
                                  reply_probe.payload.c_str ());
    }

    close_test_socket_after_reply_callback (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_dealer_receive_rejects_request_reply_metadata_after_first_part ()
{
    enum receive_surface_t
    {
        raw_part_surface,
        raw_aggregate_surface,
        typed_part_surface
    };

    for (int surface = raw_part_surface; surface <= typed_part_surface;
         ++surface) {
        void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
        void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (sender);
        TEST_ASSERT_NOT_NULL (receiver);
        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://zmp-later-kind-rejected-%d", surface);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
        msleep (SETTLE_TIME);

        zlink_msg_t first;
        init_string_part (&first, "first-kind");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&first)
            ->set_request_reply_metadata (
              zlink::request_reply::request_type, 901 + surface));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &first, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE));

        zlink_msg_t later;
        init_string_part (&later, "later-kind");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&later)
            ->set_request_reply_metadata (
              zlink::request_reply::reply_type, 901 + surface));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &later, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));

        errno = 0;
        int observed_errno = 0;
        if (surface == raw_aggregate_surface) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_recv (receiver, NULL, &parts, &part_count,
                          ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_NULL (parts);
            TEST_ASSERT_EQUAL_UINT64 (0, part_count);
        } else {
            zlink_msg_t received;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            if (surface == typed_part_surface) {
                uint8_t message_type = 0xff;
                uint64_t request_token = 0;
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_RECV_INTERNAL_ERROR,
                  zlink_dealer_recv_part (
                    receiver, &message_type, &request_token, &received,
                    &has_more, ZLINK_RECV_FLAGS_NONE));
                observed_errno = errno;
            } else {
                const zlink_routing_id_t *source_rid = NULL;
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_RECV_INTERNAL_ERROR,
                  zlink_recv_part (receiver, &source_rid, &received,
                                   &has_more, ZLINK_RECV_FLAGS_NONE));
                observed_errno = errno;
            }
            TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&received));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
        }
        TEST_ASSERT_EQUAL_INT (EPROTO, observed_errno);

        {
            const socket_handle_t receiver_handle = as_socket_handle (receiver);
            if (receiver_handle.socket->has_request_reply_state ()) {
                const std::shared_ptr<
                  zlink::socket_reqrep_internal::socket_request_reply_state_t>
                  state = receiver_handle.socket->request_reply_state ();
                std::lock_guard<std::mutex> lock (state->mutex);
                TEST_ASSERT_TRUE (state->dealer_reply_targets.empty ());
                TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_slots);
                TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_reservations);
                TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_checkouts);
            }
        }

        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_source_pipe_pin_failure_preserves_receive_ownership_without_targets ()
{
    const int receiver_types[] = {ZLINK_SOCKET_DEALER,
                                  ZLINK_SOCKET_ROUTER};
    for (size_t receiver_index = 0;
         receiver_index < sizeof (receiver_types) / sizeof (receiver_types[0]);
         ++receiver_index) {
        const int receiver_type = receiver_types[receiver_index];
        for (size_t part_count = 1; part_count <= 3; part_count += 2) {
            void *receiver = test_context_socket (receiver_type);
            void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
            TEST_ASSERT_NOT_NULL (receiver);
            TEST_ASSERT_NOT_NULL (sender);
            char endpoint[112];
            snprintf (endpoint, sizeof (endpoint),
                      "inproc://source-pin-fail-%d-%zu", receiver_type,
                      part_count);
            if (receiver_type == ZLINK_SOCKET_ROUTER)
                set_routing_id_text (sender, "source-pin-fail-router");
            TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
            msleep (SETTLE_TIME);

            if (part_count == 1)
                send_internal_request_message (sender, 1000 + part_count);
            else
                send_internal_request_multipart_message (
                  sender, 1000 + part_count, part_count);

            socket_handle_t receiver_handle =
              as_socket_handle (receiver);
            TEST_ASSERT_NOT_NULL (receiver_handle.socket);
            receiver_handle.socket->test_fail_next_recv_pipe_pin ();

            zlink_msg_t output;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
            zlink_part_flag_t has_more = ZLINK_PART_MORE;
            errno = 0;
            if (receiver_type == ZLINK_SOCKET_ROUTER) {
                const zlink_routing_id_t *source_rid = NULL;
                uint64_t request_seq = UINT64_MAX;
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_RECV_INTERNAL_ERROR,
                  zlink_router_recv_part (
                    receiver, &source_rid, &request_seq, &output, &has_more,
                    ZLINK_RECV_FLAGS_NONE));
            } else {
                uint8_t message_type = 0xff;
                uint64_t request_token = UINT64_MAX;
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_RECV_INTERNAL_ERROR,
                  zlink_dealer_recv_part (
                    receiver, &message_type, &request_token, &output,
                    &has_more, ZLINK_RECV_FLAGS_NONE));
            }
            TEST_ASSERT_EQUAL_INT (EPROTO, errno);
            TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&output));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));

            if (receiver_handle.socket->has_request_reply_state ()) {
                const std::shared_ptr<
                  zlink::socket_reqrep_internal::socket_request_reply_state_t>
                  state = receiver_handle.socket->request_reply_state ();
                std::lock_guard<std::mutex> lock (state->mutex);
                TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_slots);
                TEST_ASSERT_EQUAL_UINT64 (0,
                                          state->reply_target_reservations);
                TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_checkouts);
                TEST_ASSERT_TRUE (state->dealer_reply_targets.empty ());
                TEST_ASSERT_TRUE (state->router_reply_targets.empty ());
            }

            receiver_handle = socket_handle_t ();
            test_context_socket_close_zero_linger (sender);
            test_context_socket_close_zero_linger (receiver);
        }
    }

    // Generic DEALER receive deliberately treats a first-frame request kind
    // as ordinary raw payload. A lost source pin must not publish a local
    // reply token, and multipart ownership still has to remain well formed.
    for (size_t part_count = 1; part_count <= 3; part_count += 2) {
        void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
        void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://raw-source-pin-fail-%zu", part_count);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
        msleep (SETTLE_TIME);
        if (part_count == 1)
            send_internal_request_message (sender, 2000 + part_count);
        else
            send_internal_request_multipart_message (
              sender, 2000 + part_count, part_count);

        socket_handle_t receiver_handle = as_socket_handle (receiver);
        receiver_handle.socket->test_fail_next_recv_pipe_pin ();
        for (size_t i = 0; i < part_count; ++i) {
            zlink_msg_t output;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
            const zlink_routing_id_t *source_rid = NULL;
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_OK,
              zlink_recv_part (receiver, &source_rid, &output, &has_more,
                               ZLINK_RECV_FLAGS_NONE));
            TEST_ASSERT_NULL (source_rid);
            TEST_ASSERT_EQUAL_INT (
              i + 1 == part_count ? ZLINK_PART_FINAL : ZLINK_PART_MORE,
              has_more);
            unsigned char kind = 0xff;
            uint64_t sequence = UINT64_MAX;
            TEST_ASSERT_FALSE (
              reinterpret_cast<zlink::msg_t *> (&output)
                ->get_request_reply_metadata (&kind, &sequence));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));
        }
        TEST_ASSERT_FALSE (receiver_handle.socket->has_request_reply_state ());
        receiver_handle = socket_handle_t ();
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_pair_raw_receive_strips_first_kind_and_rejects_later_kind ()
{
    {
        void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
        void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_bind (receiver, "inproc://pair-first-kind-ordinary"));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (sender, "inproc://pair-first-kind-ordinary"));
        msleep (SETTLE_TIME);

        zlink_msg_t sent;
        init_string_part (&sent, "pair-first-kind");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&sent)
            ->set_request_reply_metadata (
              zlink::request_reply::request_type, 321));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &sent, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));
        zlink_msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
        zlink_part_flag_t has_more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          recv_generic_part_with_retry (receiver, &received, &has_more));
        TEST_ASSERT_EQUAL_STRING (
          "pair-first-kind", msg_to_string (&received).c_str ());
        unsigned char kind = 0xff;
        uint64_t sequence = UINT64_MAX;
        TEST_ASSERT_FALSE (
          reinterpret_cast<zlink::msg_t *> (&received)
            ->get_request_reply_metadata (&kind, &sequence));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }

    for (int aggregate = 0; aggregate <= 1; ++aggregate) {
        void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
        void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://pair-later-kind-%d", aggregate);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));
        msleep (SETTLE_TIME);

        zlink_msg_t first;
        init_string_part (&first, "pair-first");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &first, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE));
        zlink_msg_t later;
        init_string_part (&later, "pair-later");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&later)
            ->set_request_reply_metadata (
              zlink::request_reply::reply_type, 322 + aggregate));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &later, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));

        errno = 0;
        int observed_errno = 0;
        if (aggregate) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_recv (receiver, NULL, &parts, &part_count,
                          ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_NULL (parts);
            TEST_ASSERT_EQUAL_UINT64 (0, part_count);
        } else {
            zlink_msg_t received;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            const zlink_routing_id_t *source_rid = NULL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_recv_part (receiver, &source_rid, &received, &has_more,
                               ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&received));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
        }
        TEST_ASSERT_EQUAL_INT (EPROTO, observed_errno);
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_request_reply_preserves_empty_payload_shapes ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "empty-shape-dealer");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://request-reply-empty-shapes"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://request-reply-empty-shapes"));
    msleep (SETTLE_TIME);

    const std::vector<std::vector<std::string> > shapes = {
      std::vector<std::string> (1, std::string ()),
      {std::string (), std::string ("payload")},
      {std::string ("payload"), std::string ()}};
    for (size_t shape_index = 0; shape_index < shapes.size ();
         ++shape_index) {
        const std::vector<std::string> &shape = shapes[shape_index];
        std::vector<zlink_msg_t> request (shape.size ());
        for (size_t i = 0; i < shape.size (); ++i)
            init_bytes_part (&request[i], shape[i]);

        reply_probe_t reply_probe;
        reply_probe.progress_handle = dealer;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_request (
            dealer, request.data (), request.size (), &capture_reply,
            &reply_probe, ZLINK_SEND_FLAGS_NONE, 3000));

        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *received = NULL;
        size_t received_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (router, &source_rid, &request_seq, &received,
                             &received_count, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_TRUE (request_seq != 0);
        const zlink_routing_id_t reply_rid = *source_rid;
        TEST_ASSERT_EQUAL_UINT64 (shape.size (), received_count);
        for (size_t i = 0; i < shape.size (); ++i) {
            TEST_ASSERT_EQUAL_UINT64 (shape[i].size (),
                                      zlink_msg_size (&received[i]));
            if (!shape[i].empty ())
                TEST_ASSERT_EQUAL_MEMORY (
                  shape[i].data (), zlink_msg_data (&received[i]),
                  shape[i].size ());
            unsigned char kind = 0xff;
            uint64_t sequence = UINT64_MAX;
            TEST_ASSERT_FALSE (
              reinterpret_cast<zlink::msg_t *> (&received[i])
                ->get_request_reply_metadata (&kind, &sequence));
        }
        zlink_multipart_close (received, received_count);

        std::vector<zlink_msg_t> reply (shape.size ());
        for (size_t i = 0; i < shape.size (); ++i)
            init_bytes_part (&reply[i], shape[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_router_reply (router, &reply_rid, request_seq, reply.data (),
                              reply.size ()));
        TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
        {
            std::lock_guard<std::mutex> lock (reply_probe.mutex);
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
            TEST_ASSERT_EQUAL_UINT64 (shape.size (), reply_probe.parts.size ());
            for (size_t i = 0; i < shape.size (); ++i)
                TEST_ASSERT_TRUE (shape[i] == reply_probe.parts[i]);
            TEST_ASSERT_FALSE (reply_probe.metadata_present);
        }
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

std::vector<std::string> legacy_request_signature_parts ()
{
    std::vector<std::string> parts;
    parts.push_back (std::string (1, static_cast<char> (0x01)));
    parts.push_back (std::string (1, static_cast<char> (0x01)));
    parts.push_back (std::string (1, static_cast<char> (0x01)));
    const unsigned char sequence_bytes[8] = {0x01, 0x23, 0x45, 0x67,
                                             0x89, 0xab, 0xcd, 0xef};
    parts.push_back (std::string (
      reinterpret_cast<const char *> (sequence_bytes),
      sizeof (sequence_bytes)));
    return parts;
}

void send_raw_parts (void *socket_, const std::vector<std::string> &parts_)
{
    for (size_t i = 0; i < parts_.size (); ++i) {
        zlink_msg_t part;
        init_bytes_part (&part, parts_[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE,
                           i + 1 == parts_.size () ? ZLINK_PART_FINAL
                                                   : ZLINK_PART_MORE));
    }
}

void test_legacy_four_part_signature_remains_ordinary_payload ()
{
    const std::vector<std::string> signature =
      legacy_request_signature_parts ();

    {
        void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
        void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_bind (receiver, "inproc://legacy-signature-dealer"));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (sender, "inproc://legacy-signature-dealer"));
        msleep (SETTLE_TIME);
        send_raw_parts (sender, signature);
        for (size_t i = 0; i < signature.size (); ++i) {
            uint8_t type = 0xff;
            uint64_t token = UINT64_MAX;
            zlink_msg_t part;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
            zlink_part_flag_t more = ZLINK_PART_FINAL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_OK,
              recv_dealer_part_with_retry (
                receiver, &type, &token, &part, &more));
            TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, type);
            TEST_ASSERT_EQUAL_UINT64 (0, token);
            TEST_ASSERT_EQUAL_UINT64 (signature[i].size (),
                                      zlink_msg_size (&part));
            TEST_ASSERT_EQUAL_MEMORY (
              signature[i].data (), zlink_msg_data (&part),
              signature[i].size ());
            TEST_ASSERT_EQUAL_INT (
              i + 1 == signature.size () ? ZLINK_PART_FINAL
                                         : ZLINK_PART_MORE,
              more);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        }
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    set_routing_id_text (dealer, "legacy-signature-router");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://legacy-signature-router"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://legacy-signature-router"));
    msleep (SETTLE_TIME);

    send_raw_parts (dealer, signature);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = UINT64_MAX;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (signature.size (), part_count);
    for (size_t i = 0; i < signature.size (); ++i) {
        TEST_ASSERT_EQUAL_UINT64 (signature[i].size (),
                                  zlink_msg_size (&parts[i]));
        TEST_ASSERT_EQUAL_MEMORY (
          signature[i].data (), zlink_msg_data (&parts[i]),
          signature[i].size ());
    }
    zlink_multipart_close (parts, part_count);

    std::vector<zlink_msg_t> request (signature.size ());
    for (size_t i = 0; i < signature.size (); ++i)
        init_bytes_part (&request[i], signature[i]);
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, request.data (), request.size (),
                            &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));
    parts = NULL;
    part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (signature.size (), part_count);
    for (size_t i = 0; i < signature.size (); ++i)
        TEST_ASSERT_EQUAL_MEMORY (
          signature[i].data (), zlink_msg_data (&parts[i]),
          signature[i].size ());
    zlink_multipart_close (parts, part_count);
    zlink_msg_t reply;
    init_string_part (&reply, "legacy-signature-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply (router, source_rid, request_seq, &reply, 1));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

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
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);
    uint64_t public_mailbox_drains_before = 0;
    handle.socket->test_receive_owner_snapshot (
      NULL, &public_mailbox_drains_before, NULL);
    receive_wait_owner_probe_t wait_probe;
    handle.socket->test_set_receive_wait_hook (
      &capture_receive_wait_owner, &wait_probe);
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

    bool transport_receive_waiting = false;
    const auto receive_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < receive_deadline) {
        uint64_t public_mailbox_drains = 0;
        handle.socket->test_receive_owner_snapshot (
          NULL, &public_mailbox_drains, NULL);
        bool async_wait_entered = false;
        {
            std::lock_guard<std::mutex> lock (wait_probe.mutex);
            async_wait_entered = wait_probe.entered;
        }
        transport_receive_waiting =
          public_mailbox_drains > public_mailbox_drains_before
          || async_wait_entered;
        if (transport_receive_waiting)
            break;
        msleep (1);
    }
    const bool request_state_absent_before_request =
      !zlink::socket_reqrep_internal::find_request_reply_state (handle);

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
    handle.socket->test_set_receive_wait_hook (NULL, NULL);
    send_captured_reply (router, &handler_probe, "reply-after-transition");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));

    TEST_ASSERT_TRUE_MESSAGE (
      transport_receive_waiting,
      "blocking DEALER receive did not enter a transport wait");
    TEST_ASSERT_TRUE_MESSAGE (
      request_state_absent_before_request,
      "blocking DEALER receive eagerly created request/reply state");
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, recv_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_has_more);
    TEST_ASSERT_EQUAL_STRING ("unsolicited-during-transition", received_payload.c_str ());
    if (use_dealer_receive_) {
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, received_message_type);
        TEST_ASSERT_EQUAL_UINT64 (0, received_request_seq);
    } else {
        TEST_ASSERT_TRUE (generic_source_was_null);
    }

    handle = socket_handle_t ();
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

void exercise_request_reply_transport (void *router_,
                                       void *dealer_,
                                       const char *endpoint_,
                                       const char *routing_id_,
                                       const char *request_payload_,
                                       const char *reply_payload_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_, routing_id_, strlen (routing_id_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_, endpoint_));
    msleep (SETTLE_TIME * 50);

    zlink_msg_t request;
    init_string_part (&request, request_payload_);
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer_;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer_, &request, 1, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 5000));

    request_handler_probe_t received;
    recv_router_request_into_probe (router_, &received);
    {
        std::lock_guard<std::mutex> lock (received.mutex);
        TEST_ASSERT_TRUE (received.request_seq != 0);
        TEST_ASSERT_EQUAL_STRING (routing_id_, received.peer_rid.c_str ());
        TEST_ASSERT_EQUAL_STRING (request_payload_,
                                  received.request_payload.c_str ());
        TEST_ASSERT_FALSE (received.metadata_present);
    }
    send_captured_reply (router_, &received, reply_payload_);
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING (reply_payload_, reply_probe.payload.c_str ());
        TEST_ASSERT_FALSE (reply_probe.metadata_present);
    }
}

void test_dealer_to_router_request_reply_over_ipc ()
{
#if defined ZLINK_HAVE_IPC
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "ipc://*", endpoint, sizeof (endpoint));
    exercise_request_reply_transport (
      router, dealer, endpoint, "dealer-ipc", "request-ipc", "reply-ipc");
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
#else
    TEST_IGNORE_MESSAGE ("IPC not available");
#endif
}

void test_dealer_to_router_request_reply_over_tls ()
{
#if defined ZLINK_HAVE_TLS
    const tls_test_files_t files = make_tls_test_files ();
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const int trust_system = 0;
    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system,
                        sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_TLS_CERT,
                        files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_TLS_KEY,
                        files.server_key.c_str (), files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (),
                        files.ca_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_TLS_HOSTNAME, hostname,
                        strlen (hostname)));

    char endpoint[MAX_SOCKET_STRING];
    test_bind (router, "tls://127.0.0.1:*", endpoint, sizeof (endpoint));
    exercise_request_reply_transport (
      router, dealer, endpoint, "dealer-tls", "request-tls", "reply-tls");
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
    cleanup_tls_test_files (files);
#else
    TEST_IGNORE_MESSAGE ("TLS not available");
#endif
}

//  A completion lane is exempt from application HWM admission. Exercise a
//  sustained TCP burst without depending on incidental kernel-buffer
//  backpressure, while still proving that any transient retry completes and
//  preserves the payload and connection identity.
namespace burst_completion
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

void test_router_reply_burst_completion_remains_correct_over_tcp ()
{
    using namespace burst_completion;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    const char dealer_rid[] = "burst-dealer";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, dealer_rid, sizeof (dealer_rid) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 10);

    //  The completion poller is registered before the first request.
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_poller_add (poller, dealer, NULL, ZLINK_POLLCOMPLETION));

    probe_t probe;
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

            //  The reply reuses the payload received on the application
            //  connection. Its connection ID must be rewritten for the
            //  completion lane or the requester will discard the reply.
            zlink_routing_id_t reply_rid = *peer_rid;
            zlink_submit_result_t rc = zlink_router_reply_part (
              router, &reply_rid, request_seq, &parts[0], ZLINK_PART_FINAL);
            zlink_multipart_close (parts, part_count);

            //  The submit entry consumes the message either way. A transient
            //  retry therefore rebuilds an equivalent payload.
            while (rc != ZLINK_SUBMIT_OK) {
                //  Only transient backpressure is retryable here.
                TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, rc);
                zlink_poller_event_t event;
                (void) zlink_poller_wait (poller, &event, 1, 10, NULL);
                TEST_ASSERT_FALSE_MESSAGE (
                  deadline_passed (test_deadline),
                  "router reply did not recover during the completion burst");

                zlink_msg_t retry_part;
                fill_payload (&retry_part, ordinal_of_request (cycle, i));
                rc = zlink_router_reply_part (router, &reply_rid, request_seq, &retry_part,
                                              ZLINK_PART_FINAL);
            }
        }

        //  Drain this cycle before the next burst, keeping each cycle's
        //  completion and payload checks bounded.
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

    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    const std::chrono::steady_clock::time_point pair_ready_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!router_handle.socket->test_pair_is_ready (
      target.transport_pair_id, target.transport_pair_generation)) {
        TEST_ASSERT_TRUE_MESSAGE (
          std::chrono::steady_clock::now () < pair_ready_deadline,
          "ROUTER transport pair did not become ready");
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

    async_input_command_probe_t command_probe;
    router_handle.socket->test_set_receive_command_sync_probe_hook (
      &capture_async_input_command, &command_probe);
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

    const bool activate_read_seen =
      wait_for_async_input_command (&command_probe, 3000);

    uint64_t epoch_after = 0;
    uint64_t public_drains_after = 0;
    uint64_t async_drains_after = 0;
    router_handle.socket->test_receive_owner_snapshot (
      &epoch_after, &public_drains_after, &async_drains_after);
    router_handle.socket->test_set_receive_command_sync_probe_hook (NULL,
                                                                    NULL);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, send_result);
    TEST_ASSERT_EQUAL_INT (1, poll_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, event.source_kind);
    TEST_ASSERT_EQUAL_PTR (router, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);
    TEST_ASSERT_TRUE_MESSAGE (
      poll_elapsed_ms >= 0 && poll_elapsed_ms < 1000,
      "socket poller slept until its timeout after async input progress");
    TEST_ASSERT_TRUE_MESSAGE (
      activate_read_seen,
      "input activate_read command did not reach the async mailbox owner");
    TEST_ASSERT_TRUE_MESSAGE (
      async_drains_after > async_drains_before,
      "async mailbox owner did not process the input activation");
    TEST_ASSERT_EQUAL_UINT64 (public_drains_before, public_drains_after);
    TEST_ASSERT_TRUE_MESSAGE (
      epoch_after > epoch_before,
      "async mailbox owner did not publish receive progress");

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
        int completion_errno = -1;
        while (true) {
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                if (probe.done)
                    break;
            }
            zlink_poller_event_t event;
            errno = 0;
            TEST_ASSERT_TRUE (
              zlink_poller_wait (poller, &event, 1, 25, NULL) >= 0);
            const int wait_errno = zlink_errno ();
            {
                std::lock_guard<std::mutex> lock (probe.mutex);
                if (probe.done)
                    completion_errno = wait_errno;
            }
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
        TEST_ASSERT_EQUAL_INT_MESSAGE (
          0, completion_errno,
          "callback-owned reply parts were touched again after callback return");

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

    close_test_socket_after_reply_callback (dealer);
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

    pending_request_t expected;
    expected.identity.request_seq = 77;
    expected.identity.cookie = 7001;
    expected.transport_pair_id = 101;
    expected.transport_pair_generation = 9;
    expected.handler = NULL;
    expected.userdata = NULL;
    const pending_request_identity_t expected_identity = expected.identity;
    add_socket_pending_request_locked (&state, std::move (expected));

    pending_request_t taken;
    //  The peer may settle on a routing ID different from the requested
    //  intent. The per-socket sequence identifies the pending request while
    //  the transport pair still fences stale connections.
    TEST_ASSERT_TRUE (take_pending_reply_from_transport_locked (
      &state, expected_identity.request_seq, 101, 9, &taken));
    TEST_ASSERT_EQUAL_UINT64 (expected_identity.cookie,
                              taken.identity.cookie);
    TEST_ASSERT_TRUE (state.pending_requests.empty ());

    pending_request_t reused;
    reused.identity = expected_identity;
    reused.transport_pair_id = 101;
    reused.transport_pair_generation = 9;
    add_socket_pending_request_locked (&state, std::move (reused));
    TEST_ASSERT_FALSE (take_pending_reply_from_transport_locked (
      &state, expected_identity.request_seq, 202, 9, &taken));
    TEST_ASSERT_FALSE (take_pending_reply_from_transport_locked (
      &state, expected_identity.request_seq, 101, 10, &taken));
    TEST_ASSERT_TRUE (take_pending_reply_from_transport_locked (
      &state, expected_identity.request_seq, 101, 9, &taken));
    TEST_ASSERT_EQUAL_UINT64 (77, taken.identity.request_seq);
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
    zlink::socket_reqrep_internal::pending_request_t pending;
    pending.identity.request_seq = 1;
    pending.identity.cookie = 1;
    pending.transport_pair_id = 0;
    pending.transport_pair_generation = 0;
    pending.handler = &capture_reply;
    pending.userdata = NULL;
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        zlink::socket_reqrep_internal::add_socket_pending_request_locked (
          state.get (), std::move (pending));
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

    zlink_recv_result_t receive_head_result = ZLINK_RECV_INTERNAL_ERROR;
    zlink_recv_result_t receive_tail_result = ZLINK_RECV_INTERNAL_ERROR;
    zlink_submit_result_t reply_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    uint8_t received_message_type = 0;
    uint64_t received_request_seq = 0;
    zlink_part_flag_t received_head_has_more = ZLINK_PART_FINAL;
    zlink_part_flag_t received_tail_has_more = ZLINK_PART_MORE;
    std::string received_head_payload;
    std::string received_tail_payload;
    std::thread server ([&] () {
        zlink_msg_t received;
        zlink_msg_init (&received);
        receive_head_result = zlink_dealer_recv_part (
          dealer, &received_message_type, &received_request_seq, &received,
          &received_head_has_more, static_cast<zlink_recv_flags_t> (0));
        if (receive_head_result != ZLINK_RECV_OK) {
            zlink_msg_close (&received);
            return;
        }
        received_head_payload.assign (
          static_cast<const char *> (zlink_msg_data (&received)),
          zlink_msg_size (&received));
        zlink_msg_close (&received);

        zlink_msg_init (&received);
        receive_tail_result = zlink_dealer_recv_part (
          dealer, &received_message_type, &received_request_seq, &received,
          &received_tail_has_more, static_cast<zlink_recv_flags_t> (0));
        if (receive_tail_result != ZLINK_RECV_OK) {
            zlink_msg_close (&received);
            return;
        }
        received_tail_payload.assign (
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
    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      dealer_request_state =
        zlink::socket_reqrep_internal::find_request_reply_state (dealer_handle);

    completion_owner_probe_t probe;
    zlink_msg_t request_head;
    init_string_part (&request_head, "exact-request-head");
    const zlink_submit_result_t request_head_result =
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &request_head,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE, 0, NULL, NULL);
    zlink_msg_t request_tail;
    init_string_part (&request_tail, "exact-request-tail");
    const zlink_submit_result_t request_tail_result =
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &request_tail,
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
    TEST_ASSERT_NULL (dealer_request_state.get ());
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request_head_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, request_tail_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, receive_head_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, receive_tail_result);
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST,
                             received_message_type);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, received_head_has_more);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_tail_has_more);
    TEST_ASSERT_EQUAL_STRING ("exact-request-head",
                              received_head_payload.c_str ());
    TEST_ASSERT_EQUAL_STRING ("exact-request-tail",
                              received_tail_payload.c_str ());
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

void test_same_rid_handover_colliding_wire_sequences_keep_exact_reply_targets ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer_a);
    TEST_ASSERT_NOT_NULL (dealer_b);

    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
                        sizeof (handover)));
    set_routing_id_text (dealer_a, "duplicate-peer");
    set_routing_id_text (dealer_b, "duplicate-peer");

    const char endpoint[] = "inproc://zmp-router-same-rid-sequence-collision";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_a, endpoint));
    msleep (SETTLE_TIME);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_b, endpoint));
    // Let HANDOVER retain dealer A as standby before both physical peers
    // allocate their first (wire sequence 1) request.
    msleep (SETTLE_TIME * 2);

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = dealer_a;
    reply_b.progress_handle = dealer_b;
    zlink_msg_t request_a;
    zlink_msg_t request_b;
    init_string_part (&request_a, "from-a");
    init_string_part (&request_b, "from-b");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer_a, &request_a, 1, &capture_reply,
                            &reply_a, ZLINK_SEND_FLAGS_NONE, 3000));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer_b, &request_b, 1, &capture_reply,
                            &reply_b, ZLINK_SEND_FLAGS_NONE, 3000));

    multi_request_probe_t received;
    recv_router_request_into_event (router, &received);
    recv_router_request_into_event (router, &received);

    request_event_t from_a;
    request_event_t from_b;
    request_event_t first_received;
    request_event_t second_received;
    {
        std::lock_guard<std::mutex> lock (received.mutex);
        TEST_ASSERT_EQUAL_UINT64 (2, received.events.size ());
        first_received = received.events[0];
        second_received = received.events[1];
        for (size_t i = 0; i < received.events.size (); ++i) {
            if (received.events[i].request_payload == "from-a")
                from_a = received.events[i];
            else if (received.events[i].request_payload == "from-b")
                from_b = received.events[i];
            else
                TEST_FAIL_MESSAGE ("unexpected duplicate-peer request payload");
        }
    }
    TEST_ASSERT_TRUE (from_a.request_seq != 0);
    TEST_ASSERT_TRUE (from_b.request_seq != 0);
    TEST_ASSERT_TRUE (from_a.request_seq != from_b.request_seq);
    TEST_ASSERT_TRUE (from_a.request_seq == 1 || from_b.request_seq == 1);
    TEST_ASSERT_EQUAL_STRING ("duplicate-peer", from_a.peer_rid.c_str ());
    TEST_ASSERT_EQUAL_STRING ("duplicate-peer", from_b.peer_rid.c_str ());

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (router));
    TEST_ASSERT_NOT_NULL (state.get ());
    {
        zlink::socket_reqrep_internal::pending_key_t key_a;
        key_a.peer_rid = from_a.peer_rid;
        key_a.request_seq = from_a.request_seq;
        zlink::socket_reqrep_internal::pending_key_t key_b;
        key_b.peer_rid = from_b.peer_rid;
        key_b.request_seq = from_b.request_seq;

        std::lock_guard<std::mutex> lock (state->mutex);
        const auto target_a = state->router_reply_targets.find (key_a);
        const auto target_b = state->router_reply_targets.find (key_b);
        TEST_ASSERT_TRUE (target_a != state->router_reply_targets.end ());
        TEST_ASSERT_TRUE (target_b != state->router_reply_targets.end ());
        TEST_ASSERT_EQUAL_UINT64 (1, target_a->second.wire_request_seq);
        TEST_ASSERT_EQUAL_UINT64 (1, target_b->second.wire_request_seq);
        TEST_ASSERT_TRUE (target_a->second.pipe != target_b->second.pipe);
        TEST_ASSERT_TRUE (
          target_a->second.transport_pair_id
            != target_b->second.transport_pair_id
          || target_a->second.transport_pair_generation
               != target_b->second.transport_pair_generation);
        TEST_ASSERT_EQUAL_UINT64 (1, state->router_reply_aliases.size ());
    }

    // Reply in reverse receive order. RID and wire sequence are identical,
    // so only the opaque token can select the exact source pipe.
    send_router_reply_to_event (
      router, second_received,
      second_received.request_payload == "from-a" ? "reply-a" : "reply-b");
    send_router_reply_to_event (
      router, first_received,
      first_received.request_payload == "from-a" ? "reply-a" : "reply-b");
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->router_reply_targets.empty ());
        TEST_ASSERT_TRUE (state->router_reply_aliases.empty ());
    }
    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));
    {
        std::lock_guard<std::mutex> lock (reply_a.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_a.result);
        TEST_ASSERT_EQUAL_STRING ("reply-a", reply_a.payload.c_str ());
    }
    {
        std::lock_guard<std::mutex> lock (reply_b.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_b.result);
        TEST_ASSERT_EQUAL_STRING ("reply-b", reply_b.payload.c_str ());
    }

    close_test_socket_after_reply_callback (dealer_a);
    close_test_socket_after_reply_callback (dealer_b);
    test_context_socket_close_zero_linger (router);
}

void test_same_physical_peer_duplicate_wire_sequence_is_protocol_error ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer_a);
    TEST_ASSERT_NOT_NULL (dealer_b);

    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
                        sizeof (handover)));
    set_routing_id_text (dealer_a, "duplicate-sequence-peer");
    set_routing_id_text (dealer_b, "duplicate-sequence-peer");

    const char endpoint[] = "inproc://zmp-router-same-source-sequence-duplicate";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_a, endpoint));
    msleep (SETTLE_TIME);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_b, endpoint));
    msleep (SETTLE_TIME * 2);

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = dealer_a;
    reply_b.progress_handle = dealer_b;
    zlink_msg_t request_a;
    zlink_msg_t request_b;
    init_string_part (&request_a, "from-a");
    init_string_part (&request_b, "from-b");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer_a, &request_a, 1, &capture_reply,
                            &reply_a, ZLINK_SEND_FLAGS_NONE, 3000));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer_b, &request_b, 1, &capture_reply,
                            &reply_b, ZLINK_SEND_FLAGS_NONE, 3000));

    multi_request_probe_t received;
    recv_router_request_into_event (router, &received);
    recv_router_request_into_event (router, &received);
    request_event_t from_a;
    request_event_t from_b;
    {
        std::lock_guard<std::mutex> lock (received.mutex);
        TEST_ASSERT_EQUAL_UINT64 (2, received.events.size ());
        for (size_t i = 0; i < received.events.size (); ++i) {
            if (received.events[i].request_payload == "from-a")
                from_a = received.events[i];
            else if (received.events[i].request_payload == "from-b")
                from_b = received.events[i];
            else
                TEST_FAIL_MESSAGE ("unexpected same-source duplicate payload");
        }
    }

    const request_event_t natural =
      from_a.request_seq == 1 ? from_a : from_b;
    const request_event_t alias =
      from_a.request_seq == 1 ? from_b : from_a;
    void *const alias_dealer =
      alias.request_payload == "from-a" ? dealer_a : dealer_b;
    reply_probe_t *const natural_reply =
      natural.request_payload == "from-a" ? &reply_a : &reply_b;

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (router));
    TEST_ASSERT_NOT_NULL (state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (2, state->router_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, state->router_reply_aliases.size ());
    }

    send_router_reply_to_event (router, natural, "natural-reply");
    TEST_ASSERT_TRUE (wait_for_reply (natural_reply));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, state->router_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, state->router_reply_aliases.size ());
    }

    // The natural token is now free, but the same physical source still owns
    // an aliased wire sequence 1. Reusing it must not bypass duplicate
    // detection merely because the natural primary-map key disappeared.
    send_internal_request_message (alias_dealer, 1);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_token = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_router_recv (router, &source_rid, &request_token, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);

    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (3, state->router_next_reply_token);
        TEST_ASSERT_TRUE (state->router_reply_targets.size () <= 1);
        TEST_ASSERT_TRUE (state->router_reply_aliases.size () <= 1);
    }

    test_context_socket_close_zero_linger (dealer_a);
    test_context_socket_close_zero_linger (dealer_b);
    test_context_socket_close_zero_linger (router);
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

void test_extra_reply_is_rejected_after_first_completion ()
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
    zlink_msg_t extra_reply;
    zlink_msg_init (&extra_reply);
    init_string_part (&extra_reply, "reply-second");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_router_reply (server_router, &handler_probe.peer_rid_value,
                          handler_probe.request_seq, &extra_reply, 1));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&extra_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&extra_reply));
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

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      server_reply_state =
        zlink::socket_reqrep_internal::find_request_reply_state (
          as_socket_handle (server_dealer));
    TEST_ASSERT_NOT_NULL (server_reply_state.get ());
    {
        std::lock_guard<std::mutex> lock (server_reply_state->mutex);
        const std::unordered_map<
          uint64_t, zlink::socket_reqrep_internal::dealer_reply_target_t>::const_iterator
          target_a = server_reply_state->dealer_reply_targets.find (seq_a);
        const std::unordered_map<
          uint64_t, zlink::socket_reqrep_internal::dealer_reply_target_t>::const_iterator
          target_b = server_reply_state->dealer_reply_targets.find (seq_b);
        TEST_ASSERT_TRUE (target_a
                          != server_reply_state->dealer_reply_targets.end ());
        TEST_ASSERT_TRUE (target_b
                          != server_reply_state->dealer_reply_targets.end ());
        TEST_ASSERT_TRUE (target_a->second.request_seq != 0);
        TEST_ASSERT_EQUAL_UINT64 (target_a->second.request_seq,
                                  target_b->second.request_seq);
        TEST_ASSERT_TRUE (target_a->second.pipe != target_b->second.pipe);
    }

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

void test_dealer_multipart_request_preserves_pending_cookie ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_dealer, "inproc://zmp-dealer-request-cookie"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_dealer, "inproc://zmp-dealer-request-cookie"));
    msleep (SETTLE_TIME);

    zlink_msg_t first;
    zlink_msg_init (&first);
    init_string_part (&first, "request-head");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (client_dealer, &first, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_MORE, 0, NULL, NULL));

    socket_handle_t client_handle = as_socket_handle (client_dealer);
    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (client_handle.socket);
    TEST_ASSERT_NOT_NULL (helper_state.get ());
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        TEST_ASSERT_TRUE (helper_state->send.active);
        TEST_ASSERT_EQUAL_UINT64 (0, helper_state->send.spec.request_seq);
        TEST_ASSERT_EQUAL_UINT64 (0, helper_state->send.spec.pending_cookie);
    }

    zlink_msg_t final;
    zlink_msg_init (&final);
    init_string_part (&final, "request-tail");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = client_dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (client_dealer, &final, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &reply_probe));
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        TEST_ASSERT_FALSE (helper_state->send.active);
    }

    uint8_t message_type = 0;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t received;
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server_dealer, &message_type, &request_seq,
                                   &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_STRING ("request-head",
                              part_to_string_and_close (&received).c_str ());

    const uint64_t first_request_seq = request_seq;
    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server_dealer, &message_type, &request_seq,
                                   &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_EQUAL_UINT64 (first_request_seq, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("request-tail",
                              part_to_string_and_close (&received).c_str ());

    zlink_msg_t reply;
    zlink_msg_init (&reply);
    init_string_part (&reply, "cookie-ok");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server_dealer, request_seq, &reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_STRING ("cookie-ok", reply_probe.payload.c_str ());
    }

    client_handle = socket_handle_t ();
    helper_state.reset ();
    close_test_socket_after_reply_callback (client_dealer);
    test_context_socket_close_zero_linger (server_dealer);
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

void test_grouped_request_is_consumed_before_pending_registration ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    zlink_msg_t single;
    init_string_part (&single, "grouped-single");
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&single)->set_group ("request-group"));
    reply_probe_t probe;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_dealer_request_part (
        dealer, &single, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 1000,
        &capture_reply, &probe));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&single));
    TEST_ASSERT_EQUAL_STRING (
      "", reinterpret_cast<zlink::msg_t *> (&single)->group ());
    unsigned char retained_kind = 0xff;
    uint64_t retained_sequence = std::numeric_limits<uint64_t>::max ();
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&single)
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    TEST_ASSERT_FALSE (as_socket_handle (dealer).socket->has_request_reply_state ());
    TEST_ASSERT_FALSE (
      zlink::part_helper_internal::send_sequence_active (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&single));

    zlink_msg_t staged_first;
    init_string_part (&staged_first, "grouped-staged-first");
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&staged_first)
        ->set_group ("request-group"));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_dealer_request_part (
        dealer, &staged_first, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, 0,
        NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&staged_first));
    TEST_ASSERT_EQUAL_STRING (
      "", reinterpret_cast<zlink::msg_t *> (&staged_first)->group ());
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&staged_first)
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    TEST_ASSERT_FALSE (as_socket_handle (dealer).socket->has_request_reply_state ());
    TEST_ASSERT_FALSE (
      zlink::part_helper_internal::send_sequence_active (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&staged_first));

    test_context_socket_close_zero_linger (dealer);
}

void test_runtime_receive_spill_failure_discards_record_tail ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *router_sender = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (router_sender);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-recv-spill-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (router_sender, "inproc://zmp-router-recv-spill-oom"));
    msleep (SETTLE_TIME);

    const char router_spill[] = "router-spill";
    for (size_t i = 0; i < 10; ++i) {
        send_raw_request_frame (
          router_sender, router_spill, strlen (router_spill),
          i + 1 == 10 ? ZLINK_PART_FINAL : ZLINK_PART_MORE);
    }
    const char router_after[] = "router-after-spill";
    send_raw_request_frame (router_sender, router_after,
                            strlen (router_after), ZLINK_PART_FINAL);

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_receive_spill);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING (router_after, msg_to_string (&parts[0]).c_str ());
    zlink_multipart_close (parts, part_count);

    void *dealer_receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_sender = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer_receiver);
    TEST_ASSERT_NOT_NULL (dealer_sender);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (dealer_receiver, "inproc://zmp-dealer-recv-spill-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_sender, "inproc://zmp-dealer-recv-spill-oom"));
    msleep (SETTLE_TIME);

    const char dealer_spill[] = "dealer-spill";
    for (size_t i = 0; i < 10; ++i) {
        send_raw_request_frame (
          dealer_sender, dealer_spill, strlen (dealer_spill),
          i + 1 == 10 ? ZLINK_PART_FINAL : ZLINK_PART_MORE);
    }
    const char dealer_after[] = "dealer-after-spill";
    send_raw_request_frame (dealer_sender, dealer_after,
                            strlen (dealer_after), ZLINK_PART_FINAL);

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_receive_spill);
    uint8_t message_type = 0xff;
    request_seq = std::numeric_limits<uint64_t>::max ();
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    zlink_msg_init (&received);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_dealer_recv_part (dealer_receiver, &message_type, &request_seq,
                              &received, &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (dealer_receiver, &message_type,
                                   &request_seq, &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, message_type);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING (dealer_after,
                              part_to_string_and_close (&received).c_str ());

    test_context_socket_close_zero_linger (dealer_sender);
    test_context_socket_close_zero_linger (dealer_receiver);
    test_context_socket_close_zero_linger (router_sender);
    test_context_socket_close_zero_linger (router);
}

void test_router_lazy_state_failure_discards_record_tail ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-lazy-state-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-lazy-state-oom"));
    msleep (SETTLE_TIME);

    // Consume the ROUTER's initial routing-id frame before taking the queue
    // accounting baseline. Ordinary data must not create lazy request state.
    send_raw_request_frame (dealer, "warmup", 6, ZLINK_PART_FINAL);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t part;
    zlink_msg_init (&part);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("warmup", msg_to_string (&part).c_str ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

    const uint64_t accounting_baseline =
      read_request_reply_hwm_snapshot ().current_accounted_bytes;
    {
        const socket_handle_t router_handle = as_socket_handle (router);
        TEST_ASSERT_NOT_NULL (router_handle.socket);
        TEST_ASSERT_FALSE (router_handle.socket->has_request_reply_state ());
    }

    // A consumed terminal LMSG must be closed even though admission failed.
    std::vector<unsigned char> failed_single_payload (1024, 's');
    std::atomic<int> failed_single_release_count (0);
    zlink_msg_t failed_single;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (
      &failed_single, &failed_single_payload[0], failed_single_payload.size (),
      &count_reply_payload_free, &failed_single_release_count));
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&failed_single)
        ->set_request_reply_metadata (
          zlink::request_reply::request_type, 77));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &failed_single, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed_single));
    send_raw_request_frame (dealer, "record-after-single-oom", 23,
                            ZLINK_PART_FINAL);
    TEST_ASSERT_TRUE (
      read_request_reply_hwm_snapshot ().current_accounted_bytes
      > accounting_baseline);

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_lazy_state_create);
    zlink_msg_init (&part);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_TRUE (wait_for_free_count (failed_single_release_count, 1));

    zlink_msg_init (&part);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("record-after-single-oom",
                              msg_to_string (&part).c_str ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_EQUAL_UINT64 (
      accounting_baseline,
      read_request_reply_hwm_snapshot ().current_accounted_bytes);

    // A multipart rejection closes both the initially consumed LMSG and the
    // final tail while leaving the next record available on the same source.
    std::vector<unsigned char> failed_head_payload (1024, 'h');
    std::vector<unsigned char> failed_tail_payload (1536, 't');
    std::atomic<int> failed_multipart_release_count (0);
    zlink_msg_t failed_head;
    zlink_msg_t failed_tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (
      &failed_head, &failed_head_payload[0], failed_head_payload.size (),
      &count_reply_payload_free, &failed_multipart_release_count));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (
      &failed_tail, &failed_tail_payload[0], failed_tail_payload.size (),
      &count_reply_payload_free, &failed_multipart_release_count));
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&failed_head)
        ->set_request_reply_metadata (
          zlink::request_reply::request_type, 78));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &failed_head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &failed_tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed_head));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&failed_tail));
    send_raw_request_frame (dealer, "record-after-multipart-oom", 26,
                            ZLINK_PART_FINAL);
    TEST_ASSERT_TRUE (
      read_request_reply_hwm_snapshot ().current_accounted_bytes
      > accounting_baseline);

    {
        const socket_handle_t router_handle = as_socket_handle (router);
        TEST_ASSERT_NOT_NULL (router_handle.socket);
        TEST_ASSERT_FALSE (router_handle.socket->has_request_reply_state ());
    }
    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_lazy_state_create);
    zlink_msg_init (&part);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_TRUE (
      wait_for_free_count (failed_multipart_release_count, 2));

    zlink_msg_init (&part);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("record-after-multipart-oom",
                              msg_to_string (&part).c_str ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_EQUAL_UINT64 (
      accounting_baseline,
      read_request_reply_hwm_snapshot ().current_accounted_bytes);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_receive_export_failure_rolls_back_reply_target ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-recv-export-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-recv-export-oom"));
    msleep (SETTLE_TIME);

    reply_probe_t failed_probe;
    failed_probe.progress_handle = dealer;
    zlink_msg_t failed_request[2];
    zlink_msg_init (&failed_request[0]);
    zlink_msg_init (&failed_request[1]);
    init_string_part (&failed_request[0], "router-export-failed-head");
    init_string_part (&failed_request[1], "router-export-failed-tail");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, failed_request, 2, &capture_reply,
                            &failed_probe, ZLINK_SEND_FLAGS_NONE, 100));

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_payload_export);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);

    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      router_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (router));
    TEST_ASSERT_NOT_NULL (router_state.get ());
    {
        std::lock_guard<std::mutex> lock (router_state->mutex);
        TEST_ASSERT_TRUE (router_state->router_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, router_state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, router_state->reply_target_reservations);
        TEST_ASSERT_EQUAL_UINT64 (0, router_state->reply_target_checkouts);
    }

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "router-export-retry");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    zlink_routing_id_t reply_rid = *source_rid;
    zlink_multipart_close (parts, part_count);
    {
        std::lock_guard<std::mutex> lock (router_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, router_state->router_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, router_state->reply_target_slots);
    }

    zlink_msg_t reply;
    zlink_msg_init (&reply);
    init_string_part (&reply, "router-export-retry-ok");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply (router, &reply_rid, request_seq, &reply, 1));
    {
        std::lock_guard<std::mutex> lock (router_state->mutex);
        TEST_ASSERT_TRUE (router_state->router_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, router_state->reply_target_slots);
    }
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    TEST_ASSERT_TRUE (wait_for_reply (&failed_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("router-export-retry-ok",
                                  reply_probe.payload.c_str ());
    }
    {
        std::lock_guard<std::mutex> lock (failed_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, failed_probe.result);
    }

    router_state.reset ();
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_receive_export_failure_rolls_back_reply_target ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server, "inproc://zmp-dealer-recv-export-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client, "inproc://zmp-dealer-recv-export-oom"));
    msleep (SETTLE_TIME);

    reply_probe_t failed_probe;
    failed_probe.progress_handle = client;
    zlink_msg_t failed_request[2];
    zlink_msg_init (&failed_request[0]);
    zlink_msg_init (&failed_request[1]);
    init_string_part (&failed_request[0], "dealer-export-failed-head");
    init_string_part (&failed_request[1], "dealer-export-failed-tail");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (client, failed_request, 2, &capture_reply,
                            &failed_probe, ZLINK_SEND_FLAGS_NONE, 100));

    zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
      zlink::socket_reqrep_internal::request_reply_allocation_payload_export);
    uint8_t message_type = 0xff;
    uint64_t request_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    zlink_msg_init (&received);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_dealer_recv_part (server, &message_type, &request_token, &received,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      server_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (server));
    TEST_ASSERT_NOT_NULL (server_state.get ());
    {
        std::lock_guard<std::mutex> lock (server_state->mutex);
        TEST_ASSERT_TRUE (server_state->dealer_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_reservations);
        TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_checkouts);
    }

    reply_probe_t reply_probe;
    reply_probe.progress_handle = client;
    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "dealer-export-retry");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (client, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));

    zlink_msg_init (&received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server, &message_type, &request_token,
                                   &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING ("dealer-export-retry",
                              part_to_string_and_close (&received).c_str ());
    {
        std::lock_guard<std::mutex> lock (server_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, server_state->dealer_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, server_state->reply_target_slots);
    }

    zlink_msg_t reply;
    zlink_msg_init (&reply);
    init_string_part (&reply, "dealer-export-retry-ok");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server, request_token, &reply,
                               ZLINK_PART_FINAL));
    {
        std::lock_guard<std::mutex> lock (server_state->mutex);
        TEST_ASSERT_TRUE (server_state->dealer_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_slots);
    }
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    TEST_ASSERT_TRUE (wait_for_reply (&failed_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("dealer-export-retry-ok",
                                  reply_probe.payload.c_str ());
    }
    {
        std::lock_guard<std::mutex> lock (failed_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, failed_probe.result);
    }

    server_state.reset ();
    close_test_socket_after_reply_callback (client);
    test_context_socket_close_zero_linger (server);
}

void test_router_part_stage_failure_revokes_published_reply_target ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-part-stage-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-part-stage-oom"));
    msleep (SETTLE_TIME);

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        send_internal_request_multipart_message (dealer, 100 + sequence, 3);
        zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
          zlink::socket_reqrep_internal::request_reply_allocation_receive_part_stage);

        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_INTERNAL_ERROR,
          zlink_router_recv_part (router, &source_rid, &request_seq, &part,
                                  &has_more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

        const std::shared_ptr<
          zlink::socket_reqrep_internal::socket_request_reply_state_t>
          state = zlink::socket_reqrep_internal::find_request_reply_state (
            as_socket_handle (router));
        TEST_ASSERT_NOT_NULL (state.get ());
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->router_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_reservations);
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_checkouts);
    }

    zlink_msg_t request;
    init_string_part (&request, "request-after-router-stage-oom");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));
    request_handler_probe_t received;
    recv_router_request_into_probe (router, &received);
    send_captured_reply (router, &received, "router-stage-recovered");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("router-stage-recovered",
                                  reply_probe.payload.c_str ());
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_part_stage_failure_revokes_published_reply_token ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server, "inproc://zmp-dealer-part-stage-oom"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client, "inproc://zmp-dealer-part-stage-oom"));
    msleep (SETTLE_TIME);

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        send_internal_request_multipart_message (client, 200 + sequence, 3);
        zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
          zlink::socket_reqrep_internal::request_reply_allocation_receive_part_stage);

        uint8_t message_type = 0xff;
        uint64_t request_token = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_INTERNAL_ERROR,
          zlink_dealer_recv_part (server, &message_type, &request_token,
                                  &part, &has_more,
                                  ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

        const std::shared_ptr<
          zlink::socket_reqrep_internal::socket_request_reply_state_t>
          state = zlink::socket_reqrep_internal::find_request_reply_state (
            as_socket_handle (server));
        TEST_ASSERT_NOT_NULL (state.get ());
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->dealer_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_reservations);
        TEST_ASSERT_EQUAL_UINT64 (0, state->reply_target_checkouts);
    }

    zlink_msg_t request;
    init_string_part (&request, "request-after-dealer-stage-oom");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = client;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (client, &request, 1, &capture_reply, &reply_probe,
                            ZLINK_SEND_FLAGS_NONE, 3000));

    uint8_t message_type = 0xff;
    uint64_t request_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server, &message_type, &request_token,
                                   &received, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING (
      "request-after-dealer-stage-oom",
      part_to_string_and_close (&received).c_str ());

    zlink_msg_t reply;
    init_string_part (&reply, "dealer-stage-recovered");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server, request_token, &reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("dealer-stage-recovered",
                                  reply_probe.payload.c_str ());
    }

    close_test_socket_after_reply_callback (client);
    test_context_socket_close_zero_linger (server);
}

void test_dealer_staged_reply_completes_without_combined_buffer ()
{
    void *server_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server_dealer);
    TEST_ASSERT_NOT_NULL (client_dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server_dealer, "inproc://zmp-dealer-staged-reply"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client_dealer, "inproc://zmp-dealer-staged-reply"));
    msleep (SETTLE_TIME);

    zlink_msg_t request;
    zlink_msg_init (&request);
    init_string_part (&request, "dealer-staged-request");
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
    TEST_ASSERT_EQUAL_STRING ("dealer-staged-request",
                              part_to_string_and_close (&received).c_str ());

    zlink_msg_t staged[4];
    for (size_t i = 0; i < 4; ++i) {
        zlink_msg_init (&staged[i]);
        init_string_part (&staged[i], "staged-reply");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_reply_part (server_dealer, request_seq, &staged[i],
                                   ZLINK_PART_MORE));
    }

    zlink_msg_t final_part;
    zlink_msg_init (&final_part);
    init_string_part (&final_part, "staged-final");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server_dealer, request_seq, &final_part,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&final_part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&final_part));
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&staged[i]));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&staged[i]));
    }

    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      server_reply_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (server_dealer));
    TEST_ASSERT_NOT_NULL (server_reply_state.get ());
    {
        std::lock_guard<std::mutex> lock (server_reply_state->mutex);
        TEST_ASSERT_TRUE (server_reply_state->dealer_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, server_reply_state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, server_reply_state->reply_target_checkouts);
    }
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.callback_count);
        TEST_ASSERT_EQUAL_UINT64 (5, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING ("staged-reply",
                                  reply_probe.payload.c_str ());
    }

    msleep (SETTLE_TIME);
    server_reply_state.reset ();
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

    std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      router_reply_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (router));
    TEST_ASSERT_NOT_NULL (router_reply_state.get ());
    {
        std::lock_guard<std::mutex> lock (router_reply_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, router_reply_state->router_reply_targets.size ());
        TEST_ASSERT_FALSE (router_reply_state->router_reply_targets.begin ()->second.checked_out);
        TEST_ASSERT_EQUAL_UINT64 (1, router_reply_state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, router_reply_state->reply_target_checkouts);
    }

    zlink_msg_t fresh_reply;
    zlink_msg_init (&fresh_reply);
    init_string_part (&fresh_reply, "fresh-router-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply_part (router, &reply_rid, request_seq, &fresh_reply,
                               ZLINK_PART_FINAL));
    {
        std::lock_guard<std::mutex> lock (router_reply_state->mutex);
        TEST_ASSERT_TRUE (router_reply_state->router_reply_targets.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, router_reply_state->reply_target_slots);
        TEST_ASSERT_EQUAL_UINT64 (0, router_reply_state->reply_target_checkouts);
    }
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
    router_reply_state.reset ();
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
    unsigned char retained_kind = 0xff;
    uint64_t retained_sequence = std::numeric_limits<uint64_t>::max ();
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&request)
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    const std::shared_ptr<zlink::socket_reqrep_internal::socket_request_reply_state_t>
      request_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (dealer));
    TEST_ASSERT_NOT_NULL (request_state.get ());
    {
        std::lock_guard<std::mutex> lock (request_state->mutex);
        TEST_ASSERT_TRUE (request_state->pending_requests.empty ());
    }
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

void test_request_correlation_budget_allows_one_empty_pair_oversize_and_recovers ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const uint64_t hwm = 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 5);

    reply_probe_t first_reply;
    first_reply.progress_handle = dealer;
    zlink_msg_t first;
    init_filled_part (&first, 65536, 'a');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &first, ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &first_reply));

    request_handler_probe_t received_first;
    recv_router_request_into_probe (router, &received_first);
    process_socket_commands_through_public_api (dealer);

    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    zlink_auto_hwm_budget_snapshot_t drained =
      read_request_reply_hwm_snapshot ();
    while (drained.current_accounted_bytes != 0
           && std::chrono::steady_clock::now () < drain_deadline) {
        process_socket_commands_through_public_api (dealer);
        msleep (1);
        drained = read_request_reply_hwm_snapshot ();
    }
    TEST_ASSERT_EQUAL_UINT64 (0, drained.current_accounted_bytes);

    reply_probe_t rejected_reply;
    rejected_reply.progress_handle = dealer;
    zlink_msg_t rejected;
    init_filled_part (&rejected, 65536, 'b');
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_dealer_request_part (dealer, &rejected,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &rejected_reply));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    zlink_msg_t ordinary;
    init_string_part (&ordinary, "ordinary-send-remains-admissible");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &ordinary, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL));
    const zlink_routing_id_t *ordinary_source = NULL;
    uint64_t ordinary_request_seq = 1;
    zlink_msg_t *ordinary_parts = NULL;
    size_t ordinary_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &ordinary_source, &ordinary_request_seq,
                         &ordinary_parts, &ordinary_part_count,
                         ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, ordinary_request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, ordinary_part_count);
    TEST_ASSERT_EQUAL_STRING (
      "ordinary-send-remains-admissible",
      msg_to_string (&ordinary_parts[0]).c_str ());
    zlink_multipart_close (ordinary_parts, ordinary_part_count);

    send_captured_reply (router, &received_first, "first-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&first_reply));

    reply_probe_t recovered_reply;
    recovered_reply.progress_handle = dealer;
    zlink_msg_t recovered;
    init_filled_part (&recovered, 65536, 'c');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &recovered,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &recovered_reply));

    request_handler_probe_t received_recovered;
    recv_router_request_into_probe (router, &received_recovered);
    send_captured_reply (router, &received_recovered, "recovered-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&recovered_reply));

    {
        std::lock_guard<std::mutex> lock (rejected_reply.mutex);
        TEST_ASSERT_FALSE (rejected_reply.done);
        TEST_ASSERT_EQUAL_UINT64 (0, rejected_reply.callback_count);
    }
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void run_request_correlation_work_budget_case (uint64_t hwm_,
                                               const char *endpoint_)
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm_, sizeof (hwm_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm_, sizeof (hwm_)));
    const int send_timeout_ms = 500;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint_));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint_));
    msleep (SETTLE_TIME * 5);

    reply_probe_t first_reply;
    first_reply.progress_handle = dealer;
    zlink_msg_t first;
    init_filled_part (&first, 65536, 'a');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &first,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &first_reply));
    request_handler_probe_t received_first;
    recv_router_request_into_probe (router, &received_first);

    reply_probe_t rejected_reply;
    rejected_reply.progress_handle = dealer;
    zlink_msg_t rejected;
    init_filled_part (&rejected, 65536, 'x');
    errno = 0;
    const std::chrono::steady_clock::time_point rejected_at =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_dealer_request_part (dealer, &rejected,
                                 ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &rejected_reply));
    const long rejected_elapsed_ms =
      static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - rejected_at)
                           .count ());
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_TRUE_MESSAGE (
      rejected_elapsed_ms < 250,
      "request correlation backpressure must not wait for SNDTIMEO");

    send_captured_reply (router, &received_first, "window-release");
    TEST_ASSERT_TRUE (wait_for_reply (&first_reply));

    reply_probe_t recovered_reply;
    recovered_reply.progress_handle = dealer;
    zlink_msg_t recovered;
    init_filled_part (&recovered, 65536, 'r');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &recovered,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &recovered_reply));
    request_handler_probe_t received_recovered;
    recv_router_request_into_probe (router, &received_recovered);
    send_captured_reply (router, &received_recovered, "window-recovered");
    TEST_ASSERT_TRUE (wait_for_reply (&recovered_reply));

    {
        std::lock_guard<std::mutex> lock (rejected_reply.mutex);
        TEST_ASSERT_EQUAL_UINT64 (0, rejected_reply.callback_count);
    }
    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_correlation_work_budget_applies_with_finite_and_zero_hwm ()
{
    run_request_correlation_work_budget_case (
      1024 * 1024, "inproc://zmp-request-correlation-liveness-finite");
    run_request_correlation_work_budget_case (
      0, "inproc://zmp-request-correlation-liveness-zero");
}

void test_request_correlation_work_budget_keeps_1024b_pipeline_open ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const uint64_t hwm = 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-request-correlation-1024-pipeline"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer,
                     "inproc://zmp-request-correlation-1024-pipeline"));
    msleep (SETTLE_TIME * 5);

    const size_t request_count = 192;
    std::vector<std::unique_ptr<reply_probe_t> > reply_probes;
    std::vector<request_event_t> requests;
    reply_probes.reserve (request_count);
    requests.reserve (request_count);

    for (size_t i = 0; i != request_count; ++i) {
        std::unique_ptr<reply_probe_t> reply_probe (new reply_probe_t ());
        reply_probe->progress_handle = dealer;
        zlink_msg_t request;
        init_filled_part (&request, 1024, static_cast<unsigned char> (i));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_dealer_request_part (dealer, &request,
                                     ZLINK_SEND_FLAGS_DONTWAIT,
                                     ZLINK_PART_FINAL, 3000, &capture_reply,
                                     reply_probe.get ()));

        const zlink_routing_id_t *peer_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv (router, &peer_rid, &request_seq, &parts,
                             &part_count, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (peer_rid);
        TEST_ASSERT_TRUE (request_seq != 0);
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);

        request_event_t event;
        event.request_seq = request_seq;
        event.peer_rid_value = *peer_rid;
        requests.push_back (event);
        zlink_multipart_close (parts, part_count);
        reply_probes.push_back (std::move (reply_probe));
    }

    for (size_t i = 0; i != requests.size (); ++i) {
        zlink_msg_t reply;
        init_string_part (&reply, "pipeline-reply");
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_router_reply (router, &requests[i].peer_rid_value,
                              requests[i].request_seq, &reply, 1));
    }
    for (size_t i = 0; i != reply_probes.size (); ++i)
        TEST_ASSERT_TRUE (wait_for_reply (reply_probes[i].get ()));

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_correlation_budget_recovers_after_timeout ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    const uint64_t hwm = 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-request-correlation-timeout"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-request-correlation-timeout"));
    msleep (SETTLE_TIME * 5);

    reply_probe_t timeout_reply;
    timeout_reply.progress_handle = dealer;
    zlink_msg_t first;
    init_filled_part (&first, 65536, 't');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &first,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 500, &capture_reply,
                                 &timeout_reply));

    request_handler_probe_t received_first;
    recv_router_request_into_probe (router, &received_first);
    process_socket_commands_through_public_api (dealer);

    reply_probe_t rejected_reply;
    rejected_reply.progress_handle = dealer;
    zlink_msg_t rejected;
    init_filled_part (&rejected, 65536, 'x');
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_dealer_request_part (dealer, &rejected,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &rejected_reply));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    TEST_ASSERT_TRUE (wait_for_reply (&timeout_reply));
    {
        std::lock_guard<std::mutex> lock (timeout_reply.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
                               timeout_reply.result);
        TEST_ASSERT_EQUAL_UINT64 (1, timeout_reply.callback_count);
    }

    reply_probe_t recovered_reply;
    recovered_reply.progress_handle = dealer;
    zlink_msg_t recovered;
    init_filled_part (&recovered, 65536, 'r');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &recovered,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &recovered_reply));
    request_handler_probe_t received_recovered;
    recv_router_request_into_probe (router, &received_recovered);
    send_captured_reply (router, &received_recovered, "timeout-recovered");
    TEST_ASSERT_TRUE (wait_for_reply (&recovered_reply));
    {
        std::lock_guard<std::mutex> lock (rejected_reply.mutex);
        TEST_ASSERT_EQUAL_UINT64 (0, rejected_reply.callback_count);
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_request_correlation_budget_is_independent_per_pair ()
{
    void *router_a = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *router_b = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router_a);
    TEST_ASSERT_NOT_NULL (router_b);
    TEST_ASSERT_NOT_NULL (dealer);

    const uint64_t hwm = 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router_b, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router_a, "inproc://zmp-request-correlation-pair-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router_b, "inproc://zmp-request-correlation-pair-b"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-request-correlation-pair-a"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-request-correlation-pair-b"));
    msleep (SETTLE_TIME * 5);

    reply_probe_t reply_a;
    reply_probe_t reply_b;
    reply_a.progress_handle = dealer;
    reply_b.progress_handle = dealer;
    zlink_msg_t first;
    init_filled_part (&first, 65536, '1');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &first,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &reply_a));

    request_handler_probe_t received_a;
    request_handler_probe_t received_b;
    bool first_at_a = false;
    bool first_received = false;
    const std::chrono::steady_clock::time_point first_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!first_received
           && std::chrono::steady_clock::now () < first_deadline) {
        if (try_recv_router_request_into_probe (router_a, &received_a)) {
            first_at_a = true;
            first_received = true;
        } else if (try_recv_router_request_into_probe (router_b, &received_b)) {
            first_received = true;
        } else {
            msleep (1);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (first_received,
                              "first request did not reach either pair");
    process_socket_commands_through_public_api (dealer);

    zlink_msg_t second;
    init_filled_part (&second, 65536, '2');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (dealer, &second,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &reply_b));
    bool second_received = false;
    const std::chrono::steady_clock::time_point second_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!second_received
           && std::chrono::steady_clock::now () < second_deadline) {
        second_received =
          first_at_a
            ? try_recv_router_request_into_probe (router_b, &received_b)
            : try_recv_router_request_into_probe (router_a, &received_a);
        if (!second_received)
            msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (
      second_received, "second request did not use the independent pair");
    process_socket_commands_through_public_api (dealer);

    reply_probe_t rejected_reply;
    rejected_reply.progress_handle = dealer;
    zlink_msg_t rejected;
    init_filled_part (&rejected, 65536, '3');
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_dealer_request_part (dealer, &rejected,
                                 ZLINK_SEND_FLAGS_DONTWAIT,
                                 ZLINK_PART_FINAL, 3000, &capture_reply,
                                 &rejected_reply));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    send_captured_reply (router_a, &received_a, "pair-a-reply");
    send_captured_reply (router_b, &received_b, "pair-b-reply");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_a));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_b));
    {
        std::lock_guard<std::mutex> lock (rejected_reply.mutex);
        TEST_ASSERT_EQUAL_UINT64 (0, rejected_reply.callback_count);
    }

    close_test_socket_after_reply_callback (dealer);
    test_context_socket_close_zero_linger (router_a);
    test_context_socket_close_zero_linger (router_b);
}

void test_router_exact_request_full_keeps_route_active ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (dealer);
    TEST_ASSERT_NOT_NULL (router);

    const char dealer_rid_text[] = "request-full-exact-dealer";
    set_routing_id_text (dealer, dealer_rid_text);
    set_routing_id_text (router, "request-full-exact-router");
    const uint64_t hwm = 1024 * 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    const int send_timeout_ms = 500;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (dealer, "inproc://zmp-router-request-full-exact"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (router, "inproc://zmp-router-request-full-exact"));

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

    reply_probe_t first_reply;
    first_reply.progress_handle = router;
    zlink_msg_t first;
    init_filled_part (&first, 65536, 'q');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &first,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 3000,
        &capture_reply, &first_reply));

    uint8_t first_type = 0xff;
    uint64_t first_sequence = 0;
    zlink_part_flag_t first_more = ZLINK_PART_MORE;
    zlink_msg_t first_received;
    zlink_msg_init (&first_received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (dealer, &first_type, &first_sequence,
                                   &first_received, &first_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, first_type);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, first_more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&first_received));
    process_socket_commands_through_public_api (router);

    reply_probe_t rejected_reply;
    rejected_reply.progress_handle = router;
    zlink_msg_t rejected;
    init_filled_part (&rejected, 65536, 'z');
    errno = 0;
    const std::chrono::steady_clock::time_point rejected_at =
      std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &rejected,
        ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 3000,
        &capture_reply, &rejected_reply));
    const long rejected_elapsed_ms =
      static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                           std::chrono::steady_clock::now () - rejected_at)
                           .count ());
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_TRUE_MESSAGE (
      rejected_elapsed_ms < 250,
      "exact request correlation backpressure must not wait for SNDTIMEO");

    zlink_routed_submit_target_t still_active;
    memset (&still_active, 0, sizeof (still_active));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_select_routed_submit_target (router, &dealer_rid, &still_active));
    TEST_ASSERT_EQUAL_UINT64 (target.transport_pair_id,
                              still_active.transport_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (target.transport_pair_generation,
                              still_active.transport_pair_generation);

    zlink_msg_t ordinary;
    init_string_part (&ordinary, "ordinary-after-request-full");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (router, &dealer_rid, &ordinary,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL));
    uint8_t ordinary_type = 0xff;
    uint64_t ordinary_sequence = 1;
    zlink_part_flag_t ordinary_more = ZLINK_PART_MORE;
    zlink_msg_t ordinary_received;
    zlink_msg_init (&ordinary_received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (dealer, &ordinary_type,
                                   &ordinary_sequence, &ordinary_received,
                                   &ordinary_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, ordinary_type);
    TEST_ASSERT_EQUAL_UINT64 (0, ordinary_sequence);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, ordinary_more);
    TEST_ASSERT_EQUAL_STRING (
      "ordinary-after-request-full",
      part_to_string_and_close (&ordinary_received).c_str ());

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, router, router, ZLINK_POLLOUT));
    zlink_poller_event_t writable_event;
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &writable_event, 1, 0, NULL));

    zlink_msg_t reply;
    init_string_part (&reply, "exact-request-full-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (dealer, first_sequence, &reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&first_reply));
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &writable_event, 1, 3000, NULL));
    TEST_ASSERT_EQUAL_PTR (router, writable_event.socket);
    TEST_ASSERT_TRUE ((writable_event.events & ZLINK_POLLOUT) != 0);

    reply_probe_t recovered_reply;
    recovered_reply.progress_handle = router;
    zlink_msg_t recovered;
    init_filled_part (&recovered, 65536, 'r');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_request_transport_pair_part (
        router, &dealer_rid, target.transport_pair_id,
        target.transport_pair_generation, &recovered,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 3000,
        &capture_reply, &recovered_reply));

    uint8_t recovered_type = 0xff;
    uint64_t recovered_sequence = 0;
    zlink_part_flag_t recovered_more = ZLINK_PART_MORE;
    zlink_msg_t recovered_received;
    zlink_msg_init (&recovered_received);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (dealer, &recovered_type,
                                   &recovered_sequence, &recovered_received,
                                   &recovered_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, recovered_type);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, recovered_more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&recovered_received));

    zlink_msg_t recovered_reply_part;
    init_string_part (&recovered_reply_part, "exact-request-recovered");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (dealer, recovered_sequence,
                               &recovered_reply_part, ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&recovered_reply));
    {
        std::lock_guard<std::mutex> lock (rejected_reply.mutex);
        TEST_ASSERT_EQUAL_UINT64 (0, rejected_reply.callback_count);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, router));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));

    close_test_socket_after_reply_callback (router);
    test_context_socket_close_zero_linger (dealer);
}

void test_dealer_disconnect_forgets_reply_token_before_pipe_deallocation ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server, "inproc://dealer-reply-token-disconnect"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client, "inproc://dealer-reply-token-disconnect"));
    msleep (SETTLE_TIME);

    send_internal_request_message (client, 701);
    uint8_t message_type = 0xff;
    uint64_t request_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server, &message_type, &request_token,
                                   &request, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      server_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (server));
    TEST_ASSERT_NOT_NULL (server_state.get ());
    {
        std::lock_guard<std::mutex> lock (server_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, server_state->dealer_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, server_state->reply_target_slots);
    }

    test_context_socket_close_zero_linger (client);
    bool target_forgotten = false;
    for (int attempt = 0; attempt < 1000 && !target_forgotten; ++attempt) {
        process_socket_commands_through_public_api (server);
        {
            std::lock_guard<std::mutex> lock (server_state->mutex);
            target_forgotten = server_state->dealer_reply_targets.empty ()
                               && server_state->reply_target_slots == 0;
        }
        if (!target_forgotten)
            msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (target_forgotten,
                              "disconnected DEALER reply token was retained");

    zlink_msg_t stale_reply;
    init_string_part (&stale_reply, "stale-token-reply");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_FOUND,
      zlink_dealer_reply_part (server, request_token, &stale_reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOENT, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&stale_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&stale_reply));

    // Repeat detach without replying so a missing cleanup cannot accumulate
    // one hidden slot per reconnect until the bounded target table fills.
    for (uint64_t cycle = 0; cycle < 2; ++cycle) {
        void *expired_client = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (expired_client);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (
          expired_client, "inproc://dealer-reply-token-disconnect"));
        msleep (SETTLE_TIME);
        send_internal_request_message (expired_client, 702 + cycle);

        request_token = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&request));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          recv_dealer_part_with_retry (server, &message_type, &request_token,
                                       &request, &has_more));
        TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
        TEST_ASSERT_TRUE (request_token != 0);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));
        {
            std::lock_guard<std::mutex> lock (server_state->mutex);
            TEST_ASSERT_EQUAL_UINT64 (
              1, server_state->dealer_reply_targets.size ());
            TEST_ASSERT_EQUAL_UINT64 (1, server_state->reply_target_slots);
        }

        test_context_socket_close_zero_linger (expired_client);
        target_forgotten = false;
        for (int attempt = 0; attempt < 1000 && !target_forgotten;
             ++attempt) {
            process_socket_commands_through_public_api (server);
            {
                std::lock_guard<std::mutex> lock (server_state->mutex);
                target_forgotten =
                  server_state->dealer_reply_targets.empty ()
                  && server_state->reply_target_slots == 0;
            }
            if (!target_forgotten)
                msleep (1);
        }
        TEST_ASSERT_TRUE (target_forgotten);

        init_string_part (&stale_reply, "stale-reconnect-token-reply");
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_NOT_FOUND,
          zlink_dealer_reply_part (server, request_token, &stale_reply,
                                   ZLINK_PART_FINAL));
        TEST_ASSERT_EQUAL_INT (ENOENT, errno);
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&stale_reply));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&stale_reply));
    }

    void *next_client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (next_client);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (next_client, "inproc://dealer-reply-token-disconnect"));
    msleep (SETTLE_TIME);
    zlink_msg_t next_request;
    init_string_part (&next_request, "request-after-disconnect");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = next_client;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (next_client, &next_request, 1, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 3000));

    request_token = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server, &message_type, &request_token,
                                   &request, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));
    zlink_msg_t reply;
    init_string_part (&reply, "reply-after-disconnect");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_reply_part (server, request_token, &reply,
                               ZLINK_PART_FINAL));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("reply-after-disconnect",
                                  reply_probe.payload.c_str ());
    }

    close_test_socket_after_reply_callback (next_client);
    test_context_socket_close_zero_linger (server);
}

void test_dealer_reply_drains_queued_disconnect_before_target_checkout ()
{
    const char *endpoint =
      "inproc://dealer-reply-drains-queued-disconnect";
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    msleep (SETTLE_TIME);

    send_internal_request_message (client, 750);
    uint8_t message_type = 0xff;
    uint64_t request_token = 0;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_dealer_part_with_retry (server, &message_type, &request_token,
                                   &request, &has_more));
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_REQUEST, message_type);
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));

    // Keep the white-box public-handle pin inside this inspection scope. Close
    // requires its own pin to be the sole remaining public-handle owner.
    {
        const socket_handle_t server_handle = as_socket_handle (server);
        TEST_ASSERT_NOT_NULL (server_handle.socket);
        const std::shared_ptr<
          zlink::socket_reqrep_internal::socket_request_reply_state_t>
          server_state = zlink::socket_reqrep_internal::find_request_reply_state (
            server_handle);
        TEST_ASSERT_NOT_NULL (server_state.get ());

        // Disconnect queues pipe_term on the server. Let the server acknowledge
        // it, then let the client queue the reciprocal pipe_term_ack. Do not touch
        // the server mailbox again before the direct reply commit point.
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint));
        process_socket_commands_through_public_api (server);
        process_socket_commands_through_public_api (client);

        {
            std::lock_guard<std::mutex> lock (server_state->mutex);
            const std::unordered_map<
              uint64_t,
              zlink::socket_reqrep_internal::dealer_reply_target_t>::const_iterator
              target = server_state->dealer_reply_targets.find (request_token);
            TEST_ASSERT_TRUE (target != server_state->dealer_reply_targets.end ());
            TEST_ASSERT_NOT_NULL (target->second.pipe);
            TEST_ASSERT_FALSE (target->second.checked_out);
            TEST_ASSERT_EQUAL_UINT64 (1, server_state->reply_target_slots);
            TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_checkouts);
        }

        uint64_t public_mailbox_drains_before = 0;
        server_handle.socket->test_receive_owner_snapshot (
          NULL, &public_mailbox_drains_before, NULL);

        zlink_msg_t reply;
        init_string_part (&reply, "reply-after-queued-disconnect");
        errno = 0;
        const zlink_submit_result_t submit = zlink_dealer_reply_part (
          server, request_token, &reply, ZLINK_PART_FINAL);
        const int submit_errno = errno;

        uint64_t public_mailbox_drains_after = 0;
        server_handle.socket->test_receive_owner_snapshot (
          NULL, &public_mailbox_drains_after, NULL);
        TEST_ASSERT_TRUE (public_mailbox_drains_after
                          > public_mailbox_drains_before);
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_FOUND, submit);
        TEST_ASSERT_EQUAL_INT (ENOENT, submit_errno);
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&reply));

        {
            std::lock_guard<std::mutex> lock (server_state->mutex);
            TEST_ASSERT_TRUE (server_state->dealer_reply_targets.empty ());
            TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_slots);
            TEST_ASSERT_EQUAL_UINT64 (0, server_state->reply_target_checkouts);
        }
    }

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void test_router_disconnect_forgets_reply_target_before_pipe_deallocation ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (client);
    set_routing_id_text (client, "router-disconnect-old");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://router-reply-target-disconnect"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (client, "inproc://router-reply-target-disconnect"));
    msleep (SETTLE_TIME);

    send_internal_request_message (client, 801);
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (801, request_seq);
    zlink_routing_id_t stale_rid = *source_rid;
    zlink_multipart_close (parts, part_count);

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      router_state = zlink::socket_reqrep_internal::find_request_reply_state (
        as_socket_handle (router));
    TEST_ASSERT_NOT_NULL (router_state.get ());
    {
        std::lock_guard<std::mutex> lock (router_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, router_state->router_reply_targets.size ());
        TEST_ASSERT_EQUAL_UINT64 (1, router_state->reply_target_slots);
    }

    test_context_socket_close_zero_linger (client);
    bool target_forgotten = false;
    for (int attempt = 0; attempt < 1000 && !target_forgotten; ++attempt) {
        process_socket_commands_through_public_api (router);
        {
            std::lock_guard<std::mutex> lock (router_state->mutex);
            target_forgotten = router_state->router_reply_targets.empty ()
                               && router_state->reply_target_slots == 0;
        }
        if (!target_forgotten)
            msleep (1);
    }
    TEST_ASSERT_TRUE_MESSAGE (target_forgotten,
                              "disconnected ROUTER reply target was retained");

    zlink_msg_t stale_reply;
    init_string_part (&stale_reply, "stale-router-reply");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_router_reply_part (router, &stale_rid, request_seq,
                               &stale_reply, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&stale_reply));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&stale_reply));

    void *next_client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (next_client);
    set_routing_id_text (next_client, "router-disconnect-new");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (next_client, "inproc://router-reply-target-disconnect"));
    msleep (SETTLE_TIME);
    zlink_msg_t next_request;
    init_string_part (&next_request, "router-request-after-disconnect");
    reply_probe_t reply_probe;
    reply_probe.progress_handle = next_client;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (next_client, &next_request, 1, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 3000));
    request_handler_probe_t received;
    recv_router_request_into_probe (router, &received);
    send_captured_reply (router, &received, "router-reply-after-disconnect");
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("router-reply-after-disconnect",
                                  reply_probe.payload.c_str ());
    }

    close_test_socket_after_reply_callback (next_client);
    test_context_socket_close_zero_linger (router);
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
        send_internal_request_message (dealer, static_cast<uint64_t> (i + 1));
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

    send_internal_request_message (
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
    RUN_SELECTED (test_reserved_zmp_kind_is_not_request_reply);
    RUN_SELECTED (test_reply_callback_rejects_sync_and_async_submit_on_all_sockets);
    RUN_SELECTED (test_dealer_to_router_request_reply_basic);
    RUN_SELECTED (test_reply_callback_cleanup_preserves_ownership_actions);
    RUN_SELECTED (test_dealer_receives_unsolicited_message_after_request_reply);
    RUN_SELECTED (test_generic_dealer_receive_clears_request_reply_metadata);
    RUN_SELECTED (test_dealer_receive_rejects_request_reply_metadata_after_first_part);
    RUN_SELECTED (
      test_source_pipe_pin_failure_preserves_receive_ownership_without_targets);
    RUN_SELECTED (
      test_pair_raw_receive_strips_first_kind_and_rejects_later_kind);
    RUN_SELECTED (test_request_reply_preserves_empty_payload_shapes);
    RUN_SELECTED (test_legacy_four_part_signature_remains_ordinary_payload);
    RUN_SELECTED (test_concurrent_first_dealer_requests_share_dispatch_install);
    RUN_SELECTED (test_blocking_generic_dealer_recv_remains_on_transport_pipe);
    RUN_SELECTED (test_blocking_typed_dealer_recv_remains_on_transport_pipe);
    RUN_SELECTED (test_generic_dealer_recv_honors_configured_no_input_timeout);
    RUN_SELECTED (test_typed_dealer_recv_honors_configured_no_input_timeout);
    RUN_SELECTED (test_direct_dealer_generic_recv_and_poller_preserve_raw_order);
    RUN_SELECTED (test_prefixed_multipart_second_prefix_allocation_failure_rolls_back);
    RUN_SELECTED (test_dealer_to_router_request_reply_over_tcp_with_explicit_routing_id);
    RUN_SELECTED (test_dealer_to_router_request_reply_over_ipc);
    RUN_SELECTED (test_dealer_to_router_request_reply_over_tls);
    RUN_SELECTED (test_router_reply_burst_completion_remains_correct_over_tcp);
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
    RUN_SELECTED (
      test_same_rid_handover_colliding_wire_sequences_keep_exact_reply_targets);
    RUN_SELECTED (
      test_same_physical_peer_duplicate_wire_sequence_is_protocol_error);
    RUN_SELECTED (test_out_of_order_replies_match_original_request);
    RUN_SELECTED (test_extra_reply_is_rejected_after_first_completion);
    RUN_SELECTED (test_dealer_to_dealer_reply_routes_to_source_peer_and_closes);
    RUN_SELECTED (test_dealer_to_dealer_multipart_reply_preserves_large_first_part);
    RUN_SELECTED (test_dealer_multipart_request_preserves_pending_cookie);
    RUN_SELECTED (test_request_stage_allocation_failure_consumes_and_aborts_sequence);
    RUN_SELECTED (test_grouped_request_is_consumed_before_pending_registration);
    RUN_SELECTED (test_runtime_receive_spill_failure_discards_record_tail);
    RUN_SELECTED (test_router_lazy_state_failure_discards_record_tail);
    RUN_SELECTED (test_router_receive_export_failure_rolls_back_reply_target);
    RUN_SELECTED (test_dealer_receive_export_failure_rolls_back_reply_target);
    RUN_SELECTED (test_router_part_stage_failure_revokes_published_reply_target);
    RUN_SELECTED (test_dealer_part_stage_failure_revokes_published_reply_token);
    RUN_SELECTED (test_dealer_staged_reply_completes_without_combined_buffer);
    RUN_SELECTED (test_router_reply_allocations_preserve_target_until_fresh_reply);
    RUN_SELECTED (test_dealer_request_receive_without_reply_closes_cleanly);
    RUN_SELECTED (test_dealer_close_drains_pending_request_completion);
    RUN_SELECTED (test_router_request_to_dealer_times_out_when_reply_is_missing);
    RUN_SELECTED (test_dealer_request_uses_socket_default_timeout_when_reply_is_missing);
    RUN_SELECTED (test_reply_timeout_does_not_turn_failed_request_admission_into_success);
    RUN_SELECTED (
      test_request_correlation_budget_allows_one_empty_pair_oversize_and_recovers);
    RUN_SELECTED (
      test_request_correlation_work_budget_applies_with_finite_and_zero_hwm);
    RUN_SELECTED (
      test_request_correlation_work_budget_keeps_1024b_pipeline_open);
    RUN_SELECTED (test_request_correlation_budget_recovers_after_timeout);
    RUN_SELECTED (test_request_correlation_budget_is_independent_per_pair);
    RUN_SELECTED (test_router_exact_request_full_keeps_route_active);
    RUN_SELECTED (test_dealer_disconnect_forgets_reply_token_before_pipe_deallocation);
    RUN_SELECTED (test_dealer_reply_drains_queued_disconnect_before_target_checkout);
    RUN_SELECTED (test_router_disconnect_forgets_reply_target_before_pipe_deallocation);
    RUN_SELECTED (test_router_reply_target_slots_are_bounded_and_released);
    RUN_SELECTED (test_request_reply_process_exits_cleanly_after_round_trip);
#undef RUN_SELECTED
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
