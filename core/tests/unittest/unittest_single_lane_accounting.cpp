/* SPDX-License-Identifier: MPL-2.0 */

#include "contract_socket_pair_fixture.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int contract_wait_ms = 5000;
struct reply_prefix_accounting_gate_t
{
    reply_prefix_accounting_gate_t () : entered (false), released (false) {}

    std::mutex sync;
    std::condition_variable changed;
    bool entered;
    bool released;
};

void hold_reply_after_first_physical_prefix (void *userdata_)
{
    reply_prefix_accounting_gate_t *const gate =
      static_cast<reply_prefix_accounting_gate_t *> (userdata_);
    std::unique_lock<std::mutex> lock (gate->sync);
    gate->entered = true;
    gate->changed.notify_all ();
    gate->changed.wait (lock, [gate] { return gate->released; });
}

bool wait_for_reply_prefix_gate (reply_prefix_accounting_gate_t *gate_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    return gate_->changed.wait_for (
      lock, std::chrono::milliseconds (contract_wait_ms),
      [gate_] { return gate_->entered; });
}

void release_reply_prefix_gate (reply_prefix_accounting_gate_t *gate_)
{
    std::lock_guard<std::mutex> lock (gate_->sync);
    gate_->released = true;
    gate_->changed.notify_all ();
}

void init_part (zlink_msg_t *part_, const char *payload_)
{
    const size_t size = strlen (payload_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size));
    if (size != 0)
        memcpy (zlink_msg_data (part_), payload_, size);
}
void init_sized_part (zlink_msg_t *part_, size_t size_, unsigned char fill_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size_));
    if (size_ != 0)
        memset (zlink_msg_data (part_), fill_, size_);
}
void assert_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}
std::string part_text (zlink_msg_t *part_)
{
    return std::string (static_cast<const char *> (zlink_msg_data (part_)),
                        zlink_msg_size (part_));
}
zlink_completion_id_t send_request (void *socket_, const zlink_routing_id_t *rid_,
                                    const char *payload_, uint32_t timeout_ms_)
{
    zlink_msg_t request;
    init_part (&request, payload_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (socket_, rid_, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, timeout_ms_, NULL,
                          &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_consumed (&request);
    return completion_id;
}
zlink_auto_hwm_budget_snapshot_t read_budget_snapshot ()
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
int read_budget_snapshot_unchecked (
  zlink_auto_hwm_budget_snapshot_t *snapshot_)
{
    memset (snapshot_, 0, sizeof (*snapshot_));
    snapshot_->abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot_->struct_size = sizeof (*snapshot_);
    return zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (),
                                                   snapshot_);
}
struct received_router_part_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    zlink_part_flag_t part_flag;
    std::string payload;
};

received_router_part_t receive_router_part (void *socket_)
{
    contract_socket_pair_t::pump_owner (as_socket_handle (socket_).socket);
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *rid = NULL;
    received_router_part_t result;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_router_recv_part (socket_, &rid, &result.reply_token, &part,
                              &result.part_flag, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (rid);
    result.source_rid = *rid;
    result.payload = part_text (&part);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    return result;
}

zlink_completion_t receive_completion (void *socket_)
{
    zlink::socket_base_t *core = as_socket_handle (socket_).socket;
    contract_socket_pair_t::pump_owner (core);
    zlink::completion_drain_scope_t owner (core);
    core->process_ready_completion_pipes ();
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_completion_recv (socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    return completion;
}

void test_sl_flow_snapshot_accounts_dr_reply_as_application ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    contract_socket_pair_t dr (dealer, router);
    TEST_ASSERT_TRUE (dr.cores[0]->acquire_completion_poller (&dr));
    const zlink_completion_id_t request_id =
      send_request (dealer, NULL, "snapshot-request", 3000);
    const received_router_part_t request = receive_router_part (router);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    dr.pump ();
    TEST_ASSERT_TRUE (dr.cores[1]->application_pipe_remote_flow_paused (1, 1));
    const zlink_auto_hwm_budget_snapshot_t baseline = read_budget_snapshot ();

    zlink_msg_t prefix;
    init_sized_part (&prefix, 1024, 'p');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, &request.source_rid,
                        request.reply_token, &prefix, ZLINK_PART_MORE));
    assert_consumed (&prefix);
    // MORE is socket-local staging. Physical provisional accounting begins
    // only after FINAL selects the current route and moves the prefix.
    const zlink_auto_hwm_budget_snapshot_t staged =
      read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.core_queue_accounted_bytes, staged.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.current_accounted_bytes, staged.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.provisional_accounted_bytes, staged.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (baseline.total_messaging_accounted_bytes,
                              staged.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      staged.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_peak_accounted_bytes,
      staged.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      staged.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (baseline.active_directional_queue_count,
                              staged.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.active_completion_directional_queue_count,
      staged.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, staged.application_accounted_bytes);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    dr.pump ();
    TEST_ASSERT_FALSE (dr.cores[1]->application_pipe_remote_flow_paused (1, 1));
    zlink_msg_t final;
    init_sized_part (&final, 1024, 'f');
    reply_prefix_accounting_gate_t prefix_gate;
    bool prefix_entered = false;
    int provisional_snapshot_rc = -1;
    zlink_auto_hwm_budget_snapshot_t provisional;
    memset (&provisional, 0, sizeof (provisional));
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      &hold_reply_after_first_physical_prefix, &prefix_gate);
    std::thread prefix_observer ([&] {
        prefix_entered = wait_for_reply_prefix_gate (&prefix_gate);
        if (prefix_entered)
            provisional_snapshot_rc =
              read_budget_snapshot_unchecked (&provisional);
        release_reply_prefix_gate (&prefix_gate);
    });
    const zlink_submit_result_t final_result = zlink_reply_part (
      router, &request.source_rid, request.reply_token, &final,
      ZLINK_PART_FINAL);
    prefix_observer.join ();
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      NULL, NULL);
    TEST_ASSERT_TRUE (prefix_entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, provisional_snapshot_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, final_result);
    assert_consumed (&final);
    TEST_ASSERT_TRUE (provisional.core_queue_accounted_bytes
                      > baseline.core_queue_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.current_accounted_bytes
                      > baseline.current_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.provisional_accounted_bytes
                      > baseline.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.peak_accounted_bytes
                      >= provisional.current_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.total_messaging_accounted_bytes
                      > baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      provisional.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      provisional.completion_pending_message_count);
    TEST_ASSERT_TRUE (([&] {
        const zlink_auto_hwm_budget_snapshot_t snapshot =
          read_budget_snapshot ();
        return snapshot.current_accounted_bytes
                 > baseline.current_accounted_bytes
               && snapshot.core_queue_accounted_bytes
                    > baseline.core_queue_accounted_bytes;
    }) ());
    const zlink_auto_hwm_budget_snapshot_t queued = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (
      queued.core_queue_accounted_bytes - baseline.core_queue_accounted_bytes,
      queued.current_accounted_bytes - baseline.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (baseline.provisional_accounted_bytes,
                              queued.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (queued.peak_accounted_bytes
                      >= queued.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      queued.current_accounted_bytes - baseline.current_accounted_bytes,
      queued.total_messaging_accounted_bytes
        - baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      queued.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_peak_accounted_bytes,
      queued.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      queued.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (baseline.active_directional_queue_count,
                              queued.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.active_completion_directional_queue_count,
      queued.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, queued.application_accounted_bytes);

    zlink_completion_t completion = receive_completion (dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&completion.reply_parts[1]));
    zlink_completion_close (&completion);
    dr.cores[0]->release_completion_poller (&dr);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);

    // The same public reply transaction on R/R is accounted exclusively by
    // the physical Completion class: no Application/current/provisional field
    // moves, while Completion current/peak/pending and total messaging do.
    void *first = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *second = test_context_socket (ZLINK_SOCKET_ROUTER);
    contract_socket_pair_t rr (first, second);
    TEST_ASSERT_TRUE (rr.cores[0]->acquire_completion_poller (&rr));
    const zlink_completion_id_t rr_request_id = send_request (
      first, &rr.rids[1], "snapshot-rr-request", 3000);
    const received_router_part_t rr_request = receive_router_part (second);
    const zlink_auto_hwm_budget_snapshot_t rr_baseline = read_budget_snapshot ();
    TEST_ASSERT_TRUE (
      rr_baseline.active_completion_directional_queue_count > 0);

    zlink_msg_t rr_prefix;
    init_sized_part (&rr_prefix, 1024, 'c');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (second, &rr_request.source_rid,
                        rr_request.reply_token, &rr_prefix, ZLINK_PART_MORE));
    assert_consumed (&rr_prefix);
    const zlink_auto_hwm_budget_snapshot_t rr_staged =
      read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_staged.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_staged.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_staged.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_current_accounted_bytes,
      rr_staged.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_peak_accounted_bytes,
      rr_staged.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_pending_message_count,
      rr_staged.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.total_messaging_accounted_bytes,
      rr_staged.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.active_directional_queue_count,
                              rr_staged.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.active_completion_directional_queue_count,
      rr_staged.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, rr_staged.application_accounted_bytes);

    zlink_msg_t rr_final;
    init_sized_part (&rr_final, 1024, 'd');
    reply_prefix_accounting_gate_t rr_prefix_gate;
    bool rr_prefix_entered = false;
    int rr_provisional_snapshot_rc = -1;
    zlink_auto_hwm_budget_snapshot_t rr_provisional;
    memset (&rr_provisional, 0, sizeof (rr_provisional));
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      &hold_reply_after_first_physical_prefix, &rr_prefix_gate);
    std::thread rr_prefix_observer ([&] {
        rr_prefix_entered = wait_for_reply_prefix_gate (&rr_prefix_gate);
        if (rr_prefix_entered)
            rr_provisional_snapshot_rc =
              read_budget_snapshot_unchecked (&rr_provisional);
        release_reply_prefix_gate (&rr_prefix_gate);
    });
    const zlink_submit_result_t rr_final_result = zlink_reply_part (
      second, &rr_request.source_rid, rr_request.reply_token, &rr_final,
      ZLINK_PART_FINAL);
    rr_prefix_observer.join ();
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      NULL, NULL);
    TEST_ASSERT_TRUE (rr_prefix_entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, rr_provisional_snapshot_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, rr_final_result);
    assert_consumed (&rr_final);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_provisional.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_provisional.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_provisional.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (
      rr_provisional.completion_current_accounted_bytes
      > rr_baseline.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_provisional.completion_peak_accounted_bytes
                      >= rr_provisional.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_pending_message_count,
      rr_provisional.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_provisional.completion_current_accounted_bytes
        - rr_baseline.completion_current_accounted_bytes,
      rr_provisional.total_messaging_accounted_bytes
        - rr_baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_TRUE (([&] {
        const zlink_auto_hwm_budget_snapshot_t snapshot =
          read_budget_snapshot ();
        return snapshot.completion_pending_message_count
                 > rr_baseline.completion_pending_message_count
               && snapshot.completion_current_accounted_bytes
                    > rr_baseline.completion_current_accounted_bytes;
    }) ());
    const zlink_auto_hwm_budget_snapshot_t rr_queued = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_queued.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_queued.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_queued.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_current_accounted_bytes
                      > rr_baseline.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_peak_accounted_bytes
                      >= rr_queued.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_pending_message_count
                      > rr_baseline.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_queued.completion_current_accounted_bytes
        - rr_baseline.completion_current_accounted_bytes,
      rr_queued.total_messaging_accounted_bytes
        - rr_baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.active_directional_queue_count,
                              rr_queued.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.active_completion_directional_queue_count,
      rr_queued.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, rr_queued.application_accounted_bytes);

    zlink_completion_t rr_completion = receive_completion (first);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, rr_completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (rr_request_id, rr_completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, rr_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, rr_completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&rr_completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&rr_completion.reply_parts[1]));
    zlink_completion_close (&rr_completion);
    rr.cores[0]->release_completion_poller (&rr);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (first);
}
void assert_physical_pair_topology_by_id (void *socket_, uint64_t pair_id,
                                          uint64_t generation,
                                          bool router_router_)
{
    TEST_ASSERT_NOT_EQUAL (0, pair_id);
    TEST_ASSERT_NOT_EQUAL (0, generation);
    zlink::pipe_t *const observed =
      as_socket_handle (socket_).socket->test_pair_pipe (pair_id, generation, false);
    TEST_ASSERT_NOT_NULL (observed);
    const zlink::blob_t &identity = observed->get_transport_peer_identity ();
    uint64_t resolved_pair_id = 0;
    uint64_t resolved_generation = 0;
    bool ready = false;
    TEST_ASSERT_TRUE (as_socket_handle (socket_).socket->test_pair_identity_for_peer (
      identity.data (), identity.size (), &resolved_pair_id, &resolved_generation,
      &ready));
    TEST_ASSERT_TRUE (ready);
    TEST_ASSERT_EQUAL_UINT64 (pair_id, resolved_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (generation, resolved_generation);
    zlink::pipe_t *const application =
      as_socket_handle (socket_).socket->test_pair_pipe (pair_id, generation, false);
    zlink::pipe_t *const completion =
      as_socket_handle (socket_).socket->test_pair_pipe (pair_id, generation, true);
    TEST_ASSERT_NOT_NULL (application);
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           application->get_transport_lane ());
    TEST_ASSERT_EQUAL_UINT (router_router_ ? 2u : 1u,
                            application->get_transport_lane_count ());
    if (router_router_) {
        TEST_ASSERT_NOT_NULL (completion);
        TEST_ASSERT_EQUAL_INT (zlink::transport_lane_completion,
                               completion->get_transport_lane ());
        TEST_ASSERT_EQUAL_UINT (2u, completion->get_transport_lane_count ());
    } else {
        TEST_ASSERT_NULL (completion);
    }
}


void test_single_lane_topology_is_owned_by_the_pair ()
{
    const zlink_socket_type_t second_types[] = {ZLINK_SOCKET_DEALER,
                                               ZLINK_SOCKET_ROUTER};
    for (size_t i = 0; i != 2; ++i) {
        void *first = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *second = test_context_socket (second_types[i]);
        contract_socket_pair_t pair (first, second);
        assert_physical_pair_topology_by_id (first, 1, 1, i == 1);
        assert_physical_pair_topology_by_id (second, 1, 1, i == 1);
        pair.application[0]->terminate (false);
        pair.pump ();
        contract_socket_pair_t replacement (first, second, 1, 2);
        assert_physical_pair_topology_by_id (first, 1, 2, i == 1);
        assert_physical_pair_topology_by_id (second, 1, 2, i == 1);
        test_context_socket_close_zero_linger (first);
        test_context_socket_close_zero_linger (second);
    }
}

void test_backpressured_final_releases_the_helper_sequence ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const uint64_t one_pending = 1;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_PENDING_MAX_MSGS,
                        &one_pending, sizeof (one_pending)));
    contract_socket_pair_t pair (dealer, router, 1, 1, true, 256);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer, ZLINK_RECEIVE_FLOW_PAUSED));
    pair.pump ();
    TEST_ASSERT_TRUE (pair.cores[1]->application_pipe_remote_flow_paused (1, 1));
    bool reached_backpressure = false;
    for (size_t attempt = 0; attempt != 32; ++attempt) {
        zlink_msg_t part;
        init_sized_part (&part, 1024, 'x');
        zlink_completion_id_t completion_id = 0;
        const zlink_submit_result_t result = zlink_send_part_rid (
          router, &pair.rids[0], &part, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, NULL, &completion_id);
        assert_consumed (&part);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            reached_backpressure = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (reached_backpressure);
    bool routed_send_sequence_still_active = false;
    int routed_send_sequence_family = -1;
    const std::shared_ptr<zlink::part_helper_internal::handle_state_t>
      helper_state = zlink::part_helper_internal::find_socket_state (pair.cores[1]);
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        routed_send_sequence_still_active = helper_state->send.active;
        routed_send_sequence_family = static_cast<int> (helper_state->send.spec.family);
    }
    char diagnostic[96];
    snprintf (diagnostic, sizeof (diagnostic),
              "failed FINAL retained send sequence family=%d",
              routed_send_sequence_family);
    TEST_ASSERT_FALSE_MESSAGE (routed_send_sequence_still_active, diagnostic);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_application_hwm_and_remote_pause_remain_independent_blockers ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int no_wait = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &no_wait, sizeof (no_wait)));
    const uint64_t hwm = 3u * (1024u + sizeof (zlink::msg_t));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    contract_socket_pair_t pair (dealer, router, 1, 1, true, hwm);
    size_t accepted = 0;
    for (; accepted != 256; ++accepted) {
        zlink_msg_t part;
        init_sized_part (&part, 1024, 'h');
        const zlink_submit_result_t result = zlink_send_part (
          dealer, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
        assert_consumed (&part);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (accepted > 0 && accepted < 256);
    pair.pump ();
    bool active = false;
    bool hwm_full = false;
    bool remote_paused = false;
    TEST_ASSERT_TRUE (pair.cores[0]->test_application_pipe_flow_probe (
      1, 1, &active, &hwm_full, &remote_paused));
    TEST_ASSERT_FALSE (active);
    TEST_ASSERT_TRUE (hwm_full);
    TEST_ASSERT_FALSE (remote_paused);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (router, ZLINK_RECEIVE_FLOW_PAUSED));
    pair.pump ();
    TEST_ASSERT_TRUE (pair.cores[0]->test_application_pipe_flow_probe (
      1, 1, &active, &hwm_full, &remote_paused));
    TEST_ASSERT_FALSE (active);
    TEST_ASSERT_TRUE (hwm_full);
    TEST_ASSERT_TRUE (remote_paused);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_sl_flow_snapshot_accounts_dr_reply_as_application);
    RUN_TEST (test_single_lane_topology_is_owned_by_the_pair);
    RUN_TEST (test_backpressured_final_releases_the_helper_sequence);
    RUN_TEST (test_application_hwm_and_remote_pause_remain_independent_blockers);
    return UNITY_END ();
}
