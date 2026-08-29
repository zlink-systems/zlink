/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "sockets/common/socket_base.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct completion_snapshot_t
{
    zlink_send_op_id_t op_id;
    zlink_send_complete_result_t result;
    int terminal_errno;
};

struct completion_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<completion_snapshot_t> events;
    std::thread::id callback_thread;
};

struct free_probe_t
{
    free_probe_t () : count (0) {}

    std::mutex mutex;
    std::condition_variable changed;
    int count;
};

struct completion_self_close_probe_t
{
    completion_self_close_probe_t () : callback_count (0), op_id (0),
                                       result (ZLINK_SEND_ADMITTED),
                                       terminal_errno (0),
                                       close_result (ZLINK_CLOSE_BUSY),
                                       done (false), active_callbacks (0)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    size_t callback_count;
    zlink_send_op_id_t op_id;
    zlink_send_complete_result_t result;
    int terminal_errno;
    zlink_close_result_t close_result;
    bool done;
    size_t active_callbacks;
};

class completion_self_close_callback_scope_t
{
  public:
    explicit completion_self_close_callback_scope_t (
      completion_self_close_probe_t *probe_) : _probe (probe_)
    {
        std::lock_guard<std::mutex> lock (_probe->mutex);
        ++_probe->active_callbacks;
    }

    ~completion_self_close_callback_scope_t ()
    {
        std::lock_guard<std::mutex> lock (_probe->mutex);
        --_probe->active_callbacks;
        // Keep notification inside the lock: the waiter cannot return and
        // destroy the probe until this callback has finished its final access.
        _probe->changed.notify_all ();
    }

  private:
    completion_self_close_probe_t *_probe;
};

struct completion_reentry_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    void *different_socket;
    void *request_socket;
    void *publish_socket;
    size_t callback_count;
    zlink_send_complete_result_t completion_result;
    zlink_send_op_id_t completion_op_id;
    zlink_submit_result_t same_send;
    zlink_submit_result_t different_send;
    zlink_submit_result_t async_send;
    zlink_submit_result_t different_async_send;
    zlink_submit_result_t publish;
    zlink_submit_result_t request;
    int same_send_errno;
    int different_send_errno;
    int async_send_errno;
    int different_async_errno;
    int publish_errno;
    int request_errno;
    bool done;

    completion_reentry_probe_t () :
        different_socket (NULL), request_socket (NULL), publish_socket (NULL),
        callback_count (0), completion_result (ZLINK_SEND_TERMINAL), completion_op_id (0),
        same_send (ZLINK_SUBMIT_INTERNAL_ERROR),
        different_send (ZLINK_SUBMIT_INTERNAL_ERROR),
        async_send (ZLINK_SUBMIT_INTERNAL_ERROR),
        different_async_send (ZLINK_SUBMIT_INTERNAL_ERROR),
        publish (ZLINK_SUBMIT_INTERNAL_ERROR), request (ZLINK_SUBMIT_INTERNAL_ERROR),
        same_send_errno (0), different_send_errno (0), async_send_errno (0),
        different_async_errno (0),
        publish_errno (0), request_errno (0), done (false)
    {
    }
};

struct gate_release_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

struct inline_fallback_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

struct deadline_enqueue_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

void block_first_gate_release (void *userdata_)
{
    gate_release_probe_t *probe =
      static_cast<gate_release_probe_t *> (userdata_);
    std::unique_lock<std::mutex> lock (probe->mutex);
    if (probe->entered)
        return;
    probe->entered = true;
    probe->changed.notify_all ();
    probe->changed.wait (lock, [probe] { return probe->release; });
}

void block_inline_fallback (void *userdata_)
{
    inline_fallback_probe_t *probe =
      static_cast<inline_fallback_probe_t *> (userdata_);
    std::unique_lock<std::mutex> lock (probe->mutex);
    probe->entered = true;
    probe->changed.notify_all ();
    probe->changed.wait (lock, [probe] { return probe->release; });
}

void block_deadline_enqueue (void *userdata_)
{
    deadline_enqueue_probe_t *probe =
      static_cast<deadline_enqueue_probe_t *> (userdata_);
    std::unique_lock<std::mutex> lock (probe->mutex);
    probe->entered = true;
    probe->changed.notify_all ();
    probe->changed.wait (lock, [probe] { return probe->release; });
}

void init_part (zlink_msg_t *part_, const std::string &payload_);

void capture_completion (void *, const zlink_send_complete_event_t *event_, void *userdata_)
{
    completion_probe_t *probe = static_cast<completion_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        completion_snapshot_t snapshot;
        snapshot.op_id = event_->op_id;
        snapshot.result = event_->result;
        snapshot.terminal_errno = event_->terminal_errno;
        probe->events.push_back (snapshot);
        probe->callback_thread = std::this_thread::get_id ();
        probe->changed.notify_all ();
    }
}

void capture_completion_and_self_close (
  void *subject_, const zlink_send_complete_event_t *event_, void *userdata_)
{
    completion_self_close_probe_t *probe =
      static_cast<completion_self_close_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    // The test must not destroy the probe after observing completion while the
    // dispatcher is still executing this callback's epilogue.
    completion_self_close_callback_scope_t callback_scope (probe);

    errno = 0;
    const zlink_close_result_t close_result = zlink_close (subject_);
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        ++probe->callback_count;
        probe->op_id = event_->op_id;
        probe->result = event_->result;
        probe->terminal_errno = event_->terminal_errno;
        probe->close_result = close_result;
        probe->done = true;
    }
}

void ignore_reply (zlink_request_result_t, zlink_msg_t *, size_t, void *)
{
}

zlink_submit_result_t attempt_sync_part (void *socket_, int *errno_out_)
{
    zlink_msg_t part;
    init_part (&part, "reentrant-sync");
    errno = 0;
    const zlink_submit_result_t result = zlink_send_part (
      socket_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
    *errno_out_ = errno;
    if (result != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return result;
}

zlink_submit_result_t attempt_async_part (void *socket_, int *errno_out_)
{
    zlink_msg_t part;
    init_part (&part, "reentrant-async");
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    errno = 0;
    const zlink_submit_result_t result = zlink_send_async (
      socket_, &part, 1, &options, &op_id);
    *errno_out_ = errno;
    if (result != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return result;
}

void capture_reentrant_completion (
  void *subject_, const zlink_send_complete_event_t *event_, void *userdata_)
{
    completion_reentry_probe_t *probe =
      static_cast<completion_reentry_probe_t *> (userdata_);
    ++probe->callback_count;
    probe->completion_result = event_->result;
    probe->completion_op_id = event_->op_id;
    probe->same_send = attempt_sync_part (subject_, &probe->same_send_errno);
    probe->different_send = attempt_sync_part (
      probe->different_socket, &probe->different_send_errno);
    probe->async_send = attempt_async_part (subject_, &probe->async_send_errno);
    probe->different_async_send = attempt_async_part (
      probe->different_socket, &probe->different_async_errno);

    zlink_msg_t publish_part;
    init_part (&publish_part, "reentrant-publish");
    errno = 0;
    probe->publish = zlink_publish_part (
      probe->publish_socket, "reentrant", &publish_part,
      ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
    probe->publish_errno = errno;
    if (probe->publish != ZLINK_SUBMIT_OK)
        zlink_msg_close (&publish_part);

    zlink_msg_t request_part;
    init_part (&request_part, "reentrant-request");
    errno = 0;
    probe->request = zlink_dealer_request_part (
      probe->request_socket, &request_part, ZLINK_SEND_FLAGS_NONE,
      ZLINK_PART_FINAL, 1000, &ignore_reply, NULL);
    probe->request_errno = errno;
    if (probe->request != ZLINK_SUBMIT_OK)
        zlink_msg_close (&request_part);

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
        probe->changed.notify_all ();
    }
}

bool wait_for_completion_count (completion_probe_t *probe_, size_t count_, int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->changed.wait_until (
             lock, deadline,
             [probe_, count_] { return probe_->events.size () >= count_; });
}

size_t completion_count (completion_probe_t *probe_)
{
    std::lock_guard<std::mutex> lock (probe_->mutex);
    return probe_->events.size ();
}

completion_snapshot_t completion_at (completion_probe_t *probe_, size_t index_)
{
    std::lock_guard<std::mutex> lock (probe_->mutex);
    return probe_->events[index_];
}

zlink_routing_id_t make_rid (const char *text_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (text_);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8 (sizeof (rid.data), size);
    rid.size = static_cast<uint8_t> (size);
    memcpy (rid.data, text_, size);
    return rid;
}

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

void count_free (void *, void *hint_)
{
    ++*static_cast<int *> (hint_);
}

void count_free_synchronized (void *, void *hint_)
{
    free_probe_t *probe = static_cast<free_probe_t *> (hint_);
    if (!probe)
        return;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        ++probe->count;
        probe->changed.notify_all ();
    }
}

int free_count (free_probe_t *probe_)
{
    std::lock_guard<std::mutex> lock (probe_->mutex);
    return probe_->count;
}

bool wait_for_free_count (free_probe_t *probe_, int expected_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->changed.wait_for (
      lock, std::chrono::seconds (3),
      [probe_, expected_] { return probe_->count >= expected_; });
}

zlink_submit_result_t submit_record (void *socket_,
                                      const std::vector<std::string> &payloads_,
                                      const zlink_send_async_options_t *options_,
                                      zlink_send_op_id_t *op_id_out_ = NULL)
{
    std::vector<zlink_msg_t> parts (payloads_.size ());
    for (size_t i = 0; i != payloads_.size (); ++i)
        init_part (&parts[i], payloads_[i]);

    zlink_send_op_id_t op_id = 0;
    const zlink_submit_result_t result =
      zlink_send_async (socket_, parts.data (), parts.size (), options_, &op_id);
    if (result != ZLINK_SUBMIT_OK)
        zlink_multipart_close (parts.data (), parts.size ());
    if (op_id_out_)
        *op_id_out_ = op_id;
    return result;
}

zlink_routed_submit_target_t select_router_target_eventually (
  void *router_, const zlink_routing_id_t *peer_rid_)
{
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    for (int i = 0; i != 3000; ++i) {
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (router_, peer_rid_, &target);
        if (result == ZLINK_SUBMIT_OK)
            return target;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("ROUTER target did not become selectable");
    return target;
}

zlink_routed_submit_target_t select_dealer_target_eventually (void *dealer_)
{
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    for (int i = 0; i != 3000; ++i) {
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (dealer_, NULL, &target);
        if (result == ZLINK_SUBMIT_OK)
            return target;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("DEALER target did not become selectable");
    return target;
}

bool recv_dealer_record_eventually (void *dealer_,
                                    const std::vector<std::string> &expected_,
                                    int timeout_ms_ = 3000)
{
    std::vector<std::string> actual;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_dealer_recv_part (
          dealer_, &message_type, &request_seq, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            zlink_msg_close (&part);
            msleep (1);
            continue;
        }
        if (result != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            return false;
        }

        actual.push_back (std::string (
          static_cast<const char *> (zlink_msg_data (&part)),
          zlink_msg_size (&part)));
        const bool final = has_more == ZLINK_PART_FINAL;
        zlink_msg_close (&part);
        if (final)
            break;
    }
    return actual == expected_;
}

int drain_dealer_parts (void *dealer_, int timeout_ms_ = 300)
{
    int count = 0;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_dealer_recv_part (
          dealer_, &message_type, &request_seq, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            zlink_msg_close (&part);
            msleep (1);
            continue;
        }
        if (result != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            break;
        }
        ++count;
        zlink_msg_close (&part);
    }
    return count;
}

bool dealer_has_no_part (void *dealer_, int timeout_ms_ = 100)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_dealer_recv_part (
          dealer_, &message_type, &request_seq, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            return false;
        }
        zlink_msg_close (&part);
        if (result != ZLINK_RECV_NO_DATA)
            return false;
        msleep (1);
    }
    return true;
}

bool recv_pair_record_eventually (void *receiver_,
                                  const std::vector<std::string> &expected_,
                                  int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t result = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        if (result != ZLINK_RECV_OK)
            return false;

        bool matches = part_count == expected_.size ();
        for (size_t i = 0; matches && i != part_count; ++i) {
            matches = zlink_msg_size (&parts[i]) == expected_[i].size ()
                      && memcmp (zlink_msg_data (&parts[i]),
                                 expected_[i].data (),
                                 expected_[i].size ()) == 0;
        }
        zlink_multipart_close (parts, part_count);
        return matches;
    }
    return false;
}

bool pair_has_no_record_for (void *receiver_, int timeout_ms_ = 100)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t result = zlink_recv (
          receiver_, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            zlink_multipart_close (parts, part_count);
            return false;
        }
        if (result != ZLINK_RECV_NO_DATA)
            return false;
        msleep (1);
    }
    return true;
}

bool close_test_socket_eventually (void *socket_, int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (true) {
        errno = 0;
        const zlink_close_result_t result = zlink_close (socket_);
        if (result == ZLINK_CLOSE_OK) {
            test_context_socket_mark_closed (socket_);
            return true;
        }
        if (result != ZLINK_CLOSE_BUSY || errno != EBUSY
            || std::chrono::steady_clock::now () >= deadline)
            return false;
        msleep (1);
    }
}

zlink_submit_result_t send_router_filler (
  void *router_, const zlink_routed_submit_target_t &target_, size_t size_)
{
    zlink_msg_t part;
    zlink_msg_init_size (&part, size_);
    memset (zlink_msg_data (&part), 0x4f, size_);
    //  The part helper consumes the message on both success and failure.
    return zlink_send_part_transport_pair (
      router_, &target_.peer_rid, target_.transport_pair_id,
      target_.transport_pair_generation, &part, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL);
}

}

void test_send_pending_limits_default_to_unlimited_and_accept_zero ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
    const zlink_option_t options[2] = {
      ZLINK_OPT_SEND_PENDING_MAX_MSGS, ZLINK_OPT_SEND_PENDING_MAX_BYTES};
    for (size_t i = 0; i != 2; ++i) {
        uint64_t value = 1;
        size_t value_size = sizeof (value);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (socket, options[i], &value, &value_size));
        TEST_ASSERT_EQUAL_UINT64 (0, value);

        value = 7;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (socket, options[i], &value, sizeof (value)));
        value = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (socket, options[i], &value, sizeof (value)));
        value = 1;
        value_size = sizeof (value);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (socket, options[i], &value, &value_size));
        TEST_ASSERT_EQUAL_UINT64 (0, value);
    }
}

void test_pair_immediate_async_admission_has_zero_op_id_and_no_callback ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://send-async-pair-immediate"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://send-async-pair-immediate"));
    msleep (SETTLE_TIME);

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 99;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (sender, {"pair-immediate"}, &options, &op_id));
    msleep (25);

    std::printf ("pair_immediate_async op_id=%llu completion_count=%zu\n",
                 static_cast<unsigned long long> (op_id),
                 completion_count (&completion));
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &parts, &part_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "pair-immediate", static_cast<const char *> (zlink_msg_data (&parts[0])),
      zlink_msg_size (&parts[0]));
    zlink_multipart_close (parts, part_count);
}

void test_completion_poller_drains_pending_send_eterm_after_context_shutdown ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_ctx_set (ctx, ZLINK_CTX_OPT_BLOCKY, 0));

    void *sender = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (sender);
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, sender, sender, ZLINK_POLLCOMPLETION));

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    const zlink_submit_result_t submit_result =
      submit_record (sender, {"shutdown-pending"}, &options, &op_id);
    const size_t completions_before_shutdown = completion_count (&completion);

    const zlink_close_result_t shutdown_result = zlink_ctx_shutdown (ctx);
    const size_t completions_after_shutdown = completion_count (&completion);

    zlink_poller_event_t poll_event;
    memset (&poll_event, 0, sizeof (poll_event));
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    const std::thread::id wait_thread = std::this_thread::get_id ();
    const int wait_result =
      zlink_poller_wait (poller, &poll_event, 1, 3000, &poll_error);

    completion_snapshot_t completion_event;
    memset (&completion_event, 0, sizeof (completion_event));
    std::thread::id callback_thread;
    size_t completions_after_wait = 0;
    {
        std::lock_guard<std::mutex> lock (completion.mutex);
        completions_after_wait = completion.events.size ();
        if (!completion.events.empty ())
            completion_event = completion.events[0];
        callback_thread = completion.callback_thread;
    }

    // Snapshot the observable result first, then release every private-context
    // resource before assertions can abort this test.
    const zlink_config_result_t remove_result =
      zlink_poller_remove (poller, sender);
    const zlink_close_result_t poller_close_result =
      zlink_poller_destroy (&poller);
    const zlink_close_result_t socket_close_result = zlink_close (sender);
    const size_t completions_after_close = completion_count (&completion);
    zlink_close_result_t term_result = ZLINK_CLOSE_INTERNAL_ERROR;
    if (socket_close_result == ZLINK_CLOSE_OK)
        term_result = zlink_ctx_term (ctx);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
    TEST_ASSERT_TRUE_MESSAGE (
      op_id != 0, "unconnected PAIR async send did not remain pending");
    TEST_ASSERT_EQUAL_UINT64 (0, completions_before_shutdown);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, shutdown_result);
    TEST_ASSERT_EQUAL_UINT64 (0, completions_after_shutdown);
    TEST_ASSERT_EQUAL_INT (1, wait_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_EQUAL_PTR (sender, poll_event.socket);
    TEST_ASSERT_TRUE ((poll_event.events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, completions_after_wait);
    TEST_ASSERT_EQUAL_UINT64 (op_id, completion_event.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, completion_event.result);
    TEST_ASSERT_EQUAL_INT (ETERM, completion_event.terminal_errno);
    TEST_ASSERT_TRUE (callback_thread == wait_thread);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, remove_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, poller_close_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, socket_close_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completions_after_close);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, term_result);
}

void test_completion_poller_does_not_publish_send_redrive_as_completion ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t hwm = 1024;
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://send-async-poller-redrive"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://send-async-poller-redrive"));

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, sender, sender, ZLINK_POLLCOMPLETION));

    const std::string filler_payload (400, 'F');
    zlink_msg_t filler;
    init_part (&filler, filler_payload);
    const zlink_submit_result_t filler_result = zlink_send_part (
      sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));

    const std::vector<std::string> pending_record = {
      std::string (128, 'A'), std::string (128, 'B'),
      std::string (400, 'C')};
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    const zlink_submit_result_t submit_result =
      submit_record (sender, pending_record, &options, &op_id);

    // Hold the async mailbox driver at its blocked-target gate. The poller can
    // then consume the real HWM-release command while admission is still
    // owned by that worker; releasing this hook makes the worker, rather than
    // the waiter, create the completion after the original command wake.
    gate_release_probe_t async_gate;
    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    sender_handle.socket->test_set_send_pending_gate_release_hook (
      &block_first_gate_release, &async_gate);
    sender_handle.socket->notify_incremental_send_released ();
    bool async_gate_entered = false;
    {
        std::unique_lock<std::mutex> lock (async_gate.mutex);
        async_gate_entered = async_gate.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&async_gate] { return async_gate.entered; });
    }

    std::atomic<bool> wait_started (false);
    std::atomic<bool> wait_returned (false);
    int first_wait_result = -1;
    zlink_config_result_t first_wait_error = ZLINK_CONFIG_INTERNAL_ERROR;
    zlink_poller_event_t first_event;
    memset (&first_event, 0, sizeof (first_event));
    size_t completions_at_first_return = 0;
    std::thread::id wait_thread_id;
    std::thread waiter ([&] () {
        wait_thread_id = std::this_thread::get_id ();
        wait_started.store (true, std::memory_order_release);
        first_wait_result = zlink_poller_wait (
          poller, &first_event, 1, 3000, &first_wait_error);
        completions_at_first_return = completion_count (&completion);
        wait_returned.store (true, std::memory_order_release);
    });

    while (!wait_started.load (std::memory_order_acquire))
        std::this_thread::yield ();
    // Any submit-side progress/redrive wake is internal. It may wake the
    // native poller, but no public completion exists until receiver credit is
    // returned and the send callback runs on this waiter.
    msleep (100);
    const bool returned_before_credit =
      wait_returned.load (std::memory_order_acquire);

    const bool filler_received = recv_pair_record_eventually (
      receiver, std::vector<std::string> (1, filler_payload));
    msleep (100);
    const bool returned_while_async_gate_owned =
      wait_returned.load (std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock (async_gate.mutex);
        async_gate.release = true;
    }
    async_gate.changed.notify_all ();
    waiter.join ();
    sender_handle.socket->test_set_send_pending_gate_release_hook (NULL, NULL);
    sender_handle = socket_handle_t ();

    // Keep cleanup bounded even when the first wait exposed the regression
    // and consumed only the internal redrive wake.
    int recovery_wait_result = 0;
    if (completion_count (&completion) == 0) {
        zlink_poller_event_t recovery_event;
        memset (&recovery_event, 0, sizeof (recovery_event));
        recovery_wait_result =
          zlink_poller_wait (poller, &recovery_event, 1, 3000, NULL);
    }
    const bool pending_record_received =
      recv_pair_record_eventually (receiver, pending_record);

    std::thread::id callback_thread;
    {
        std::lock_guard<std::mutex> lock (completion.mutex);
        callback_thread = completion.callback_thread;
    }
    const size_t final_completion_count = completion_count (&completion);
    completion_snapshot_t completion_event;
    memset (&completion_event, 0, sizeof (completion_event));
    if (final_completion_count != 0)
        completion_event = completion_at (&completion, 0);
    const zlink_config_result_t remove_result =
      zlink_poller_remove (poller, sender);
    const zlink_close_result_t poller_close_result =
      zlink_poller_destroy (&poller);
    const bool sender_closed = close_test_socket_eventually (sender);
    const bool receiver_closed = close_test_socket_eventually (receiver);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, filler_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
    TEST_ASSERT_TRUE_MESSAGE (
      op_id != 0, "poller regression did not create a pending send");
    TEST_ASSERT_TRUE_MESSAGE (
      async_gate_entered,
      "async mailbox did not own the blocked admission pass");
    TEST_ASSERT_FALSE_MESSAGE (
      returned_before_credit,
      "send redrive wake escaped as POLLCOMPLETION before its callback");
    TEST_ASSERT_FALSE_MESSAGE (
      returned_while_async_gate_owned,
      "poller completed while the async mailbox still owned admission");
    TEST_ASSERT_EQUAL_INT (1, first_wait_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, first_wait_error);
    TEST_ASSERT_EQUAL_PTR (sender, first_event.socket);
    TEST_ASSERT_TRUE (
      (first_event.events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, completions_at_first_return);
    TEST_ASSERT_TRUE (callback_thread == wait_thread_id);
    TEST_ASSERT_EQUAL_UINT64 (1, final_completion_count);
    TEST_ASSERT_EQUAL_UINT64 (op_id, completion_event.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion_event.result);
    TEST_ASSERT_EQUAL_INT (0, completion_event.terminal_errno);
    TEST_ASSERT_EQUAL_INT (0, recovery_wait_result);
    TEST_ASSERT_TRUE (filler_received);
    TEST_ASSERT_TRUE (pending_record_received);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, remove_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, poller_close_result);
    TEST_ASSERT_TRUE (sender_closed);
    TEST_ASSERT_TRUE (receiver_closed);
}

void test_pair_async_multipart_retries_pristine_after_later_frame_hwm ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t hwm = 1024;
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://send-async-pair-pristine-retry"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://send-async-pair-pristine-retry"));

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    const std::string filler_payload (400, 'F');
    zlink_msg_t filler;
    init_part (&filler, filler_payload);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));

    // The unread filler plus the first two frames fit, while adding the third
    // frame crosses byte HWM. Physical admission must roll the attempt back
    // and retain a pristine Core-owned record for the credit-driven retry.
    const std::vector<std::string> record = {
      std::string (128, 'A'), std::string (128, 'B'), std::string (400, 'C')};
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, submit_record (sender, record, &options, &op_id));
    TEST_ASSERT_TRUE_MESSAGE (
      op_id != 0,
      "later-frame byte-HWM did not leave the PAIR async record pending");
    TEST_ASSERT_FALSE_MESSAGE (
      wait_for_completion_count (&completion, 1, 100),
      "PAIR async multipart completed before byte-HWM credit was returned");

    TEST_ASSERT_TRUE (recv_pair_record_eventually (
      receiver, std::vector<std::string> (1, filler_payload)));
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_completion_count (&completion, 1),
      "PAIR async multipart was not redriven after byte-HWM credit");
    const completion_snapshot_t event = completion_at (&completion, 0);
    TEST_ASSERT_EQUAL_UINT64 (op_id, event.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, event.result);
    TEST_ASSERT_EQUAL_INT (0, event.terminal_errno);
    TEST_ASSERT_TRUE_MESSAGE (
      recv_pair_record_eventually (receiver, record),
      "PAIR async multipart retry did not preserve its pristine record");

    msleep (50);
    TEST_ASSERT_EQUAL_UINT64 (1, completion_count (&completion));
    TEST_ASSERT_TRUE_MESSAGE (
      pair_has_no_record_for (receiver),
      "PAIR async multipart retry admitted a duplicate or partial record");

    const bool sender_closed = close_test_socket_eventually (sender);
    const bool receiver_closed = close_test_socket_eventually (receiver);
    TEST_ASSERT_TRUE_MESSAGE (
      sender_closed, "PAIR async sender did not close after callback epilogue");
    TEST_ASSERT_TRUE (receiver_closed);
}

void test_pair_pending_multipart_peer_detach_completes_terminal_once ()
{
    // No existing test hook stops the pending driver between claim and its
    // physical attempt. Repeated bounded detach runs exercise both public
    // owner orderings while preserving an exact observable terminal contract.
    for (int round = 0; round != 8; ++round) {
        void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
        void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
        const uint64_t hwm = 1024;
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://send-async-pair-pending-detach-%d", round);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, endpoint));

        completion_probe_t completion;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_HANDLER_OK,
          zlink_send_complete_handler (sender, &capture_completion,
                                        &completion));

        zlink_msg_t filler;
        init_part (&filler, std::string (400, 'F'));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));

        const std::vector<std::string> record = {
          std::string (128, 'A'), std::string (128, 'B'),
          std::string (400, 'C')};
        zlink_send_async_options_t options;
        memset (&options, 0, sizeof (options));
        options.struct_size = sizeof (options);
        zlink_send_op_id_t op_id = 0;
        const zlink_submit_result_t submit_result =
          submit_record (sender, record, &options, &op_id);
        const bool pending_before_detach =
          submit_result == ZLINK_SUBMIT_OK && op_id != 0
          && !wait_for_completion_count (&completion, 1, 100);

        const bool receiver_closed = close_test_socket_eventually (receiver);
        const bool terminal_before_local_close =
          wait_for_completion_count (&completion, 1);
        completion_snapshot_t event;
        memset (&event, 0, sizeof (event));
        if (terminal_before_local_close)
            event = completion_at (&completion, 0);
        const size_t count_before_local_close = completion_count (&completion);

        // Local close is also the bounded cleanup path for a regression that
        // loses the peer-detach handoff. Assertions run only afterward.
        const bool sender_closed = close_test_socket_eventually (sender);
        if (!terminal_before_local_close)
            (void) wait_for_completion_count (&completion, 1, 100);
        const size_t final_completion_count = completion_count (&completion);

        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
        TEST_ASSERT_TRUE (receiver_closed);
        TEST_ASSERT_TRUE_MESSAGE (
          sender_closed,
          "PAIR pending sender did not close after callback epilogue");
        TEST_ASSERT_TRUE_MESSAGE (
          pending_before_detach,
          "PAIR detach regression did not begin with one pending multipart record");
        TEST_ASSERT_TRUE_MESSAGE (
          terminal_before_local_close,
          "PAIR peer detach did not resolve pending multipart before local close");
        TEST_ASSERT_EQUAL_UINT64 (1, count_before_local_close);
        TEST_ASSERT_EQUAL_UINT64 (op_id, event.op_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, event.result);
        TEST_ASSERT_EQUAL_INT (ENOTCONN, event.terminal_errno);
        TEST_ASSERT_EQUAL_UINT64 (1, final_completion_count);
    }
}

void test_inline_terminal_completion_can_self_close_before_submit_returns ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    completion_self_close_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (
        router, &capture_completion_and_self_close, &probe));

    zlink_routed_submit_target_t missing_target;
    memset (&missing_target, 0, sizeof (missing_target));
    missing_target.peer_rid = make_rid ("missing-self-close-peer");
    missing_target.transport_pair_id = 1;
    missing_target.transport_pair_generation = 1;

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &missing_target;

    zlink_msg_t part;
    init_part (&part, "terminal-self-close");
    zlink_send_op_id_t op_id = 0;
    errno = 0;
    const zlink_submit_result_t submit_result =
      zlink_send_async (router, &part, 1, &options, &op_id);

    bool callback_done = false;
    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        callback_done = probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&probe] {
              return probe.done && probe.active_callbacks == 0;
          });
    }

    size_t callback_count = 0;
    zlink_send_op_id_t completion_op_id = 0;
    zlink_send_complete_result_t completion_result = ZLINK_SEND_ADMITTED;
    int terminal_errno = 0;
    zlink_close_result_t close_result = ZLINK_CLOSE_BUSY;
    if (callback_done) {
        std::lock_guard<std::mutex> lock (probe.mutex);
        callback_count = probe.callback_count;
        completion_op_id = probe.op_id;
        completion_result = probe.result;
        terminal_errno = probe.terminal_errno;
        close_result = probe.close_result;
    }

    if (callback_done && close_result == ZLINK_CLOSE_OK)
        test_context_socket_mark_closed (router);
    else
        test_context_socket_close_zero_linger (router);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit_result);
    TEST_ASSERT_TRUE (op_id != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_TRUE_MESSAGE (
      callback_done, "inline terminal completion was not dispatched");
    TEST_ASSERT_EQUAL_UINT64 (1, callback_count);
    TEST_ASSERT_EQUAL_UINT64 (op_id, completion_op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, completion_result);
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, terminal_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_result);
}

void test_send_pending_sequence_exhaustion_never_assigns_zero ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    socket_handle_t sender_handle = as_socket_handle (sender);
    sender_handle.socket->test_set_send_next_op_id (0);
    sender_handle = socket_handle_t ();

    zlink_msg_t part;
    init_part (&part, "sequence-exhausted");
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 77;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_SEQ_EXHAUSTED,
      zlink_send_async (sender, &part, 1, &options, &op_id));
    TEST_ASSERT_EQUAL_INT (EOVERFLOW, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void test_queue_index_allocation_failure_rolls_back_without_ownership_transfer ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    sender_handle.socket->test_set_send_fail_after_queue_push (true);
    sender_handle = socket_handle_t ();

    char payload[] = "queue-index-transaction";
    free_probe_t released;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (
      &part, payload, sizeof (payload) - 1, &count_free_synchronized,
      &released));
    zlink::msg_t *native = reinterpret_cast<zlink::msg_t *> (&part);
    native->set_flags (zlink::msg_t::more);

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 77;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OUT_OF_MEMORY,
      zlink_send_async (sender, &part, 1, &options, &op_id));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_EQUAL_INT (0, free_count (&released));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&part),
                              sizeof (payload) - 1);
    TEST_ASSERT_TRUE ((native->flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));

    sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    sender_handle.socket->test_set_send_fail_after_queue_push (false);
    sender_handle = socket_handle_t ();

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_async (sender, &part, 1, &options, &op_id));
    TEST_ASSERT_EQUAL_UINT64 (1, op_id);
    TEST_ASSERT_EQUAL_INT (0, free_count (&released));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send_async_cancel (sender, op_id));
    TEST_ASSERT_TRUE (wait_for_completion_count (&completion, 1));
    const completion_snapshot_t event = completion_at (&completion, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, event.result);
    TEST_ASSERT_EQUAL_INT (ECANCELED, event.terminal_errno);
    TEST_ASSERT_TRUE (wait_for_free_count (&released, 1));
    TEST_ASSERT_EQUAL_INT (1, free_count (&released));
}

void test_close_joins_firing_send_deadline_before_returning ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://send-deadline-close-join"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://send-deadline-close-join"));
    msleep (SETTLE_TIME);

    bool backpressured = false;
    for (int i = 0; i != 32 && !backpressured; ++i) {
        zlink_msg_t filler;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&filler, 65536));
        const zlink_submit_result_t result = zlink_send_part (
          sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
        TEST_ASSERT_TRUE (result == ZLINK_SUBMIT_OK
                          || result == ZLINK_SUBMIT_BACKPRESSURED);
        backpressured = result == ZLINK_SUBMIT_BACKPRESSURED;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));
    }
    TEST_ASSERT_TRUE (backpressured);

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    deadline_enqueue_probe_t deadline;
    socket_handle_t sender_handle = as_socket_handle (sender);
    TEST_ASSERT_NOT_NULL (sender_handle.socket);
    sender_handle.socket->test_set_send_deadline_enqueue_hook (
      &block_deadline_enqueue, &deadline);
    sender_handle = socket_handle_t ();

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.timeout_ms = 10;
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (sender, {std::string (65536, 'd')}, &options, &op_id));
    TEST_ASSERT_TRUE (op_id != 0);

    {
        std::unique_lock<std::mutex> lock (deadline.mutex);
        TEST_ASSERT_TRUE (deadline.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&deadline] { return deadline.entered; }));
    }

    const int zero_linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      sender, ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger)));
    std::atomic<bool> close_done (false);
    int close_rc = -1;
    std::thread closer ([&] () {
        close_rc = zlink_close (sender);
        close_done.store (true, std::memory_order_release);
    });

    msleep (30);
    const bool returned_before_deadline_released =
      close_done.load (std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock (deadline.mutex);
        deadline.release = true;
    }
    deadline.changed.notify_all ();
    closer.join ();
    test_context_socket_mark_closed (sender);

    TEST_ASSERT_FALSE_MESSAGE (
      returned_before_deadline_released,
      "close returned while the send deadline callback still owned its socket payload");
    TEST_ASSERT_EQUAL_INT (0, close_rc);
    TEST_ASSERT_EQUAL_UINT64 (1, completion_count (&completion));
    const completion_snapshot_t event = completion_at (&completion, 0);
    TEST_ASSERT_TRUE (event.result == ZLINK_SEND_TIMED_OUT
                      || event.result == ZLINK_SEND_TERMINAL);
    if (event.result == ZLINK_SEND_TIMED_OUT)
        TEST_ASSERT_EQUAL_INT (ETIMEDOUT, event.terminal_errno);
    else
        TEST_ASSERT_EQUAL_INT (ECANCELED, event.terminal_errno);
}

void test_multipart_sequence_exhaustion_preserves_caller_after_inline_rollback ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 1024;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, "exhausted-peer", 14));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://send-async-exhausted-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://send-async-exhausted-multipart"));

    const zlink_routing_id_t dealer_rid = make_rid ("exhausted-peer");
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (router, &dealer_rid);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, send_router_filler (router, target, 256));

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &completion));
    socket_handle_t router_handle = as_socket_handle (router);
    router_handle.socket->test_set_send_next_op_id (0);
    router_handle = socket_handle_t ();

    char first_payload[128];
    memset (first_payload, 'a', sizeof (first_payload));
    int free_count = 0;
    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (
      &parts[0], first_payload, sizeof (first_payload), &count_free,
      &free_count));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], 700));
    memset (zlink_msg_data (&parts[1]), 'b', 700);

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &target;
    zlink_send_op_id_t op_id = 77;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_SEQ_EXHAUSTED,
      zlink_send_async (router, parts, 2, &options, &op_id));
    TEST_ASSERT_EQUAL_INT (EOVERFLOW, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));
    TEST_ASSERT_EQUAL_INT (0, free_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (first_payload),
                              zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (first_payload, zlink_msg_data (&parts[0]),
                              sizeof (first_payload));
    TEST_ASSERT_EQUAL_UINT64 (700, zlink_msg_size (&parts[1]));
    TEST_ASSERT_EQUAL_INT ('b',
                           static_cast<const char *> (
                             zlink_msg_data (&parts[1]))[699]);

    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer, std::vector<std::string> (1, std::string (256, 'O'))));
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (dealer),
      "a rejected async multipart record leaked a partial part");

    zlink_multipart_close (parts, 2);
    TEST_ASSERT_EQUAL_INT (1, free_count);
}

void test_completion_callback_rejects_all_reentrant_submit_entry_points ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *different = test_context_socket (ZLINK_SOCKET_PAIR);
    void *request_socket = test_context_socket (ZLINK_SOCKET_DEALER);
    void *publish_socket = test_context_socket (ZLINK_SOCKET_PUB);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://completion-reentry-send"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://completion-reentry-send"));
    msleep (SETTLE_TIME);

    bool backpressured = false;
    for (int i = 0; i != 32 && !backpressured; ++i) {
        zlink_msg_t filler;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&filler, 65536));
        const zlink_submit_result_t fill = zlink_send_part (
          sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
        if (fill == ZLINK_SUBMIT_BACKPRESSURED) {
            backpressured = true;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, fill);
        }
    }
    TEST_ASSERT_TRUE (backpressured);

    completion_reentry_probe_t probe;
    probe.different_socket = different;
    probe.request_socket = request_socket;
    probe.publish_socket = publish_socket;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_reentrant_completion, &probe));

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (sender, {std::string (65536, 'r')}, &options, &op_id));
    TEST_ASSERT_TRUE (op_id != 0);

    zlink_msg_t drained;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&drained));
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (receiver, NULL, &drained, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&drained));

    {
        std::unique_lock<std::mutex> lock (probe.mutex);
        TEST_ASSERT_TRUE (probe.changed.wait_for (
          lock, std::chrono::seconds (3), [&probe] { return probe.done; }));
    }

    TEST_ASSERT_EQUAL_UINT64 (1, probe.callback_count);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, probe.completion_result);
    TEST_ASSERT_TRUE (probe.completion_op_id != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.same_send);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.same_send_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.different_send);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.different_send_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.async_send);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.async_send_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.different_async_send);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.different_async_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.publish);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.publish_errno);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_THREAD_VIOLATION, probe.request);
    TEST_ASSERT_EQUAL_INT (EDEADLK, probe.request_errno);

}

void test_router_send_async_fused_target_admits_single_and_multipart_records ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "async-router-peer", 17));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://send-async-router-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://send-async-router-multipart"));

    zlink_routing_id_t dealer_rid;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_get_routing_id (dealer, &dealer_rid));
    (void) select_router_target_eventually (router, &dealer_rid);
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    target.peer_rid = dealer_rid;

    completion_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &probe));

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &target;

    const std::vector<std::vector<std::string>> records = {
      {"router-one"},
      {"router-two-head", "router-two-tail"},
      {"router-three-head", "router-three-middle", "router-three-tail"}};
    for (size_t i = 0; i != records.size (); ++i) {
        const size_t before = completion_count (&probe);
        zlink_send_op_id_t op_id = 1;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          submit_record (router, records[i], &options, &op_id));
        TEST_ASSERT_EQUAL_UINT64 (0, op_id);
        TEST_ASSERT_EQUAL_UINT64 (before, completion_count (&probe));
        TEST_ASSERT_TRUE (recv_dealer_record_eventually (dealer, records[i]));
    }
}

void test_dealer_generic_target_admits_multipart_after_connect ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://send-async-dealer-generic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://send-async-dealer-generic"));

    //  Selecting only to wait for the application lane makes the test's
    //  target=NULL submit happen on a known connected socket state.
    (void) select_dealer_target_eventually (sender);

    completion_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &probe));
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = NULL;

    const std::vector<std::string> record = {
      "dealer-generic-head", "dealer-generic-middle", "dealer-generic-tail"};
    zlink_send_op_id_t op_id = 1;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           submit_record (sender, record, &options, &op_id));
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&probe));
    TEST_ASSERT_TRUE (recv_dealer_record_eventually (receiver, record));
}

void test_dealer_async_multipart_retries_pristine_after_later_frame_hwm ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 1024;
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sender, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://send-async-dealer-pristine-retry"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://send-async-dealer-pristine-retry"));
    (void) select_dealer_target_eventually (sender);

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    const std::string filler_payload (400, 'F');
    zlink_msg_t filler;
    init_part (&filler, filler_payload);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &filler, ZLINK_SEND_FLAGS_DONTWAIT,
                       ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&filler));

    const std::vector<std::string> record = {
      std::string (128, 'A'), std::string (128, 'B'), std::string (400, 'C')};
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, submit_record (sender, record, &options, &op_id));
    TEST_ASSERT_TRUE_MESSAGE (
      op_id != 0,
      "later-frame byte-HWM did not leave the DEALER async record pending");
    TEST_ASSERT_FALSE_MESSAGE (
      wait_for_completion_count (&completion, 1, 100),
      "DEALER async multipart completed before byte-HWM credit was returned");

    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      receiver, std::vector<std::string> (1, filler_payload)));
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_completion_count (&completion, 1),
      "DEALER async multipart was not redriven after byte-HWM credit");
    const completion_snapshot_t event = completion_at (&completion, 0);
    TEST_ASSERT_EQUAL_UINT64 (op_id, event.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, event.result);
    TEST_ASSERT_EQUAL_INT (0, event.terminal_errno);
    TEST_ASSERT_TRUE_MESSAGE (
      recv_dealer_record_eventually (receiver, record),
      "DEALER async multipart retry did not preserve its pristine record");

    msleep (50);
    TEST_ASSERT_EQUAL_UINT64 (1, completion_count (&completion));
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (receiver),
      "DEALER async multipart retry admitted a duplicate or partial record");

    const bool sender_closed = close_test_socket_eventually (sender);
    const bool receiver_closed = close_test_socket_eventually (receiver);
    TEST_ASSERT_TRUE_MESSAGE (
      sender_closed, "DEALER async sender did not close after callback epilogue");
    TEST_ASSERT_TRUE (receiver_closed);
}

void test_router_async_multipart_retries_pristine_after_later_frame_hwm ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 1024;
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "atomic-peer", 11));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://send-async-atomic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://send-async-atomic"));

    const zlink_routing_id_t dealer_rid = make_rid ("atomic-peer");
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (router, &dealer_rid);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, send_router_filler (router, target, 256));

    completion_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &probe));
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &target;

    const std::vector<std::string> record = {
      std::string (128, 'a'), std::string (700, 'b')};
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, submit_record (router, record, &options, &op_id));
    TEST_ASSERT_TRUE_MESSAGE (
      op_id != 0,
      "later-frame byte-HWM did not leave the ROUTER async record pending");
    TEST_ASSERT_FALSE_MESSAGE (
      wait_for_completion_count (&probe, 1, 100),
      "ROUTER async multipart completed before byte-HWM credit was returned");

    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer, std::vector<std::string> (1, std::string (256, 'O'))));
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_completion_count (&probe, 1),
      "ROUTER async multipart was not redriven after byte-HWM credit");
    const completion_snapshot_t completion = completion_at (&probe, 0);
    TEST_ASSERT_EQUAL_UINT64 (op_id, completion.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.result);
    TEST_ASSERT_EQUAL_INT (0, completion.terminal_errno);
    TEST_ASSERT_TRUE_MESSAGE (
      recv_dealer_record_eventually (dealer, record),
      "ROUTER async multipart retry did not preserve its pristine record");

    msleep (50);
    TEST_ASSERT_EQUAL_UINT64 (1, completion_count (&probe));
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (dealer),
      "ROUTER async multipart retry admitted a duplicate or partial record");

    const bool router_closed = close_test_socket_eventually (router);
    const bool dealer_closed = close_test_socket_eventually (dealer);
    TEST_ASSERT_TRUE_MESSAGE (
      router_closed, "ROUTER async sender did not close after callback epilogue");
    TEST_ASSERT_TRUE (dealer_closed);
}

void test_pending_driver_rechecks_enqueue_racing_gate_release ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_a, "handoff-a", 9));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_b, "handoff-b", 9));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_b, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://send-async-handoff"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_a, "inproc://send-async-handoff"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_b, "inproc://send-async-handoff"));

    const zlink_routing_id_t rid_a = make_rid ("handoff-a");
    const zlink_routing_id_t rid_b = make_rid ("handoff-b");
    const zlink_routed_submit_target_t target_a =
      select_router_target_eventually (router, &rid_a);
    const zlink_routed_submit_target_t target_b =
      select_router_target_eventually (router, &rid_b);

    bool reached_backpressure = false;
    for (int i = 0; i != 32 && !reached_backpressure; ++i) {
        const zlink_submit_result_t result =
          send_router_filler (router, target_a, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            reached_backpressure = true;
        else
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (reached_backpressure);

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &completion));

    gate_release_probe_t handoff;
    socket_handle_t router_handle = as_socket_handle (router);
    zlink::socket_base_t *socket = router_handle.socket;
    socket->test_set_send_pending_gate_release_hook (
      &block_first_gate_release, &handoff);

    zlink_submit_result_t first_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_send_op_id_t first_op_id = 0;
    std::thread first_submit ([&] () {
        zlink_send_async_options_t options;
        memset (&options, 0, sizeof (options));
        options.struct_size = sizeof (options);
        options.target = &target_a;
        first_result = submit_record (
          router, {"blocked-before-handoff"}, &options, &first_op_id);
    });

    {
        std::unique_lock<std::mutex> lock (handoff.mutex);
        TEST_ASSERT_TRUE (handoff.changed.wait_for (
          lock, std::chrono::seconds (3), [&handoff] { return handoff.entered; }));
    }

    zlink_send_async_options_t options_b;
    memset (&options_b, 0, sizeof (options_b));
    options_b.struct_size = sizeof (options_b);
    options_b.target = &target_b;
    zlink_send_op_id_t second_op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (router, {"published-during-handoff"}, &options_b,
                     &second_op_id));
    TEST_ASSERT_TRUE (second_op_id != 0);

    {
        std::lock_guard<std::mutex> lock (handoff.mutex);
        handoff.release = true;
    }
    handoff.changed.notify_all ();
    first_submit.join ();
    socket->test_set_send_pending_gate_release_hook (NULL, NULL);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, first_result);
    TEST_ASSERT_TRUE (first_op_id != 0);
    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer_b, {"published-during-handoff"}));
    TEST_ASSERT_TRUE (wait_for_completion_count (&completion, 1));

    // Release A so teardown does not leave the deliberately blocked record.
    TEST_ASSERT_TRUE (drain_dealer_parts (dealer_a) > 0);
    TEST_ASSERT_TRUE (wait_for_completion_count (&completion, 2));
}

void test_copied_single_probe_rejection_preserves_caller_more_flag ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "flag-peer", 9));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://send-async-flags"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://send-async-flags"));

    const zlink_routing_id_t rid = make_rid ("flag-peer");
    const zlink_routed_submit_target_t target =
      select_router_target_eventually (router, &rid);
    bool reached_backpressure = false;
    for (int i = 0; i != 32 && !reached_backpressure; ++i) {
        const zlink_submit_result_t result =
          send_router_filler (router, target, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            reached_backpressure = true;
        else
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (reached_backpressure);

    const uint64_t pending_byte_limit = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_SEND_PENDING_MAX_BYTES, &pending_byte_limit,
      sizeof (pending_byte_limit)));
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &completion));

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 64));
    zlink::msg_t *native = reinterpret_cast<zlink::msg_t *> (&part);
    native->set_flags (zlink::msg_t::more);

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    zlink_routed_submit_target_t rid_only_target = target;
    rid_only_target.transport_pair_id = 0;
    rid_only_target.transport_pair_generation = 0;
    options.target = &rid_only_target;
    zlink_send_op_id_t op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_async (router, &part, 1, &options, &op_id));
    TEST_ASSERT_EQUAL_UINT64 (0, op_id);
    TEST_ASSERT_TRUE (native->check ());
    TEST_ASSERT_TRUE ((native->flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

void test_pending_multipart_admission_wakes_once_after_final_release ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_a, "gate-final-a", 12));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_b, "gate-final-b", 12));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_b, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://send-async-incremental-release"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_a, "inproc://send-async-incremental-release"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_b, "inproc://send-async-incremental-release"));

    const zlink_routing_id_t rid_a = make_rid ("gate-final-a");
    const zlink_routing_id_t rid_b = make_rid ("gate-final-b");
    const zlink_routed_submit_target_t target_a =
      select_router_target_eventually (router, &rid_a);
    const zlink_routed_submit_target_t target_b =
      select_router_target_eventually (router, &rid_b);

    bool reached_backpressure = false;
    for (int i = 0; i != 32 && !reached_backpressure; ++i) {
        const zlink_submit_result_t result =
          send_router_filler (router, target_a, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            reached_backpressure = true;
        else
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (reached_backpressure);

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &completion));
    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &target_a;
    const std::vector<std::string> pending_record = {
      "async-after-marker-head", "async-after-marker-tail"};
    zlink_send_op_id_t pending_op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (router, pending_record, &options, &pending_op_id));
    TEST_ASSERT_TRUE (pending_op_id != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));

    zlink_submit_result_t sync_head_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    int sync_head_errno = 0;
    // A mailbox retry can still own the short complete-record scope just
    // after submit_record() returns. Wait until that physical attempt leaves;
    // the multipart marker, once admitted, is the state this test exercises.
    for (int attempt = 0; attempt != 100; ++attempt) {
        zlink_msg_t sync_head;
        init_part (&sync_head, "sync-head");
        errno = 0;
        sync_head_result = zlink_send_part_transport_pair (
          router, &target_b.peer_rid, target_b.transport_pair_id,
          target_b.transport_pair_generation, &sync_head,
          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE);
        sync_head_errno = errno;
        if (sync_head_result == ZLINK_SUBMIT_OK)
            break;
        if (sync_head_result != ZLINK_SUBMIT_INVALID_ARGUMENT
            || sync_head_errno != EINVAL)
            break;
        msleep (1);
    }

    gate_release_probe_t blocked_pass;
    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    zlink::socket_base_t *const socket = router_handle.socket;
    socket->test_set_send_pending_gate_release_hook (
      &block_first_gate_release, &blocked_pass);

    // Returning A's credit wakes the driver after MORE has returned and
    // released only the sync bit. Physical complete-record admission must
    // still see the lifecycle multipart marker and leave the record pending.
    const int drained = drain_dealer_parts (dealer_a);
    bool marker_pass_observed = false;
    {
        std::unique_lock<std::mutex> lock (blocked_pass.mutex);
        marker_pass_observed = blocked_pass.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&blocked_pass] { return blocked_pass.entered; });
    }
    const bool completion_before_marker_release =
      completion_count (&completion) != 0;
    const bool record_visible_before_marker_release =
      !dealer_has_no_part (dealer_a, 25);

    zlink_msg_t sync_tail;
    init_part (&sync_tail, "sync-tail");
    errno = 0;
    const zlink_submit_result_t sync_tail_result =
      zlink_send_part_transport_pair (
        router, &target_b.peer_rid, target_b.transport_pair_id,
        target_b.transport_pair_generation, &sync_tail,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    const int sync_tail_errno = errno;

    // FINAL clears the marker and emits the only new wake while the marker-
    // blocked pass still owns admission_gate. Releasing the hook fixes the
    // wake/admission handoff order without adding another HWM edge.
    {
        std::lock_guard<std::mutex> lock (blocked_pass.mutex);
        blocked_pass.release = true;
    }
    blocked_pass.changed.notify_all ();

    const bool completion_after_marker_release =
      wait_for_completion_count (&completion, 1);
    const bool record_visible_without_completion =
      !completion_after_marker_release
      && !dealer_has_no_part (dealer_a, 25);
    const bool duplicate_completion =
      completion_after_marker_release
      && wait_for_completion_count (&completion, 2, 100);
    socket->test_set_send_pending_gate_release_hook (NULL, NULL);
    router_handle = socket_handle_t ();

    TEST_ASSERT_TRUE (drained > 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, sync_head_result);
    TEST_ASSERT_EQUAL_INT (0, sync_head_errno);
    TEST_ASSERT_TRUE_MESSAGE (
      marker_pass_observed,
      "pending driver did not retry against the active multipart marker");
    TEST_ASSERT_FALSE_MESSAGE (
      completion_before_marker_release,
      "pending record completed while an incremental multipart marker was active");
    TEST_ASSERT_FALSE_MESSAGE (
      record_visible_before_marker_release,
      "pending record reached its pipe while an incremental multipart marker was active");
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, sync_tail_result);
    TEST_ASSERT_EQUAL_INT (0, sync_tail_errno);

    if (!completion_after_marker_release) {
        if (record_visible_without_completion)
            TEST_FAIL_MESSAGE (
              "pending record was physically admitted but its completion was not dispatched");
        TEST_FAIL_MESSAGE (
          "pending record remained blocked after the incremental-release wake");
    }
    TEST_ASSERT_TRUE (completion_after_marker_release);
    TEST_ASSERT_FALSE_MESSAGE (
      duplicate_completion,
      "one pending operation produced more than one completion");
    TEST_ASSERT_EQUAL_UINT64 (1, completion_count (&completion));
    const completion_snapshot_t async_completion = completion_at (&completion, 0);
    TEST_ASSERT_EQUAL_UINT64 (pending_op_id, async_completion.op_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, async_completion.result);
    TEST_ASSERT_EQUAL_INT (0, async_completion.terminal_errno);

    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer_b, {"sync-head", "sync-tail"}));
    TEST_ASSERT_TRUE (recv_dealer_record_eventually (dealer_a, pending_record));
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (dealer_a, 50),
      "one pending operation became visible more than once");
}

void test_peer_rid_single_fallback_reserves_fifo_before_queue_publication ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, "fifo-peer", 9));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://send-async-peer-rid-fifo"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://send-async-peer-rid-fifo"));

    const zlink_routing_id_t rid = make_rid ("fifo-peer");
    const zlink_routed_submit_target_t exact_target =
      select_router_target_eventually (router, &rid);
    zlink_routed_submit_target_t rid_only_target = exact_target;
    rid_only_target.transport_pair_id = 0;
    rid_only_target.transport_pair_generation = 0;

    int filler_count = 0;
    bool reached_backpressure = false;
    for (int i = 0; i != 32 && !reached_backpressure; ++i) {
        const zlink_submit_result_t result =
          send_router_filler (router, exact_target, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            reached_backpressure = true;
        else {
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
            ++filler_count;
        }
    }
    TEST_ASSERT_TRUE (reached_backpressure);
    TEST_ASSERT_TRUE (filler_count > 0);

    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (router, &capture_completion, &completion));

    inline_fallback_probe_t fallback;
    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    router_handle.socket->test_set_send_inline_fallback_hook (
      &block_inline_fallback, &fallback);
    router_handle = socket_handle_t ();

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &rid_only_target;

    zlink_msg_t first_part;
    init_part (&first_part, "fifo-first");
    zlink_submit_result_t first_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_send_op_id_t first_op_id = 0;
    size_t first_size_after = static_cast<size_t> (-1);
    std::thread first_submit ([&] () {
        errno = 0;
        first_result = zlink_send_async (
          router, &first_part, 1, &options, &first_op_id);
        first_size_after = zlink_msg_size (&first_part);
    });

    {
        std::unique_lock<std::mutex> lock (fallback.mutex);
        TEST_ASSERT_TRUE (fallback.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&fallback] { return fallback.entered; }));
    }

    //  The later record is published while the earlier record is paused
    //  after its physical EAGAIN but before its queue insertion. It must not
    //  acquire the admission gate or become peer-visible first.
    zlink_msg_t second_part;
    init_part (&second_part, "fifo-second");
    zlink_send_op_id_t second_op_id = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_async (router, &second_part, 1, &options, &second_op_id));
    TEST_ASSERT_TRUE (second_op_id != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&second_part));

    //  A rejected follower still owns its native part and never receives an
    //  operation id. Temporarily cap the already-published pending queue,
    //  then restore the normal unlimited policy before the first submit is
    //  allowed to publish.
    const uint64_t one_pending = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_SEND_PENDING_MAX_MSGS, &one_pending,
      sizeof (one_pending)));
    zlink_msg_t rejected_part;
    init_part (&rejected_part, "fifo-rejected");
    zlink_send_op_id_t rejected_op_id = 77;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_send_async (
        router, &rejected_part, 1, &options, &rejected_op_id));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_op_id);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("fifo-rejected"),
                              zlink_msg_size (&rejected_part));
    TEST_ASSERT_EQUAL_MEMORY (
      "fifo-rejected", zlink_msg_data (&rejected_part),
      strlen ("fifo-rejected"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&rejected_part));
    const uint64_t unlimited_pending = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      router, ZLINK_OPT_SEND_PENDING_MAX_MSGS, &unlimited_pending,
      sizeof (unlimited_pending)));

    const std::vector<std::string> filler_record (
      1, std::string (65536, 'O'));
    for (int i = 0; i != filler_count; ++i)
        TEST_ASSERT_TRUE (
          recv_dealer_record_eventually (dealer, filler_record));

    TEST_ASSERT_FALSE_MESSAGE (
      wait_for_completion_count (&completion, 1, 100),
      "later same-target record admitted before the earlier queue publication");
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (dealer),
      "later same-target record became visible before the earlier record");

    {
        std::lock_guard<std::mutex> lock (fallback.mutex);
        fallback.release = true;
    }
    fallback.changed.notify_all ();
    first_submit.join ();

    router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);
    router_handle.socket->test_set_send_inline_fallback_hook (NULL, NULL);
    router_handle = socket_handle_t ();

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, first_result);
    TEST_ASSERT_TRUE (first_op_id != 0);
    TEST_ASSERT_EQUAL_UINT64 (0, first_size_after);
    TEST_ASSERT_TRUE (wait_for_completion_count (&completion, 2));
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                           completion_at (&completion, 0).result);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED,
                           completion_at (&completion, 1).result);
    TEST_ASSERT_TRUE (
      recv_dealer_record_eventually (dealer, {"fifo-first"}));
    TEST_ASSERT_TRUE (
      recv_dealer_record_eventually (dealer, {"fifo-second"}));
}

int main ()
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (test_send_pending_limits_default_to_unlimited_and_accept_zero);
    RUN_TEST (test_pair_immediate_async_admission_has_zero_op_id_and_no_callback);
    RUN_TEST (
      test_completion_poller_drains_pending_send_eterm_after_context_shutdown);
    RUN_TEST (
      test_completion_poller_does_not_publish_send_redrive_as_completion);
    RUN_TEST (test_pair_async_multipart_retries_pristine_after_later_frame_hwm);
    RUN_TEST (test_pair_pending_multipart_peer_detach_completes_terminal_once);
    RUN_TEST (test_inline_terminal_completion_can_self_close_before_submit_returns);
    RUN_TEST (test_send_pending_sequence_exhaustion_never_assigns_zero);
    RUN_TEST (test_queue_index_allocation_failure_rolls_back_without_ownership_transfer);
    RUN_TEST (test_close_joins_firing_send_deadline_before_returning);
    RUN_TEST (test_multipart_sequence_exhaustion_preserves_caller_after_inline_rollback);
    RUN_TEST (test_completion_callback_rejects_all_reentrant_submit_entry_points);
    RUN_TEST (test_router_send_async_fused_target_admits_single_and_multipart_records);
    RUN_TEST (test_dealer_generic_target_admits_multipart_after_connect);
    RUN_TEST (
      test_dealer_async_multipart_retries_pristine_after_later_frame_hwm);
    RUN_TEST (
      test_router_async_multipart_retries_pristine_after_later_frame_hwm);
    RUN_TEST (test_pending_driver_rechecks_enqueue_racing_gate_release);
    RUN_TEST (test_copied_single_probe_rejection_preserves_caller_more_flag);
    RUN_TEST (test_pending_multipart_admission_wakes_once_after_final_release);
    RUN_TEST (test_peer_rid_single_fallback_reserves_fifo_before_queue_publication);
    return UNITY_END ();
}
