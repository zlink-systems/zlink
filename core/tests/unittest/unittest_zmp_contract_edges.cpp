/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "api/message/submit_result_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/request_reply_frame_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "protocol/zmp_encoder.hpp"

#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace
{
struct timeout_barrier_t
{
    timeout_barrier_t () : fired (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool fired;
};

struct blocking_timeout_remove_barrier_t
{
    blocking_timeout_remove_barrier_t () : entered (false), release (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool entered;
    bool release;
};

struct timeout_completion_probe_t
{
    timeout_completion_probe_t () : count (0), result (ZLINK_REQUEST_INTERNAL_ERROR) {}

    std::mutex mutex;
    size_t count;
    zlink_request_result_t result;
};

void signal_timeout_barrier (void *userdata_)
{
    timeout_barrier_t *barrier = static_cast<timeout_barrier_t *> (userdata_);
    std::lock_guard<std::mutex> lock (barrier->mutex);
    barrier->fired = true;
    barrier->cv.notify_all ();
}

void block_after_timeout_pending_remove (void *userdata_)
{
    blocking_timeout_remove_barrier_t *barrier =
      static_cast<blocking_timeout_remove_barrier_t *> (userdata_);
    std::unique_lock<std::mutex> lock (barrier->mutex);
    barrier->entered = true;
    barrier->cv.notify_all ();
    barrier->cv.wait (lock, [barrier] { return barrier->release; });
}

void capture_timeout_completion (zlink_request_result_t result_,
                                 zlink_msg_t *parts_,
                                 size_t part_count_,
                                 void *userdata_)
{
    timeout_completion_probe_t *probe =
      static_cast<timeout_completion_probe_t *> (userdata_);
    zlink_multipart_close (parts_, part_count_);
    std::lock_guard<std::mutex> lock (probe->mutex);
    ++probe->count;
    probe->result = result_;
}
}

void setUp ()
{
}

void tearDown ()
{
}

void test_zmp_encoder_rejects_payload_larger_than_u32 ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    unsigned char borrowed = 0;
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_data (
      &borrowed,
      static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
      NULL, NULL));

    zlink::zmp_encoder_t encoder (64);
    errno = 0;
    encoder.load_msg (&msg);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);

    unsigned char *encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (0, encoder.encode (&encoded, 0));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

void test_zmp_socket_send_rejects_oversized_borrowed_payload ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    unsigned char borrowed = 0;
    zlink_msg_t msg;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_msg_init_data (
        &msg, &borrowed,
        static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
        NULL, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_send_single_msg (&msg, dealer, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_error_reply_with_zero_errno_becomes_protocol_error ()
{
    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], 4));
    memset (zlink_msg_data (&parts[0]), 0, 4);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], 7));
    memcpy (zlink_msg_data (&parts[1]), "payload", 7);

    int callback_errno = 0;
    zlink_msg_t *callback_parts = parts;
    size_t callback_part_count = 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::request_reply::decode_reply_completion (
        zlink::request_reply::error_reply_type, parts, 2, &callback_errno,
        &callback_parts, &callback_part_count));
    TEST_ASSERT_EQUAL_INT (EPROTO, callback_errno);
    TEST_ASSERT_NULL (callback_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, callback_part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[1]));
}

void test_missing_completion_pipe_is_not_connected ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);

    zlink_msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&frame, 5));
    memcpy (zlink_msg_data (&frame), "reply", 5);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_reqrep_internal::send_completion_frames (
            handle.socket, NULL, NULL, &frame, 1));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink::submit_result_internal::from_errno (errno));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));

    handle = socket_handle_t ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_request_reply_frame_buffer_spills_after_eight_owned_frames ()
{
    zlink::socket_reqrep_internal::request_reply_frame_buffer_t frames;
    zlink_msg_t *inline_data = NULL;

    for (size_t i = 0; i < 9; ++i) {
        frames.push_back (zlink_msg_t ());
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&frames.back (), 80 + i));
        memset (zlink_msg_data (&frames.back ()), static_cast<int> ('a' + i),
                zlink_msg_size (&frames.back ()));

        if (i == 0)
            inline_data = frames.data ();
        if (i < zlink::socket_reqrep_internal::inline_request_reply_frame_capacity)
            TEST_ASSERT_EQUAL_PTR (inline_data, frames.data ());
    }

    TEST_ASSERT_TRUE (frames.data () != inline_data);
    TEST_ASSERT_EQUAL_UINT64 (9, frames.size ());
    for (size_t i = 0; i < frames.size (); ++i) {
        TEST_ASSERT_EQUAL_UINT64 (80 + i, zlink_msg_size (&frames[i]));
        const unsigned char *data =
          static_cast<const unsigned char *> (zlink_msg_data (&frames[i]));
        TEST_ASSERT_EQUAL_HEX8 (static_cast<unsigned char> ('a' + i), data[0]);
        TEST_ASSERT_EQUAL_HEX8 (static_cast<unsigned char> ('a' + i),
                                data[zlink_msg_size (&frames[i]) - 1]);
    }

    zlink::socket_reqrep_internal::close_request_reply_frame_buffer (&frames);
    TEST_ASSERT_TRUE (frames.empty ());
}

void test_recv_sequence_buffers_two_parts_inline_and_rolls_back_oom ()
{
    using namespace zlink::part_helper_internal;
    std::shared_ptr<handle_state_t> state (new handle_state_t ());

    zlink_msg_t input[2];
    for (size_t i = 0; i < 2; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&input[i], 4));
        memset (zlink_msg_data (&input[i]), static_cast<int> ('a' + i), 4);
    }
    TEST_ASSERT_SUCCESS_ERRNO (stage_recv_sequence (
      state, recv_family_basic, NULL, NULL, 0, input, 2,
      std::this_thread::get_id ()));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->recv.active);
        TEST_ASSERT_EQUAL_UINT64 (2, state->recv.buffered_parts.size ());
        TEST_ASSERT_TRUE (state->recv.buffered_parts.capacity ()
                          >= inline_recv_part_capacity);
    }

    zlink_msg_t output;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
    TEST_ASSERT_SUCCESS_ERRNO (take_recv_part (state, &output, &more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&output));
    TEST_ASSERT_SUCCESS_ERRNO (take_recv_part (state, &output, &more));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&output));
    complete_recv_step (state, more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&input[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&input[1]));

    zlink_msg_t retained;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&retained, 3));
    memcpy (zlink_msg_data (&retained), "oom", 3);
    zlink_routing_id_t source_rid;
    memset (&source_rid, 0, sizeof (source_rid));
    source_rid.size = 3;
    memcpy (source_rid.data, "rid", source_rid.size);
    state->recv.transport_pair_id = 41;
    state->recv.transport_pair_generation = 42;
    const size_t impossible_part_count =
      state->recv.buffered_parts.max_size () + 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, stage_recv_sequence (
            state, recv_family_basic, NULL, &source_rid, 77, &retained,
            impossible_part_count, std::this_thread::get_id ()));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_FALSE (state->recv.active);
        TEST_ASSERT_EQUAL_INT (recv_family_none, state->recv.family);
        TEST_ASSERT_TRUE (state->recv.buffered_parts.empty ());
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.next_part_index);
        TEST_ASSERT_TRUE (state->recv.return_source_rid_as_null);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.source_node_rid.size);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.request_seq);
        TEST_ASSERT_EQUAL_UINT64 (0, state->recv.transport_pair_id);
        TEST_ASSERT_EQUAL_UINT64 (0,
                                  state->recv.transport_pair_generation);
    }
    TEST_ASSERT_EQUAL_UINT64 (3, zlink_msg_size (&retained));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&retained));
}

void test_zero_request_timeout_resolves_to_implementation_default ()
{
    TEST_ASSERT_EQUAL_UINT32 (
      zlink::request_reply::default_timeout_ms,
      zlink::request_reply::resolve_timeout_ms (0, 0));
    TEST_ASSERT_EQUAL_UINT32 (321,
                              zlink::request_reply::resolve_timeout_ms (0, 321));
    TEST_ASSERT_EQUAL_UINT32 (
      654, zlink::request_reply::resolve_timeout_ms (0, 321, 654));
    TEST_ASSERT_EQUAL_UINT32 (
      987, zlink::request_reply::resolve_timeout_ms (987, 321, 654));
}

void test_pending_aggregate_wrap_and_stale_cookie_are_fenced ()
{
    using namespace zlink::socket_reqrep_internal;
    std::shared_ptr<socket_request_reply_state_t> state (
      new socket_request_reply_state_t (NULL, ZLINK_CORE_SOCKET_ROUTER));

    pending_request_t wrap_blocker = pending_request_t ();
    wrap_blocker.identity.request_seq = std::numeric_limits<uint64_t>::max ();
    wrap_blocker.identity.cookie = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), wrap_blocker));
    state->next_request_seq = std::numeric_limits<uint64_t>::max ();
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::request_reply_runtime::allocate_request_sequence (state.get ()));
    TEST_ASSERT_EQUAL_UINT64 (2, state->next_request_seq);

    pending_request_t removed = pending_request_t ();
    TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
      state.get (), wrap_blocker.identity, &removed));

    pending_request_t old_request = pending_request_t ();
    old_request.identity.request_seq = 1;
    old_request.identity.cookie = 41;
    old_request.transport_pair_id = 0;
    old_request.transport_pair_generation = 0;
    pending_request_token_t stale_token;
    stale_token.identity = old_request.identity;
    stale_token.resolved_timeout_ms = 1000;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), old_request));
    TEST_ASSERT_TRUE (
      remove_socket_pending_request_locked (state.get (), stale_token.identity, &removed));

    pending_request_t reused_request = pending_request_t ();
    reused_request.identity.request_seq = stale_token.identity.request_seq;
    reused_request.identity.cookie = 42;
    reused_request.transport_pair_id = 0;
    reused_request.transport_pair_generation = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      add_socket_pending_request_locked (state.get (), reused_request));

    TEST_ASSERT_FALSE (
      remove_socket_pending_request_locked (state.get (), stale_token.identity, &removed));
    TEST_ASSERT_FALSE (record_socket_pending_transport_pair_identity (
      state, stale_token.identity, 101, 9));
    TEST_ASSERT_SUCCESS_ERRNO (arm_socket_pending_request_timeout (state, stale_token));
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        const std::unordered_map<uint64_t, pending_request_t>::const_iterator current =
          state->pending_requests.find (stale_token.identity.request_seq);
        TEST_ASSERT_TRUE (current != state->pending_requests.end ());
        TEST_ASSERT_EQUAL_UINT64 (reused_request.identity.cookie,
                                  current->second.identity.cookie);
        TEST_ASSERT_EQUAL_UINT64 (0, current->second.transport_pair_id);
        TEST_ASSERT_FALSE (current->second.timeout_task);
    }

    std::shared_ptr<zlink::request_timeout::task_t> stale_timeout;
    TEST_ASSERT_SUCCESS_ERRNO (schedule_socket_pending_timeout (
      state, stale_token.identity, 1, &stale_timeout));
    timeout_barrier_t barrier;
    std::shared_ptr<zlink::request_timeout::task_t> barrier_task =
      zlink::request_timeout::schedule (20, &signal_timeout_barrier, &barrier);
    TEST_ASSERT_NOT_NULL (barrier_task.get ());
    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        TEST_ASSERT_TRUE (barrier.cv.wait_for (
          lock, std::chrono::seconds (3), [&barrier] { return barrier.fired; }));
    }
    zlink::request_timeout::cancel (stale_timeout);
    zlink::request_timeout::cancel (barrier_task);

    TEST_ASSERT_EQUAL_UINT64 (1, state->pending_requests.size ());
    TEST_ASSERT_TRUE (remove_socket_pending_request_locked (
      state.get (), reused_request.identity, &removed));
    TEST_ASSERT_TRUE (state->pending_requests.empty ());
}

void test_suspended_request_multipart_preserves_pending_cookie ()
{
    using namespace zlink::part_helper_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);

    send_sequence_spec_t staged;
    staged.family = send_family_dealer_request;
    staged.request_like = true;
    std::shared_ptr<handle_state_t> helper_state;
    bool first_part = false;
    TEST_ASSERT_SUCCESS_ERRNO (prepare_send_step (
      dealer, staged, handle.socket, &helper_state, &first_part));
    TEST_ASSERT_TRUE (first_part);
    complete_send_step (helper_state, ZLINK_PART_MORE);

    send_sequence_spec_t resumed = staged;
    resumed.timeout_ms = 1000;
    resumed.request_seq = 77;
    resumed.pending_cookie = 991;
    TEST_ASSERT_SUCCESS_ERRNO (prepare_send_step (
      dealer, resumed, handle.socket, &helper_state, &first_part));
    TEST_ASSERT_FALSE (first_part);
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (resumed.request_seq,
                                  helper_state->send.spec.request_seq);
        TEST_ASSERT_EQUAL_UINT64 (resumed.pending_cookie,
                                  helper_state->send.spec.pending_cookie);
    }
    abort_send_step (helper_state);

    handle = socket_handle_t ();
    helper_state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pending_cookie_wrap_skips_zero ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->next_pending_cookie = std::numeric_limits<uint64_t>::max ();
    }

    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pending_request (
      handle, 1000, NULL, NULL, &request_seq, &registered_state, &token));
    TEST_ASSERT_EQUAL_UINT64 (request_seq, token.identity.request_seq);
    TEST_ASSERT_EQUAL_UINT64 (std::numeric_limits<uint64_t>::max (),
                              token.identity.cookie);
    TEST_ASSERT_TRUE (erase_socket_pending_request (state, token.identity));

    request_seq = 0;
    registered_state.reset ();
    token = pending_request_token_t ();
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pending_request (
      handle, 1000, NULL, NULL, &request_seq, &registered_state, &token));
    TEST_ASSERT_EQUAL_UINT64 (request_seq, token.identity.request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, token.identity.cookie);
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (2, state->next_pending_cookie);
    }
    TEST_ASSERT_TRUE (erase_socket_pending_request (state, token.identity));

    handle = socket_handle_t ();
    registered_state.reset ();
    state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_pending_insert_failure_releases_completion_reservation ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());

    test_set_request_reply_allocation_failpoint (
      request_reply_allocation_pending_insert);
    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, ensure_socket_pending_request (
            handle, 1000, NULL, NULL, &request_seq, &registered_state,
            &token));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_NULL (registered_state.get ());
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        TEST_ASSERT_TRUE (state->pending_requests.empty ());
    }
    {
        std::lock_guard<std::mutex> lock (state->completion.mutex);
        TEST_ASSERT_EQUAL_UINT64 (0, state->completion.reserved);
        TEST_ASSERT_NULL (state->completion.reserved_head);
    }
    TEST_ASSERT_TRUE (
      zlink::request_completion::try_reserve (&state->completion));
    zlink::request_completion::release_reservation (&state->completion);

    handle = socket_handle_t ();
    state.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_timeout_remove_holds_socket_lifecycle_barrier_against_close ()
{
    using namespace zlink::socket_reqrep_internal;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    socket_handle_t handle = as_socket_handle (dealer);
    std::shared_ptr<socket_request_reply_state_t> state =
      find_or_create_request_reply_state (handle);
    TEST_ASSERT_NOT_NULL (state.get ());

    timeout_completion_probe_t completion;
    uint64_t request_seq = 0;
    std::shared_ptr<socket_request_reply_state_t> registered_state;
    pending_request_token_t token;
    TEST_ASSERT_SUCCESS_ERRNO (ensure_socket_pending_request (
      handle, 20, &capture_timeout_completion, &completion, &request_seq,
      &registered_state, &token));

    blocking_timeout_remove_barrier_t barrier;
    test_set_request_reply_timeout_after_remove_hook (
      &block_after_timeout_pending_remove, &barrier);
    TEST_ASSERT_SUCCESS_ERRNO (
      arm_socket_pending_request_timeout (state, token));
    handle = socket_handle_t ();

    {
        std::unique_lock<std::mutex> lock (barrier.mutex);
        TEST_ASSERT_TRUE (barrier.cv.wait_for (
          lock, std::chrono::seconds (3), [&barrier] { return barrier.entered; }));
    }

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, zlink_close (dealer));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);

    {
        std::lock_guard<std::mutex> lock (barrier.mutex);
        barrier.release = true;
    }
    barrier.cv.notify_all ();

    zlink_close_result_t close_rc = ZLINK_CLOSE_BUSY;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    do {
        close_rc = zlink_close (dealer);
        if (close_rc == ZLINK_CLOSE_BUSY)
            msleep (1);
    } while (close_rc == ZLINK_CLOSE_BUSY
             && std::chrono::steady_clock::now () < deadline);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_rc);
    test_set_request_reply_timeout_after_remove_hook (NULL, NULL);

    {
        std::lock_guard<std::mutex> lock (completion.mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, completion.count);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, completion.result);
    }

    registered_state.reset ();
    state.reset ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_zmp_encoder_rejects_payload_larger_than_u32);
    RUN_TEST (test_zmp_socket_send_rejects_oversized_borrowed_payload);
    RUN_TEST (test_error_reply_with_zero_errno_becomes_protocol_error);
    RUN_TEST (test_missing_completion_pipe_is_not_connected);
    RUN_TEST (test_request_reply_frame_buffer_spills_after_eight_owned_frames);
    RUN_TEST (test_recv_sequence_buffers_two_parts_inline_and_rolls_back_oom);
    RUN_TEST (test_zero_request_timeout_resolves_to_implementation_default);
    RUN_TEST (test_pending_aggregate_wrap_and_stale_cookie_are_fenced);
    RUN_TEST (test_suspended_request_multipart_preserves_pending_cookie);
    RUN_TEST (test_pending_cookie_wrap_skips_zero);
    RUN_TEST (test_pending_insert_failure_releases_completion_reservation);
    RUN_TEST (test_timeout_remove_holds_socket_lifecycle_barrier_against_close);
    return UNITY_END ();
}
