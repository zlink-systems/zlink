/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zlink::framework::runtime::stateful
{
struct stream_session_registry_test_access_t
{
    static bool wait_for_held_drain (stream_session_registry_t &registry,
                                     const std::string &actor_id,
                                     std::chrono::steady_clock::time_point deadline)
    {
        for (;;) {
            const auto observed = registry._changed_generation.load (std::memory_order_acquire);
            if (!registry.remote_route_sealed (actor_id))
                return true;
            std::unique_lock lock (registry._changed_mutex);
            if (!registry._changed.wait_until (lock, deadline, [&] {
                    return registry._changed_generation.load (std::memory_order_acquire)
                           != observed;
                }))
                return false;
        }
    }
};
} // namespace zlink::framework::runtime::stateful

namespace zlink::framework::test::actor_maintenance_join
{
struct sealed_gap_binding_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<fw::actor_ref_t> actor;
    std::optional<fw::session_actor_t> bound;
    std::vector<fw::message_flow_event_t> flows;
};

class sealed_gap_session_t final : public fw::packet_stream_session_t
{
  public:
    static inline std::shared_ptr<sealed_gap_binding_t> binding;
    sealed_gap_session_t () : _binding (binding) {}
    fw::task_t<void> on_connected (fw::stream_t &stream) override
    {
        const auto actor = [&] {
            std::lock_guard lock (_binding->mutex);
            return *_binding->actor;
        }();
        co_await stream.actors ().bind_or_get (actor).async ();
        {
            std::lock_guard lock (_binding->mutex);
            _binding->bound = stream.actors ().bound ().front ();
        }
        _binding->changed.notify_all ();
    }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &,
                                const fw::session_message_context_t &,
                                const zlink::framework::message_t &) override
    {
        co_return;
    }

  private:
    std::shared_ptr<sealed_gap_binding_t> _binding;
};

void expired_sealed_request_preserves_next_live_actor_relay (bool remote)
{
    namespace st = fw::runtime::stateful;
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto binding = std::make_shared<sealed_gap_binding_t> ();
    sealed_gap_session_t::binding = binding;
    struct binding_reset_t
    {
        ~binding_reset_t ()
        {
            sealed_gap_session_t::binding->bound.reset ();
            sealed_gap_session_t::binding.reset ();
        }
    } binding_reset;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t owner, session;
    configure (owner, evidence_owner, first_rid, store, relocations);
    const auto observe = [binding] (const fw::message_flow_event_t &event) {
        std::lock_guard lock (binding->mutex);
        binding->flows.push_back (event);
        binding->changed.notify_all ();
    };
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      owner.app.add_zlink_framework ().configure_dispatch (), observe);
    if (!remote)
        owner.app.add_zlink_framework ()
          .add_stream_node (stream_name)
          .bind ("tcp://127.0.0.1:0")
          .register_session<sealed_gap_session_t> ();
    owner.start ();
    ASSERT_TRUE (ready (owner));
    auto services = owner.app.advanced ().services ().build_provider ();
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    ASSERT_TRUE (std::holds_alternative<fw::actor_create_created_t> (created.value ()));
    const auto actor = std::get<fw::actor_create_created_t> (created.value ()).actor;
    {
        std::lock_guard lock (binding->mutex);
        binding->actor = actor;
    }
    host_t *source = &owner;
    if (remote) {
        configure_session<sealed_gap_session_t> (session, evidence_owner, store, relocations);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          session.app.add_zlink_framework ().configure_dispatch (), observe);
        session.start ();
        ASSERT_TRUE (ready (session));
        source = &session;
    }
    auto source_services = source->app.advanced ().services ().build_provider ();
    if (remote) {
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
        ASSERT_TRUE (peer_ready (source_services.get_required<fw::route_mesh_runtime_t> ()));
    }
    view_client_t connection (source_services.get_required<fw::framework_runtime_t> ()
                                .listener_status (fw::listener_kind_t::stream, stream_name)
                                .endpoint);
    ASSERT_TRUE (connection.connect ());
    std::optional<fw::session_actor_t> bound;
    {
        std::unique_lock lock (binding->mutex);
        ASSERT_TRUE (
          binding->changed.wait_for (lock, 2s, [&] { return binding->bound.has_value (); }));
        bound = binding->bound;
    }
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (source->app.advanced ().zlink ()).front ();
    const auto native = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    ASSERT_TRUE (native);
    auto &sessions = native->sessions ();
    const auto current = sessions.current_binding (actor_id);
    ASSERT_TRUE (current);
    const auto sealed = sessions.seal_remote_route (
      current->connection.connection_id, current->binding_generation, current->actor.key,
      current->actor.object_generation, current->owner_lease_generation);
    ASSERT_EQ (sealed.error, st::stateful_error_t::none);
    EXPECT_EQ (sealed.last_accepted_sequence, 0u);
    struct seal_release_t
    {
        st::stream_session_registry_t &sessions;
        std::optional<st::stream_barrier_t> barrier;
        ~seal_release_t ()
        {
            if (barrier)
                (void) sessions.abort_barrier (*barrier);
        }
    } release{sessions, sealed.barrier};
    fw::detail::stream_header_t request_header (
      fw::detail::stream_message_kind_t::request, fw::stream_codec_t::json,
      fw::detail::stream_header_flags_t::none, 17, probe_t::packet_name);
    request_header.with_correlation_id ("sealed-expired-request");
    request_header.with_flow ("0199a500-0000-7000-8000-000000000011", fw::flow_origin_t::inbound);
    {
        const fw::detail::stream_relay_dispatch_scope_t dispatch (request_header);
        auto request = bound->relay_request (zlink::framework::message_t::from (probe_t{1}))
                         .timeout (50ms)
                         .async ();
        const auto expired = request.result_for (2s);
        ASSERT_TRUE (expired);
        EXPECT_FALSE (*expired);
        EXPECT_EQ (expired->error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    }
    EXPECT_EQ (native->pending_operation_count (), 0u);
    fw::detail::stream_header_t send_header (
      fw::detail::stream_message_kind_t::send, fw::stream_codec_t::json,
      fw::detail::stream_header_flags_t::none, std::nullopt, record_t::packet_name);
    send_header.with_correlation_id ("sealed-next-record");
    send_header.with_flow ("0199a500-0000-7000-8000-000000000012", fw::flow_origin_t::inbound);
    {
        const fw::detail::stream_relay_dispatch_scope_t dispatch (send_header);
        auto sent = bound->relay (zlink::framework::message_t::from (record_t{2}));
        const auto accepted = sent.result_for (2s);
        ASSERT_TRUE (accepted);
        ASSERT_TRUE (*accepted) << "one-way must complete while the matching seal is retained";
    }
    EXPECT_TRUE (sessions.remote_route_sealed (actor_id));
    ASSERT_EQ (sessions.abort_barrier (sealed.barrier), st::stateful_error_t::none);
    release.barrier.reset ();
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (
          evidence.changed.wait_for (lock, 2s, [&] { return !evidence.recorded.empty (); }));
        EXPECT_EQ (evidence.recorded, (std::vector<int>{2}));
        EXPECT_TRUE (evidence.intake_probe_values.empty ());
    }
    const auto receiver_route = services.get_required<fw::detail::actor_gateway_runtime_t> ()
                                  .bound_session_route_async (actor)
                                  .result ()
                                  .value ();
    ASSERT_TRUE (receiver_route);
    EXPECT_EQ (receiver_route->session_sequence, 2u);
    EXPECT_EQ (receiver_route->binding_generation, current->binding_generation);
    ASSERT_TRUE (receiver_route->session_rid);
    EXPECT_EQ (receiver_route->session_rid->to_hex (), current->connection.connection_id);
    EXPECT_EQ (receiver_route->node_rid.to_hex (), native->status ().routing_id ().to_hex ());
    const auto drain_deadline = std::chrono::steady_clock::now () + 2s;
    {
        std::unique_lock lock (binding->mutex);
        ASSERT_TRUE (binding->changed.wait_until (lock, drain_deadline, [&] {
            return std::any_of (
              binding->flows.begin (), binding->flows.end (), [] (const auto &event) {
                  return event.packet_name == record_t::packet_name
                         && event.detail_stage == "invoke_erased.application_handler"
                         && event.detail_result == "result=finished success=true";
              });
        }));
        std::size_t admitted = 0;
        for (const auto &event : binding->flows) {
            if (event.detail_stage != "canonical_application.commit_accepted_ingress")
                continue;
            EXPECT_NE (event.packet_name, probe_t::packet_name);
            if (event.packet_name != record_t::packet_name)
                continue;
            ++admitted;
            EXPECT_EQ (event.actor_id, actor_id);
            EXPECT_EQ (event.correlation_id, "sealed-next-record");
            EXPECT_EQ (event.flow_id, "0199a500-0000-7000-8000-000000000012");
            ASSERT_TRUE (event.detail_result);
            EXPECT_NE (event.detail_result->find ("operation_high="), std::string::npos);
            EXPECT_NE (event.detail_result->find (" operation_low="), std::string::npos);
            EXPECT_EQ (event.detail_result->find ("operation_high=0 operation_low=0"),
                       std::string::npos);
        }
        EXPECT_EQ (admitted, 1u);
    }
    EXPECT_EQ (native->pending_operation_count (), 0u);
    ASSERT_TRUE (st::stream_session_registry_test_access_t::wait_for_held_drain (sessions, actor_id,
                                                                                 drain_deadline));
    const auto drained = sessions.seal_remote_route (
      current->connection.connection_id, current->binding_generation, current->actor.key,
      current->actor.object_generation, current->owner_lease_generation);
    ASSERT_EQ (drained.error, st::stateful_error_t::none)
      << " binding_generation=" << current->binding_generation
      << " expected_owner_lease=" << current->owner_lease_generation
      << " current_binding_generation="
      << (drained.binding ? drained.binding->binding_generation : 0)
      << " current_owner_lease=" << (drained.binding ? drained.binding->owner_lease_generation : 0)
      << " current_actor=" << (drained.binding ? drained.binding->actor.key : "<absent>")
      << " expected_actor_generation=" << current->actor.object_generation
      << " current_actor_generation="
      << (drained.binding ? drained.binding->actor.object_generation : 0);
    release.barrier = drained.barrier;
    EXPECT_EQ (drained.last_accepted_sequence, 2u);
    EXPECT_TRUE (sessions.remote_route_seal_ready (drained.barrier));
    ASSERT_EQ (sessions.abort_barrier (drained.barrier), st::stateful_error_t::none);
    release.barrier.reset ();
}

TEST (BoundSessionSequenceGap, LocalExpiredHeldRequestDoesNotBlockNextLiveActor)
{
    expired_sealed_request_preserves_next_live_actor_relay (false);
}
TEST (BoundSessionSequenceGap, RemoteExpiredHeldRequestDoesNotBlockNextLiveActor)
{
    expired_sealed_request_preserves_next_live_actor_relay (true);
}
} // namespace zlink::framework::test::actor_maintenance_join
