/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "sockets/common/socket_base.hpp"

#include <chrono>
#include <condition_variable>
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
    zlink_send_complete_result_t result;
    int terminal_errno;
};

struct completion_probe_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<completion_snapshot_t> events;
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

void init_part (zlink_msg_t *part_, const std::string &payload_);

void capture_completion (void *, const zlink_send_complete_event_t *event_, void *userdata_)
{
    completion_probe_t *probe = static_cast<completion_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        completion_snapshot_t snapshot;
        snapshot.result = event_->result;
        snapshot.terminal_errno = event_->terminal_errno;
        probe->events.push_back (snapshot);
    }
    probe->changed.notify_all ();
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
    }
    probe->changed.notify_all ();
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

struct sync_sequence_probe_t
{
    sync_sequence_probe_t () : first (ZLINK_SUBMIT_INTERNAL_ERROR),
                               second (ZLINK_SUBMIT_INTERNAL_ERROR),
                               first_errno (0), second_errno (0), first_done (false),
                               release (false)
    {
    }

    std::mutex mutex;
    std::condition_variable changed;
    zlink_submit_result_t first;
    zlink_submit_result_t second;
    int first_errno;
    int second_errno;
    bool first_done;
    bool release;
};

void run_sync_multipart_sequence (void *router_,
                                  const zlink_routed_submit_target_t &target_,
                                  sync_sequence_probe_t *probe_)
{
    zlink_msg_t first_part;
    init_part (&first_part, "sync-head");
    errno = 0;
    const zlink_submit_result_t first = zlink_send_part_transport_pair (
      router_, &target_.peer_rid, target_.transport_pair_id,
      target_.transport_pair_generation, &first_part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_MORE);
    const int first_errno = errno;
    {
        std::lock_guard<std::mutex> lock (probe_->mutex);
        probe_->first = first;
        probe_->first_errno = first_errno;
        probe_->first_done = true;
    }
    probe_->changed.notify_all ();
    if (first != ZLINK_SUBMIT_OK)
        return;

    {
        std::unique_lock<std::mutex> lock (probe_->mutex);
        probe_->changed.wait (lock, [probe_] { return probe_->release; });
    }

    zlink_msg_t second_part;
    init_part (&second_part, "sync-tail");
    errno = 0;
    const zlink_submit_result_t second = zlink_send_part_transport_pair (
      router_, &target_.peer_rid, target_.transport_pair_id,
      target_.transport_pair_generation, &second_part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    const int second_errno = errno;
    {
        std::lock_guard<std::mutex> lock (probe_->mutex);
        probe_->second = second;
        probe_->second_errno = second_errno;
    }
    probe_->changed.notify_all ();
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

void test_send_pending_sequence_exhaustion_never_assigns_zero ()
{
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    completion_probe_t completion;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (sender, &capture_completion, &completion));

    static_cast<zlink::socket_base_t *> (sender)->test_set_send_next_op_id (0);

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

    // `done` is published from the final statements of the callback, just
    // before the dispatch scope itself is released. Do not let the Unity
    // context teardown race that final scope release and observe close as
    // EBUSY on a slower scheduler.
    msleep (10);
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

void test_routed_multipart_async_rolls_back_when_later_part_hits_hwm ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 1024;
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
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           submit_record (router, record, &options));
    TEST_ASSERT_TRUE (wait_for_completion_count (&probe, 1));
    const completion_snapshot_t completion = completion_at (&probe, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, completion.result);
    TEST_ASSERT_NOT_EQUAL (0, completion.terminal_errno);

    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer, std::vector<std::string> (1, std::string (256, 'O'))));
    TEST_ASSERT_TRUE_MESSAGE (
      dealer_has_no_part (dealer),
      "a failed async multipart record leaked a partial part");
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
    zlink::socket_base_t *socket = static_cast<zlink::socket_base_t *> (router);
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

void test_borrowed_single_rejection_restores_caller_more_flag ()
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
    options.target = &target;
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

void test_pending_multipart_admission_waits_for_sync_sequence_gate ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_a, "gate-a", 6));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_b, "gate-b", 6));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer_b, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://send-async-gate"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_a, "inproc://send-async-gate"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_b, "inproc://send-async-gate"));

    const zlink_routing_id_t rid_a = make_rid ("gate-a");
    const zlink_routing_id_t rid_b = make_rid ("gate-b");
    const zlink_routed_submit_target_t target_a =
      select_router_target_eventually (router, &rid_a);
    const zlink_routed_submit_target_t target_b =
      select_router_target_eventually (router, &rid_b);

    bool reached_backpressure = false;
    for (int i = 0; i != 32 && !reached_backpressure; ++i) {
        const zlink_submit_result_t result = send_router_filler (
          router, target_a, 65536);
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
    const std::vector<std::string> pending_record = {"async-gate-head", "async-gate-tail"};
    zlink_send_op_id_t pending_op_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      submit_record (router, pending_record, &options, &pending_op_id));
    TEST_ASSERT_TRUE (pending_op_id != 0);
    //  It must still be pending on the full A pipe. The sync sequence below
    //  is deliberately started after this reservation.
    TEST_ASSERT_EQUAL_UINT64 (0, completion_count (&completion));

    sync_sequence_probe_t sync_probe;
    std::thread sync_thread (
      run_sync_multipart_sequence, router, std::cref (target_b), &sync_probe);
    {
        std::unique_lock<std::mutex> lock (sync_probe.mutex);
        TEST_ASSERT_TRUE (sync_probe.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&sync_probe] { return sync_probe.first_done; }));
    }

    //  Returning A's credit wakes the async admit loop while the B sequence
    //  still owns the per-handle sync gate. The pending record must wait for
    //  that gate, not be converted into an EINVAL terminal.
    const int drained = drain_dealer_parts (dealer_a);
    TEST_ASSERT_TRUE (drained > 0);
    msleep (25);
    const bool completion_before_gate_release = completion_count (&completion) != 0;

    {
        std::lock_guard<std::mutex> lock (sync_probe.mutex);
        sync_probe.release = true;
    }
    sync_probe.changed.notify_all ();
    sync_thread.join ();

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, sync_probe.first);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, sync_probe.second);
    TEST_ASSERT_EQUAL_INT (0, sync_probe.first_errno);
    TEST_ASSERT_EQUAL_INT (0, sync_probe.second_errno);
    TEST_ASSERT_FALSE_MESSAGE (
      completion_before_gate_release,
      "pending multipart admitted while another multipart held the send gate");

    TEST_ASSERT_TRUE (wait_for_completion_count (&completion, 1));
    const completion_snapshot_t async_completion = completion_at (&completion, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, async_completion.result);
    TEST_ASSERT_EQUAL_INT (0, async_completion.terminal_errno);
    TEST_ASSERT_TRUE (recv_dealer_record_eventually (
      dealer_b, {"sync-head", "sync-tail"}));
    TEST_ASSERT_TRUE (recv_dealer_record_eventually (dealer_a, pending_record));
}

int main ()
{
    setup_test_environment (60);

    UNITY_BEGIN ();
    RUN_TEST (test_send_pending_limits_default_to_unlimited_and_accept_zero);
    RUN_TEST (test_send_pending_sequence_exhaustion_never_assigns_zero);
    RUN_TEST (test_completion_callback_rejects_all_reentrant_submit_entry_points);
    RUN_TEST (test_router_send_async_fused_target_admits_single_and_multipart_records);
    RUN_TEST (test_dealer_generic_target_admits_multipart_after_connect);
    RUN_TEST (test_routed_multipart_async_rolls_back_when_later_part_hits_hwm);
    RUN_TEST (test_pending_driver_rechecks_enqueue_racing_gate_release);
    RUN_TEST (test_borrowed_single_rejection_restores_caller_more_flag);
    RUN_TEST (test_pending_multipart_admission_waits_for_sync_sequence_gate);
    return UNITY_END ();
}
