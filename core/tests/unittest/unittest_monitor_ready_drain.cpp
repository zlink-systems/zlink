/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"
#include "contract_zmp_engine_fixture.hpp"

SETUP_TEARDOWN_TESTCONTEXT

struct passive_ready_write_drain_gate_t
{
    passive_ready_write_drain_gate_t () :
        arrivals (0),
        pair_id (0),
        pair_generation (0),
        identity_consistent (true),
        core (NULL),
        ready_count_after_drain (0),
        pair_ready_after_drain (false)
    {
    }
    unsigned int arrivals;
    uint64_t pair_id, pair_generation;
    bool identity_consistent;
    zlink::socket_base_t *core;
    uint32_t ready_count_after_drain;
    bool pair_ready_after_drain;
};

void observe_passive_ready_after_write_drain (uint64_t pair_id_, uint64_t generation_, void *data_)
{
    passive_ready_write_drain_gate_t *gate =
      static_cast<passive_ready_write_drain_gate_t *> (data_);
    if (gate->arrivals == 0) {
        gate->pair_id = pair_id_;
        gate->pair_generation = generation_;
    } else if (gate->pair_id != pair_id_ || gate->pair_generation != generation_)
        gate->identity_consistent = false;
    gate->ready_count_after_drain |= gate->core->test_monitor_ready_count ();
    gate->pair_ready_after_drain =
      gate->pair_ready_after_drain || gate->core->test_pair_is_ready (pair_id_, generation_);
    ++gate->arrivals;
}

void run_passive_paired_ready_waits_for_ready_reply_write_drain (
  int peer_type_, int server_type_, unsigned int expected_drain_arrivals_)
{
    void *server = test_context_socket (server_type_);
    TEST_ASSERT_NOT_NULL (server);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "PASSIVE", 7));
    {
        contract_zmp_monitor_t monitor (server,
                                        ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED);
        passive_ready_write_drain_gate_t gate;
        gate.core = as_socket_handle (server).socket;
        zlink::test_set_zmp_passive_ready_write_drained_hook (
          &observe_passive_ready_after_write_drain, &gate);
        {
            contract_zmp_engine_t application (server);
            application.state->hold_writes = true;
            application.handshake (peer_type_, "ACTIVE", expected_drain_arrivals_, 0);
            const uint64_t pair_id = application.session->transport_pair_id ();
            const uint64_t generation = application.session->transport_pair_generation ();
            TEST_ASSERT_TRUE (pair_id != 0);
            TEST_ASSERT_TRUE (generation != 0);
            TEST_ASSERT_FALSE (application.state->writes.empty ());
            TEST_ASSERT_EQUAL_UINT (0, gate.arrivals);
            TEST_ASSERT_EQUAL_UINT32 (0, application.core->test_monitor_ready_count ());
            TEST_ASSERT_FALSE (application.core->test_pair_is_ready (pair_id, generation));

            std::unique_ptr<contract_zmp_engine_t> completion;
            if (expected_drain_arrivals_ == 2) {
                completion.reset (new contract_zmp_engine_t (server));
                completion->state->hold_writes = true;
                completion->handshake (peer_type_, "ACTIVE", 2, 1);
                TEST_ASSERT_FALSE (completion->state->writes.empty ());
                TEST_ASSERT_EQUAL_UINT64 (pair_id, completion->session->transport_pair_id ());
                TEST_ASSERT_EQUAL_UINT64 (generation,
                                          completion->session->transport_pair_generation ());
            }
            application.state->hold_writes = false;
            application.state->drain_writes ();
            application.pump ();
            if (completion) {
                TEST_ASSERT_EQUAL_UINT32 (0, application.core->test_monitor_ready_count ());
                TEST_ASSERT_FALSE (application.core->test_pair_is_ready (pair_id, generation));
                completion->state->hold_writes = false;
                completion->state->drain_writes ();
                completion->pump ();
                application.pump ();
            }
            TEST_ASSERT_EQUAL_UINT (expected_drain_arrivals_, gate.arrivals);
            TEST_ASSERT_TRUE (gate.identity_consistent);
            TEST_ASSERT_EQUAL_UINT32 (0, gate.ready_count_after_drain);
            TEST_ASSERT_FALSE (gate.pair_ready_after_drain);
            TEST_ASSERT_EQUAL_UINT64 (pair_id, gate.pair_id);
            TEST_ASSERT_EQUAL_UINT64 (generation, gate.pair_generation);
            TEST_ASSERT_TRUE (
              zlink::session_termination_test_access_t::attached_pipe_connection_ids_are_live (
                application.core, expected_drain_arrivals_));
            TEST_ASSERT_EQUAL_UINT32 (1, application.core->test_monitor_ready_count ());
            TEST_ASSERT_TRUE (application.core->test_pair_is_ready (pair_id, generation));
            zlink::socket_monitor_event_record_t ready_event;
            TEST_ASSERT_TRUE (monitor.next (&ready_event));
            TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_CONNECTION_READY, ready_event.event);
            TEST_ASSERT_EQUAL_UINT64 (1, ready_event.values[0]);
            TEST_ASSERT_TRUE (ready_event.endpoint_uri_pair.connection_id.load () != 0);
            TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                                    ready_event.transport_lane);
            TEST_ASSERT_TRUE (
              (ready_event.internal_flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE) != 0);
            zlink::test_set_zmp_passive_ready_write_drained_hook (NULL, NULL);
        }
    }
    test_context_socket_close_zero_linger (server);
}

void test_passive_single_lane_ready_waits_for_ready_reply_write_drain ()
{
    run_passive_paired_ready_waits_for_ready_reply_write_drain (test_zmp_wire::socket_dealer,
                                                                ZLINK_SOCKET_ROUTER, 1);
}
void test_passive_router_pair_ready_waits_for_both_ready_reply_write_drains ()
{
    run_passive_paired_ready_waits_for_ready_reply_write_drain (test_zmp_wire::socket_router,
                                                                ZLINK_SOCKET_ROUTER, 2);
}
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_passive_single_lane_ready_waits_for_ready_reply_write_drain);
    RUN_TEST (test_passive_router_pair_ready_waits_for_both_ready_reply_write_drains);
    return UNITY_END ();
}
