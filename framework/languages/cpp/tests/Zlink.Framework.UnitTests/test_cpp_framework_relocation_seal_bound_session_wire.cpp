/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include "../support/relocation_seal_execution_fixture.hpp"

using namespace framework_relocation_seal_fixture;

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, malformed_command_42_records_invalid_frame_dispatch_error)
{
    dispatch_options_t dispatch;
    dispatch.message_flow (message_flow_log_mode_t::normal);
    auto observations = std::make_shared<
      zlink::framework::test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [observations] (const auto &event) { observations->record (event); });

    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("malformed-session-seal-owner"),
        .dispatch = dispatch,
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto source_rid = zlink::routing_id_t::from ("malformed-command-42-source");
    const std::vector<std::uint8_t> malformed{0x5a, 0x4d, 0x01, 0x2a, 0x00};
    ASSERT_TRUE (
      owner->transport ().mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
        "malformed-command-42", mesh::service_mailbox_domain_t::infrastructure,
        zlink::framework::detail::backend::materialize_binding_parts ({malformed}),
        source_rid.to_bytes (), std::nullopt, 1)));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    try {
        (void) owner->dispatch_ready (noop_dispatch).result ();
    }
    catch (const protocol::service_wire_error_t &) {
    }

    zlink::framework::test::acknowledge_delivery ();
    const auto errors = observations->snapshot ();
    ASSERT_EQ (errors.size (), 1u);
    const auto &error = errors.front ();
    EXPECT_EQ (error.surface, dispatch_error_surface_t::stream_session);
    EXPECT_EQ (error.message_kind, dispatch_message_kind_t::control);
    EXPECT_EQ (error.reason, dispatch_error_reason_t::invalid_frame);
    EXPECT_EQ (error.action, dispatch_error_action_t::drop);
    EXPECT_EQ (error.packet_name, "sessionRelocationSeal");
}

TEST (FrameworkRelocationSealContract, binding_absent_wire_result_round_trips_through_runtime)
{
    protocol::session_relocation_sealed_t record{{81, 82},
                                                 {"coordinator", 7, {'c'}, 1, "store-v1"},
                                                 {"actor", 3, {'a'}, 1, 5, 9},
                                                 {'s'},
                                                 1,
                                                 "session-owner",
                                                 11,
                                                 {'r'},
                                                 13};
    auto generated = protocol::decode_session_relocation_sealed_43 (
      protocol::encode_session_relocation_sealed (record));
    generated.seal_result = protocol::service_wire_pilot_session_seal_result::binding_absent;
    const auto foreign_wire = protocol::encode_session_relocation_sealed_43 (generated);
    EXPECT_EQ (protocol::encode_session_relocation_sealed (
                 protocol::decode_session_relocation_sealed (foreign_wire)),
               foreign_wire);
}

TEST (FrameworkRelocationSealContract, bound_close_preserves_transport_terminal_and_other_bindings)
{
    stateful::stream_session_registry_t sessions (
      [] (const std::string &) -> std::optional<stateful::object_ref_t> { return std::nullopt; });
    task_completion_source_t<void> transport;
    int closes = 0;
    const auto rid = zlink::routing_id_t::from ("terminal-session");
    const auto connection = sessions.open (rid.to_hex (), {}, {}, 0, [&] {
        ++closes;
        return transport.task ();
    });
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "terminal-actor", 7, 11, "player", "actor-owner"};
    const auto [error, binding] = sessions.bind_remote (connection, actor, 1, 13);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    auto other_actor = actor;
    other_actor.key = "other-actor";
    const auto other_connection = sessions.open ("other-session");
    const auto [other_error, other_binding] =
      sessions.bind_remote (other_connection, other_actor, 1, 13);
    ASSERT_EQ (other_error, stateful::stateful_error_t::none);
    const protocol::retired_bound_session_route_fence_t owner{{'n'}, 1, "session-owner", 17, {}, 0};
    const protocol::bound_session_close_t request{
      1,
      actor.key,
      actor.object_generation,
      {{'n'}, 1, "session-owner", 17, rid.to_bytes (), binding.binding_generation}};
    auto pending = sessions.close_current (request, owner);
    EXPECT_EQ (closes, 1);
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_TRUE (sessions.is_current (binding));
    EXPECT_TRUE (sessions.is_current (other_binding));
    transport.complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "transport close failed"));
    const auto failed = pending.result ();
    ASSERT_FALSE (failed);
    EXPECT_EQ (failed.error_kind (), framework_error_kind_t::unavailable);
    EXPECT_STREQ (failed.error ()->what (), "transport close failed");
    EXPECT_TRUE (sessions.is_current (other_binding));
}
