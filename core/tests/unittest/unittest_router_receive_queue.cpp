/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include "core/ctx.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "core/recv_internal.hpp"
#include "protocol/zmp_protocol.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/internal/fq.hpp"
#include "sockets/router/router.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT
namespace zlink
{
class session_termination_test_access_t
{
  public:
    static void attach_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->attach_pipe (pipe_);
    }

    static bool receive_mutex_is_held_by_another_thread (socket_base_t *socket_)
    {
        mutex_t &sync = socket_->receive_runtime ().sync;
        if (!sync.try_lock ())
            return true;
        sync.unlock ();
        return false;
    }

    static int recv_router_with_receive_lock (
      router_t *router_, msg_t *msg_, bool routed_recv_,
      zlink_routing_id_t *source_rid_out_)
    {
        // Async receive dispatch and its public receive fallback enter the
        // concrete ROUTER reader through this base-owned lock domain.
        scoped_lock_t receive_lock (router_->receive_runtime ().sync);
        return routed_recv_
                 ? router_->xrecv_routed (msg_, source_rid_out_, NULL)
                 : router_->xrecv (msg_);
    }

    static void terminate_router_pipe_with_receive_lock (socket_base_t *socket_,
                                                         pipe_t *pipe_)
    {
        // Mirror socket_base_t::pipe_terminated's receive-side phase without
        // pretending that this synthetic pipe has completed its full physical
        // termination lifecycle.
        scoped_lock_t receive_lock (socket_->receive_runtime ().sync);
        socket_->xpipe_terminated (pipe_);
    }

    static void reset_pipe_inbound_queue (pipe_t *pipe_)
    {
        pipe_->hiccup ();
    }
};
}

namespace
{
struct fq_recv_termination_gate_t
{
    fq_recv_termination_gate_t () :
        target_pipe (NULL),
        observed_fq (NULL),
        recv_paused (false),
        release_recv (false),
        cancel_recv (false),
        termination_started (false),
        termination_done (false),
        receive_mutex_held (false)
    {
    }

    std::mutex sync;
    std::condition_variable cv;
    zlink::pipe_t *target_pipe;
    zlink::fq_t *observed_fq;
    bool recv_paused;
    bool release_recv;
    bool cancel_recv;
    bool termination_started;
    bool termination_done;
    bool receive_mutex_held;
};

struct direct_recv_result_t
{
    direct_recv_result_t () : rc (-1), errno_value (0), flags (0), source_rid (), payload () {}

    int rc;
    int errno_value;
    unsigned char flags;
    zlink_routing_id_t source_rid;
    std::string payload;
};

struct prefetched_reject_consume_probe_t
{
    prefetched_reject_consume_probe_t () :
        expected_pipe (NULL), callback_count (0), expected_pipe_seen (false),
        more_seen (false), metadata_seen (false)
    {
    }

    zlink::pipe_t *expected_pipe;
    int callback_count;
    bool expected_pipe_seen;
    bool more_seen;
    bool metadata_seen;
};

class passive_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *, bool) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
};

bool pause_selected_fq_recv (zlink::fq_t *fq_, zlink::pipe_t *pipe_, void *userdata_)
{
    fq_recv_termination_gate_t *gate =
      static_cast<fq_recv_termination_gate_t *> (userdata_);
    if (!gate || pipe_ != gate->target_pipe)
        return true;

    std::unique_lock<std::mutex> lock (gate->sync);
    gate->observed_fq = fq_;
    gate->recv_paused = true;
    gate->cv.notify_all ();
    gate->cv.wait (lock, [gate] { return gate->release_recv; });
    return !gate->cancel_recv;
}

void write_internal_pipe_message (zlink::pipe_t *pipe_,
                                  const char *payload_,
                                  bool routing_id_)
{
    zlink::msg_t msg;
    const size_t size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (size));
    memcpy (msg.data (), payload_, size);
    if (routing_id_)
        msg.set_flags (zlink::msg_t::routing_id);
    TEST_ASSERT_TRUE (pipe_->write_and_flush (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

void write_internal_pipe_part (zlink::pipe_t *pipe_,
                               const char *payload_,
                               bool more_)
{
    zlink::msg_t msg;
    const size_t size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (size));
    memcpy (msg.data (), payload_, size);
    if (more_)
        msg.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipe_->write (&msg));
    pipe_->flush ();
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

void write_internal_admitted_pipe_part (zlink::pipe_t *pipe_,
                                        const char *payload_,
                                        bool more_,
                                        uint64_t request_sequence_)
{
    zlink::msg_t msg;
    const size_t size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (size));
    memcpy (msg.data (), payload_, size);
    if (more_)
        msg.set_flags (zlink::msg_t::more);
    if (request_sequence_ != 0)
        TEST_ASSERT_SUCCESS_ERRNO (msg.set_request_reply_metadata (
          zlink::zmp_kind_request, request_sequence_));
    TEST_ASSERT_TRUE (pipe_->write (&msg));
    pipe_->flush ();
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

int reject_prefetched_record_and_consume (zlink::pipe_t *pipe_,
                                          const zlink::msg_t &msg_,
                                          void *userdata_)
{
    prefetched_reject_consume_probe_t *const probe =
      static_cast<prefetched_reject_consume_probe_t *> (userdata_);
    if (!probe) {
        errno = EFAULT;
        return zlink::pipe_t::read_admission_reject_consume;
    }

    ++probe->callback_count;
    probe->expected_pipe_seen = pipe_ == probe->expected_pipe;
    probe->more_seen = (msg_.flags () & zlink::msg_t::more) != 0;
    probe->metadata_seen = msg_.get_request_reply_metadata (NULL, NULL);
    errno = ENOMEM;
    return zlink::pipe_t::read_admission_reject_consume;
}

}
void run_router_recv_serializes_fq_with_pipe_termination (bool routed_recv_)
{
    void *router_handle =
      zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router_handle);

    socket_handle_t router_pin = as_socket_handle (router_handle);
    TEST_ASSERT_NOT_NULL (router_pin.socket);
    zlink::router_t *router = static_cast<zlink::router_t *> (router_pin.socket);
    zlink::object_t *parents[2] = {router, router};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    // This fixture exercises the recv/termination lock domain, not HWM.
    // Physical queues account payload bytes plus one msg_t per frame, so keep
    // both the routing-id and payload frames admissible under the byte HWM.
    const uint64_t queued_frame_bytes =
      2 * sizeof (zlink::msg_t) + (sizeof ("peer-A") - 1)
      + (sizeof ("payload") - 1);
    const uint64_t hwms[2] = {queued_frame_bytes, queued_frame_bytes};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));

    passive_pipe_sink_t peer_sink;
    pipes[1]->set_event_sink (&peer_sink);

    // Queue admission and payload before attaching. Router admission consumes
    // the routing-id frame and leaves the payload active in the fair queue.
    write_internal_pipe_message (pipes[1], "peer-A", true);
    write_internal_pipe_message (pipes[1], "payload", false);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      router, pipes[0]);

    fq_recv_termination_gate_t gate;
    gate.target_pipe = pipes[0];
    zlink::fq_t::set_recv_test_hook (&pause_selected_fq_recv, &gate);

    direct_recv_result_t recv_result;
    std::thread recv_thread ([router, routed_recv_, &recv_result] {
        zlink::msg_t msg;
        if (msg.init () != 0) {
            recv_result.errno_value = errno;
            return;
        }

        recv_result.rc = zlink::session_termination_test_access_t::
          recv_router_with_receive_lock (
            router, &msg, routed_recv_, &recv_result.source_rid);
        recv_result.errno_value = recv_result.rc == 0 ? 0 : errno;
        if (recv_result.rc == 0) {
            recv_result.flags = msg.flags ();
            recv_result.payload.assign (
              static_cast<const char *> (msg.data ()), msg.size ());
        }
        const int close_rc = msg.close ();
        if (close_rc != 0 && recv_result.errno_value == 0)
            recv_result.errno_value = errno;
    });

    {
        std::unique_lock<std::mutex> lock (gate.sync);
        gate.cv.wait (lock, [&gate] { return gate.recv_paused; });
    }

    std::thread termination_thread ([router, &gate, pipe = pipes[0]] {
        const bool receive_mutex_held =
          zlink::session_termination_test_access_t::
            receive_mutex_is_held_by_another_thread (router);
        {
            std::lock_guard<std::mutex> lock (gate.sync);
            gate.receive_mutex_held = receive_mutex_held;
            gate.termination_started = true;
        }
        gate.cv.notify_all ();

        zlink::session_termination_test_access_t::
          terminate_router_pipe_with_receive_lock (router, pipe);

        {
            std::lock_guard<std::mutex> lock (gate.sync);
            gate.termination_done = true;
        }
        gate.cv.notify_all ();
    });

    bool receive_mutex_held = false;
    {
        std::unique_lock<std::mutex> lock (gate.sync);
        gate.cv.wait (lock, [&gate] { return gate.termination_started; });
        receive_mutex_held = gate.receive_mutex_held;

        // On the old unlocked recv path, termination can complete while recv
        // is paused inside fq_t. Cancel that recv after observing the forbidden
        // overlap so the RED result is an assertion rather than use-after-free.
        if (!receive_mutex_held) {
            gate.cv.wait (lock, [&gate] { return gate.termination_done; });
            gate.cancel_recv = true;
        }
        gate.release_recv = true;
    }
    gate.cv.notify_all ();

    recv_thread.join ();
    termination_thread.join ();
    zlink::fq_t::set_recv_test_hook (NULL, NULL);

    if (receive_mutex_held) {
        TEST_ASSERT_EQUAL_INT (0, recv_result.rc);
        TEST_ASSERT_EQUAL_INT (0, recv_result.errno_value);
        if (routed_recv_) {
            TEST_ASSERT_EQUAL_STRING ("payload", recv_result.payload.c_str ());
            TEST_ASSERT_EQUAL_UINT (6, recv_result.source_rid.size);
            TEST_ASSERT_EQUAL_MEMORY ("peer-A", recv_result.source_rid.data,
                                      recv_result.source_rid.size);
        } else {
            TEST_ASSERT_EQUAL_STRING ("peer-A", recv_result.payload.c_str ());
            TEST_ASSERT_TRUE ((recv_result.flags & zlink::msg_t::more) != 0);

            // xrecv exposes the routing-id envelope first. Consume the
            // prefetched payload so no receive state retains the test pipe.
            zlink::msg_t payload;
            TEST_ASSERT_SUCCESS_ERRNO (payload.init ());
            TEST_ASSERT_SUCCESS_ERRNO (router->xrecv (&payload));
            TEST_ASSERT_EQUAL_UINT (7, payload.size ());
            TEST_ASSERT_EQUAL_MEMORY ("payload", payload.data (),
                                      payload.size ());
            TEST_ASSERT_SUCCESS_ERRNO (payload.close ());
        }
    }
    TEST_ASSERT_NOT_NULL (gate.observed_fq);
    TEST_ASSERT_EQUAL_UINT (0, gate.observed_fq->test_pipe_count ());

    router_pin = socket_handle_t ();
    close_zero_linger (router_handle);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (ctx->wait_for_socket_count_at_most (0, 5000));
    TEST_ASSERT_TRUE_MESSAGE (
      receive_mutex_held,
      "ROUTER recv did not hold the FQ termination lock domain");
}

void test_router_recv_serializes_fq_with_pipe_termination ()
{
    run_router_recv_serializes_fq_with_pipe_termination (false);
}

void test_router_routed_recv_serializes_fq_with_pipe_termination ()
{
    run_router_recv_serializes_fq_with_pipe_termination (true);
}

void run_router_multipart_pipe_termination_does_not_join_next_peer_record (
  bool first_part_exposed_, bool blocking_followup_ = false,
  bool read_false_abort_ = false)
{
    void *router_handle =
      zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router_handle);

    socket_handle_t router_pin = as_socket_handle (router_handle);
    TEST_ASSERT_NOT_NULL (router_pin.socket);
    zlink::router_t *router = static_cast<zlink::router_t *> (router_pin.socket);
    zlink::object_t *parents[2] = {router, router};
    zlink::pipe_t *pipe_a[2] = {NULL, NULL};
    zlink::pipe_t *pipe_b[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_a, hwms, conflates, true));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_b, hwms, conflates, true));

    passive_pipe_sink_t peer_a_sink;
    passive_pipe_sink_t peer_b_sink;
    pipe_a[1]->set_event_sink (&peer_a_sink);
    pipe_b[1]->set_event_sink (&peer_b_sink);

    write_internal_pipe_message (pipe_a[1], "peer-A", true);
    write_internal_pipe_part (pipe_a[1], "payload-A", true);
    write_internal_pipe_part (pipe_a[1], "", false);
    write_internal_pipe_message (pipe_b[1], "peer-B", true);
    write_internal_pipe_part (pipe_b[1], "payload-B", true);
    write_internal_pipe_part (pipe_b[1], "", false);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      router, pipe_a[0]);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      router, pipe_b[0]);

    if (first_part_exposed_) {
        zlink::msg_t first;
        TEST_ASSERT_SUCCESS_ERRNO (first.init ());
        zlink_routing_id_t source_rid;
        memset (&source_rid, 0, sizeof (source_rid));
        TEST_ASSERT_SUCCESS_ERRNO (
          router->recv_routed (&first, &source_rid, ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT (9, first.size ());
        TEST_ASSERT_EQUAL_MEMORY ("payload-A", first.data (), first.size ());
        TEST_ASSERT_TRUE ((first.flags () & zlink::msg_t::more) != 0);
        TEST_ASSERT_EQUAL_UINT8 (6, source_rid.size);
        TEST_ASSERT_EQUAL_MEMORY ("peer-A", source_rid.data, source_rid.size);
        TEST_ASSERT_SUCCESS_ERRNO (first.close ());
    } else {
        // Match a poller readiness check that prefetched A just before its
        // terminal command was processed.
        TEST_ASSERT_TRUE (router->xhas_in ());
    }

    // Model the transport terminal command arriving between application
    // frames. The follow-up receive must abort this logical record; it must
    // never consume peer-B as peer-A's continuation.
    if (read_false_abort_)
        zlink::session_termination_test_access_t::reset_pipe_inbound_queue (
          pipe_a[0]);
    else
        router->xpipe_terminated (pipe_a[0]);
    if (first_part_exposed_) {
        zlink_msg_t followup;
        zlink_msg_init (&followup);
        int followup_rc = -1;
        if (read_false_abort_) {
            zlink_routing_id_t aborted_source_rid;
            memset (&aborted_source_rid, 0, sizeof (aborted_source_rid));
            followup_rc = router->xrecv_routed (
              reinterpret_cast<zlink::msg_t *> (&followup),
              &aborted_source_rid, NULL);
            TEST_ASSERT_EQUAL_INT (ECONNABORTED, errno);
        } else if (blocking_followup_) {
            followup_rc = zlink::recv_followup_msg_socket_wait (
              router, &followup, 0);
        } else {
            followup_rc = router->recv (
              reinterpret_cast<zlink::msg_t *> (&followup), ZLINK_DONTWAIT);
        }
        TEST_ASSERT_EQUAL_INT (-1, followup_rc);
        TEST_ASSERT_EQUAL_INT (read_false_abort_ ? ECONNABORTED : EAGAIN,
                               errno);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&followup));
    } else {
        const zlink_routing_id_t *aborted_source_rid = NULL;
        uint64_t aborted_request_seq = 0;
        zlink_msg_t *aborted_parts = NULL;
        size_t aborted_part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          -1, zlink::socket_reqrep_internal::recv_router_message_direct (
                router_pin, &aborted_source_rid, &aborted_request_seq,
                &aborted_parts, &aborted_part_count, ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    }

    const zlink_routing_id_t *next_source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_reqrep_internal::recv_router_message_direct (
        router_pin, &next_source_rid, &request_seq, &parts, &part_count,
        ZLINK_DONTWAIT));
    TEST_ASSERT_NOT_NULL (next_source_rid);
    TEST_ASSERT_EQUAL_UINT8 (6, next_source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY ("peer-B", next_source_rid->data,
                              next_source_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (9, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("payload-B", zlink_msg_data (&parts[0]),
                              zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&parts[1]));
    zlink_multipart_close (parts, part_count);

    router_pin = socket_handle_t ();
    close_zero_linger (router_handle);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

void test_router_exposed_multipart_pipe_termination_does_not_join_next_peer_record ()
{
    run_router_multipart_pipe_termination_does_not_join_next_peer_record (true);
}

void test_router_prefetched_multipart_pipe_termination_does_not_join_next_peer_record ()
{
    run_router_multipart_pipe_termination_does_not_join_next_peer_record (false);
}

void test_router_blocking_followup_does_not_retry_across_aborted_record ()
{
    run_router_multipart_pipe_termination_does_not_join_next_peer_record (
      true, true);
}

void test_router_empty_pinned_pipe_aborts_multipart_before_next_peer_record ()
{
    run_router_multipart_pipe_termination_does_not_join_next_peer_record (
      true, false, true);
}

void run_router_prefetched_reject_consume_discards_record (bool multipart_)
{
    void *router_handle =
      zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router_handle);

    socket_handle_t router_pin = as_socket_handle (router_handle);
    TEST_ASSERT_NOT_NULL (router_pin.socket);
    zlink::router_t *const router =
      static_cast<zlink::router_t *> (router_pin.socket);
    zlink::object_t *parents[2] = {router, router};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflates, true));

    passive_pipe_sink_t peer_sink;
    pipes[1]->set_event_sink (&peer_sink);

    const char *const peer_id =
      multipart_ ? "prefetched-multipart" : "prefetched-single";
    write_internal_pipe_message (pipes[1], peer_id, true);
    if (multipart_) {
        // MORE alone requires whole-record admission. Keep this record raw so
        // the test also proves that consuming its prefetched head drains the
        // raw tail rather than exposing it as a new record.
        write_internal_admitted_pipe_part (
          pipes[1], "rejected-multipart-head", true, 0);
        write_internal_admitted_pipe_part (
          pipes[1], "rejected-multipart-tail", false, 0);
    } else {
        // A terminal frame requires admission only when it carries request /
        // reply metadata.
        write_internal_admitted_pipe_part (
          pipes[1], "rejected-single", false, 101);
    }
    write_internal_admitted_pipe_part (
      pipes[1], "record-after-reject-consume", false, 0);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      router, pipes[0]);

    // Match the readiness/poller path: xhas_in removes the first application
    // frame from the pipe and stores it in ROUTER::_prefetched_msg.
    TEST_ASSERT_TRUE (router->xhas_in ());

    prefetched_reject_consume_probe_t probe;
    probe.expected_pipe = pipes[0];
    zlink::msg_t rejected;
    TEST_ASSERT_SUCCESS_ERRNO (rejected.init ());
    zlink_routing_id_t rejected_source;
    memset (&rejected_source, 0, sizeof (rejected_source));
    zlink::pipe_t *rejected_source_pipe = NULL;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, router->xrecv_routed (
            &rejected, &rejected_source, NULL, &rejected_source_pipe,
            &reject_prefetched_record_and_consume, &probe));
    TEST_ASSERT_EQUAL_INT (ENOMEM, errno);
    TEST_ASSERT_EQUAL_INT (1, probe.callback_count);
    TEST_ASSERT_TRUE (probe.expected_pipe_seen);
    TEST_ASSERT_EQUAL (multipart_, probe.more_seen);
    TEST_ASSERT_EQUAL (!multipart_, probe.metadata_seen);
    TEST_ASSERT_NULL (rejected_source_pipe);
    TEST_ASSERT_EQUAL_UINT64 (0, rejected.size ());
    TEST_ASSERT_SUCCESS_ERRNO (rejected.close ());

    // The rejected single frame, or every frame of the rejected multipart
    // record, must be gone. A second readiness prefetch must therefore land on
    // the following independent record.
    TEST_ASSERT_TRUE (router->xhas_in ());
    zlink::msg_t next;
    TEST_ASSERT_SUCCESS_ERRNO (next.init ());
    zlink_routing_id_t next_source;
    memset (&next_source, 0, sizeof (next_source));
    zlink::pipe_t *next_source_pipe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (router->xrecv_routed (
      &next, &next_source, NULL, &next_source_pipe));
    TEST_ASSERT_EQUAL_PTR (pipes[0], next_source_pipe);
    TEST_ASSERT_EQUAL_UINT (std::strlen (peer_id), next_source.size);
    TEST_ASSERT_EQUAL_MEMORY (peer_id, next_source.data, next_source.size);
    TEST_ASSERT_EQUAL_UINT (
      std::strlen ("record-after-reject-consume"), next.size ());
    TEST_ASSERT_EQUAL_MEMORY (
      "record-after-reject-consume", next.data (), next.size ());
    TEST_ASSERT_FALSE ((next.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_INT (1, probe.callback_count);
    TEST_ASSERT_SUCCESS_ERRNO (next.close ());

    router_pin = socket_handle_t ();
    close_zero_linger (router_handle);
    zlink::ctx_t *const ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

void test_router_prefetched_reject_consume_discards_single_and_multipart_records ()
{
    run_router_prefetched_reject_consume_discards_record (false);
    run_router_prefetched_reject_consume_discards_record (true);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_router_recv_serializes_fq_with_pipe_termination);
    RUN_TEST (test_router_routed_recv_serializes_fq_with_pipe_termination);
    RUN_TEST (test_router_exposed_multipart_pipe_termination_does_not_join_next_peer_record);
    RUN_TEST (test_router_prefetched_multipart_pipe_termination_does_not_join_next_peer_record);
    RUN_TEST (test_router_blocking_followup_does_not_retry_across_aborted_record);
    RUN_TEST (test_router_empty_pinned_pipe_aborts_multipart_before_next_peer_record);
    RUN_TEST (test_router_prefetched_reject_consume_discards_single_and_multipart_records);
    return UNITY_END ();
}
