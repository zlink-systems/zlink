/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
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

namespace zlink::framework::runtime
{
struct in_memory_location_repository_test_access_t
{
    static std::promise<void> hold_lane (in_memory_location_repository_t &store)
    {
        std::promise<void> release, entered;
        auto held = release.get_future ().share ();
        auto running = entered.get_future ();
        if (!store._lane.try_post ([held, &entered] {
                entered.set_value ();
                held.wait ();
            }))
            throw std::runtime_error ("store lane is closed");
        running.wait ();
        return release;
    }
};
}

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static stateful::raw_stateful_dispatch_t &dispatch (public_host_runtime_t &host)
    {
        return *host._stateful_dispatch;
    }

    static auto dispatch_claim (public_host_runtime_t &host,
                                std::shared_ptr<mesh::service_mailbox_claim_t> claim,
                                mesh::service_mailbox_t::application_dispatch_t dispatch)
    {
        return host.dispatch_application_claim (std::move (claim), std::move (dispatch));
    }
    static auto dispatch_owner (public_host_runtime_t &host,
                                stateful::object_ref_t owner,
                                mesh::service_mailbox_t::application_dispatch_t dispatch)
    {
        return host.dispatch_stateful_owner (owner, dispatch);
    }

    static auto cut (public_host_runtime_t &host, std::vector<stateful::object_ref_t> owners)
    {
        return host.cut_relocation_source (
          std::move (owners), std::make_shared<stateful::relocation_seal_binding_t> ());
    }
    static auto ingest_cut (public_host_runtime_t &host,
                            stateful::object_ref_t owner,
                            mesh::service_mailbox_record_t original)
    {
        return host.ingest_cut_original (std::move (owner), std::move (original));
    }

    static void stage_relay (public_host_runtime_t &host,
                             const protocol::relocation_prepare_t &prepare,
                             const stateful::object_ref_t &source,
                             const stateful::object_ref_t &target)
    {
        public_host_runtime_t::relocation_target_attempt_t attempt;
        attempt.prepare = prepare;
        attempt.sources = {source};
        attempt.targets = {target};
        host._relocation_target_attempts[{1, 2, 1}] = std::move (attempt);
    }

    template <typename Header>
    static auto accept_relay (public_host_runtime_t &host,
                              const stateful::object_ref_t &target,
                              const Header &header,
                              std::vector<std::uint8_t> source)
    {
        auto retained = zlink::framework::test::retained_ingress_snapshot (host._objects, target);
        if constexpr (std::is_same_v<Header, protocol::actor_message_header_t>)
            return host._stateful_dispatch
              ->accepted_actor_authority (target, header, std::move (source), 1, std::nullopt,
                                          false, std::move (retained))
              .result ()
              .value ();
        else
            return host._stateful_dispatch
              ->accepted_spot_authority (target, header, std::move (source), 1, std::nullopt,
                                         std::move (retained))
              .result ()
              .value ();
    }
};
} // namespace zlink::framework::runtime::host

namespace zlink::framework::runtime::stateful
{
struct stateful_object_runtime_test_access_t
{
    // Holds the object state lane in one turn until the returned promise is set.
    static std::promise<void> hold_lane (stateful_object_runtime_t &objects)
    {
        std::promise<void> release;
        std::promise<void> entered;
        auto held = release.get_future ().share ();
        auto running = entered.get_future ();
        if (!objects._lane.try_post ([held, &entered] {
                entered.set_value ();
                held.wait ();
            }))
            throw std::runtime_error ("object lane is closed");
        running.wait ();
        return release;
    }
};
} // namespace zlink::framework::runtime::stateful

namespace
{

using namespace zlink::framework;
using namespace zlink::framework::detail;
namespace host = zlink::framework::runtime::host;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;

mesh::service_node_descriptor_t test_descriptor (std::string routing_id)
{
    const std::vector<std::uint8_t> routing_bytes (routing_id.begin (), routing_id.end ());
    return {"relocation-seal-test",
            routing_bytes,
            1,
            1,
            "tcp://127.0.0.1:0",
            {},
            mesh::service_node_state_t::preparing};
}

// Waits for an observation another thread makes true.
template <typename Predicate> bool eventually (Predicate predicate)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!predicate ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::sleep_for (5ms);
    }
    return true;
}

mesh::service_mailbox_record_t spot_send (host::public_host_runtime_t &host,
                                          const stateful::object_ref_t &spot,
                                          std::uint64_t operation,
                                          std::uint8_t source,
                                          std::optional<std::uint64_t> correlation = std::nullopt)
{
    mesh::service_mailbox_record_t record;
    record.owner = mesh::service_mailbox_t::application_owner (host::owner_kind_t::spot, spot.key);
    record.domain = mesh::service_mailbox_domain_t::application;
    record.source_routing_id = {source};
    record.source_node_generation = 1;
    record.correlation = correlation;
    record.parts = detail::backend::materialize_binding_parts (
      {protocol::encode_spot_message_header (
         correlation ? protocol::command::spotRequest : protocol::command::spotSend, "source-spot",
         protocol::spot_route_fence_t{spot.key, spot.object_generation, {1}, 1, 1, 3},
         {1, operation}, correlation),
       protocol::encode_application_payload ({"Probe", "application/json", {'{', '}'}})});
    record.source_original = mesh::raw_mesh_node_owner_t::local_accepted_source (
      record.source_routing_id, record.source_node_generation, {});
    return record;
}

mesh::service_mailbox_record_t native_spot_record (host::public_host_runtime_t &host,
                                                   const stateful::object_ref_t &spot,
                                                   std::uint64_t operation,
                                                   std::optional<std::uint64_t> correlation)
{
    auto wire = spot_send (host, spot, operation, 2, correlation);
    runtime::messaging::envelope_header_t envelope;
    envelope.kind = correlation ? runtime::messaging::message_kind_t::request
                                : runtime::messaging::message_kind_t::command;
    envelope.channel_name = "mesh";
    envelope.message_name = "NativeProbe";
    std::vector<zlink::message_t> parts;
    parts.push_back (runtime::messaging::envelope_codec_t{}.encode_header (envelope));
    parts.push_back (zlink::message_t::from ("{}"));
    wire.parts.back () = detail::backend::materialize_binding_part (
      protocol::encode_application_payload (protocol::application_payload_t::from_parts (parts)));
    return wire;
}

mesh::service_mailbox_record_t local_native_spot_record (host::public_host_runtime_t &host,
                                                         const stateful::object_ref_t &spot,
                                                         std::uint64_t operation)
{
    auto original = native_spot_record (host, spot, operation, std::nullopt);
    const auto bytes = protocol::message_bytes (original.parts.front ());
    const auto header =
      protocol::decode_spot_message_header (bytes, protocol::decode_header (bytes).kind);
    original.application = std::make_shared<host::local_application_dispatch_t> ();
    original.application->owner.owner_kind = host::owner_kind_t::spot;
    original.application->owner.spot_id = spot.key;
    original.application->record.kind = host::record_kind_t::spot_send;
    original.application->record.operation_id = {1, operation};
    original.application->record.source_node_rid =
      zlink::routing_id_t::from (original.source_routing_id);
    original.application->record.spot_route = header.target;
    original.application->parts =
      protocol::decode_application_parts (std::move (original.parts.back ()));
    original.parts.clear ();
    return original;
}

std::uint64_t operation_of (const stateful::turn_record_t &turn)
{
    if (turn.frozen_record)
        return turn.frozen_record->operation.low;
    return turn.application_record ? turn.application_record->operation.low : 0;
}

std::vector<std::uint64_t> operations_of (const std::vector<stateful::turn_record_t> &turns)
{
    std::vector<std::uint64_t> operations;
    for (const auto &turn : turns)
        operations.push_back (operation_of (turn));
    return operations;
}

} // namespace

TEST (FrameworkRelocationSealContract, public_actor_push_submits_while_session_route_is_sealed)
{
    const auto actor_node = zlink::routing_id_t::from ("seal-actor-owner");
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string (actor_node.to_string ()),
                                                 "player", "seal-actor", 7);
    constexpr std::uint64_t actor_authority_generation = 11;
    constexpr std::uint64_t actor_node_generation = 1;
    constexpr std::uint64_t session_owner_lease_generation = 17;

    // The send callback is held by `session_owner`; it owns its state.
    const auto deliveries = std::make_shared<std::atomic_int> (0);
    const auto delivered = std::make_shared<std::promise<void>> ();
    auto delivery = delivered->get_future ();
    auto session_owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("seal-session-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto connection = session_owner->sessions ().open ("seal-session-rid");
    const stateful::object_ref_t session_actor{
      stateful::object_kind_t::actor, "seal-actor", actor.object_generation (),
      actor_authority_generation,     "player",     actor_node.to_string ()};
    const auto [bind_error, binding] = session_owner->sessions ().bind_remote (
      connection, session_actor, actor_node_generation, session_owner_lease_generation);
    ASSERT_EQ (bind_error, stateful::stateful_error_t::none);
    const auto sealed = session_owner->sessions ().seal_remote_route (
      connection.connection_id, binding.binding_generation, session_actor.key,
      session_actor.object_generation, binding.owner_lease_generation);
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);

    host::bound_session_operations_t operations;
    operations.bind =
      [] (const protocol::bound_session_bind_t &bind, const zlink::routing_id_t &, std::uint64_t,
          const location_owner_token_t &,
          std::function<bool (const protocol::stream_bind_reply_tail_t &)> submit_terminal_reply) {
          (void) submit_terminal_reply (
            {bind.binding.generation, bind.actor.authority_owner_generation});
          return host::bound_session_bind_operation_result_t{};
      };
    operations.send = [deliveries, delivered] (const protocol::bound_session_send_t &,
                                               std::vector<zlink::message_t> parts) {
        if (parts.size () != 1)
            return stateful::stateful_error_t::invalid;
        deliveries->fetch_add (1, std::memory_order_release);
        delivered->set_value ();
        return stateful::stateful_error_t::none;
    };
    operations.replaced = [] (const protocol::bound_session_replaced_t &) {};
    operations.commit_relocation_route = [] (const protocol::session_relocation_route_t &,
                                             const stateful::stream_binding_t &,
                                             const stateful::stream_binding_t &) { return false; };
    operations.prepare_relocation_target_route = [] (const protocol::session_relocation_route_t &,
                                                     std::uint64_t) { return true; };
    session_owner->configure_bound_session_operations (std::move (operations));

    serializer_registry_t serializers;
    auto actor_state = std::make_shared<actor_gateway_state_t> ();
    actor_state->serializers = &serializers;
    actor_gateway_runtime_t actor_owner (actor_state);
    stream_runtime_t stream_runtime (std::make_shared<stream_runtime_state_t> ());
    const auto actor_owner_generation = actor_node_generation;
    actor_owner.on_bound_session_send (
      [session_owner, &stream_runtime, actor_node, actor_owner_generation,
       actor_authority_generation, session_owner_lease_generation] (
        const actor_ref_t &source_actor, std::uint64_t expected_binding_generation,
        const stream_header_t &header, const zlink::message_t &payload) -> task_t<result_t<void>> {
          const auto encoded_frame = stream_runtime.encode_frame (header, payload);
          if (!encoded_frame) {
              co_return result_t<void>::failure (encoded_frame.error_kind (),
                                                 encoded_frame.error ()
                                                   ? encoded_frame.error ()->what ()
                                                   : "bound Session frame encoding failed");
          }
          auto application = protocol::application_payload_t::from_parts (
            protocol::application_payload_t::multipart_t{
              zlink::message_t::from (encoded_frame.value ())});
          const protocol::bound_session_send_t send{
            protocol::actor_route_fence_t{std::string (source_actor.actor_id ().value ()),
                                          source_actor.object_generation (), actor_node.to_bytes (),
                                          actor_owner_generation, actor_authority_generation,
                                          session_owner_lease_generation},
            expected_binding_generation};
          mesh::service_mailbox_record_t record{
            "bound-session:" + std::string (source_actor.actor_id ().value ()),
            mesh::service_mailbox_domain_t::application,
            zlink::framework::detail::backend::materialize_binding_parts (
              {protocol::encode_bound_session_send (send),
               protocol::encode_application_payload (application)}),
            actor_node.to_bytes (),
            std::nullopt,
            std::nullopt,
            actor_owner_generation};
          if (!session_owner->transport ().mailbox ().try_enqueue (std::move (record))) {
              co_return result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "Session owner mailbox rejected the bound push");
          }
          const auto noop_dispatch = [] (const host::ready_record_t &,
                                         const host::receive_record_t &,
                                         std::vector<zlink::message_t>) {};
          const auto dispatched = session_owner->dispatch_ready (noop_dispatch).result ();
          if (!dispatched || dispatched.value () != 1) {
              co_return result_t<void>::failure (framework_error_kind_t::unavailable,
                                                 "Session owner did not dispatch the bound push");
          }
          co_return result_t<void>::success ();
      });

    ASSERT_TRUE (actor_owner.record_bound_session_route (
      actor, session_owner->status ().routing_id (),
      zlink::routing_id_t::from (connection.connection_id),
      session_owner->status ().lifecycle_generation (), actor_authority_generation,
      session_owner_lease_generation, binding.binding_generation));

    const auto submitted = actor_owner.actor_context (actor)
                             .bound_session ()
                             .send (std::string ("public-push"))
                             .async ()
                             .result ();
    ASSERT_TRUE (submitted);
    EXPECT_EQ (delivery.wait_for (std::chrono::seconds (2)), std::future_status::ready);
    EXPECT_EQ (deliveries->load (std::memory_order_acquire), 1);
}

TEST (FrameworkRelocationSealContract, failed_publication_keeps_owner_binding_and_projections)
{
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("publication-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto actor_node = zlink::routing_id_t::from ("publication-actor");
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string (actor_node.to_string ()),
                                                 "player", "publication-actor", 7);
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "publication-actor", 7, 11, "mesh", actor_node.to_string ()};
    const auto old_connection = owner->sessions ().open ("old-publication-session");
    const auto [bound, old_binding] =
      owner->sessions ().bind_remote (old_connection, object, 1, 17, {}, 19);
    ASSERT_EQ (bound, stateful::stateful_error_t::none);
    actor_gateway_runtime_t gateway (std::make_shared<actor_gateway_state_t> ());
    ASSERT_TRUE (gateway.record_bound_session_route (
      actor, owner->status ().routing_id (),
      zlink::routing_id_t::from (old_connection.connection_id), 1, 11, 17, 19));
    ASSERT_TRUE (gateway.record_session_relay_source (
      actor, zlink::routing_id_t::from (old_connection.connection_id), 19));
    const auto next_connection = owner->sessions ().open ("next-publication-session");
    int bound_frames = 0;
    const auto publish = [&] (const stateful::stream_binding_t &binding) {
        auto transition = gateway.record_bound_session_route_transition (
          actor,
          actor_bound_session_route_t{owner->status ().routing_id (),
                                      zlink::routing_id_t::from (next_connection.connection_id), 7,
                                      1, 11, 17, binding.binding_generation, 0, 0},
          [&] {
              throw std::runtime_error ("bound queue admission failed");
              ++bound_frames;
          });
        return transition ? stateful::stateful_error_t::none : stateful::stateful_error_t::conflict;
    };
    EXPECT_THROW (owner->sessions ().bind_remote (next_connection, object, 1, 17, publish, 23),
                  std::runtime_error);
    EXPECT_EQ (owner->sessions ().current_binding (object.key), old_binding);
    ASSERT_TRUE (gateway.bound_session_route_async (actor).result ().value ());
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               19u);
    EXPECT_EQ (bound_frames, 0);
    const auto stale = owner->sessions ().bind_remote (
      next_connection, object, 1, 17,
      [&] (const stateful::stream_binding_t &) {
          ++bound_frames;
          return stateful::stateful_error_t::none;
      },
      18);
    EXPECT_EQ (stale.first, stateful::stateful_error_t::conflict);
    EXPECT_EQ (owner->sessions ().current_binding (object.key), old_binding);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               19u);
    EXPECT_EQ (bound_frames, 0);
    const auto publish_current = [&] (const stateful::stream_binding_t &current) {
        auto transition = gateway.record_bound_session_route_transition (
          actor,
          actor_bound_session_route_t{owner->status ().routing_id (),
                                      zlink::routing_id_t::from (current.connection.connection_id),
                                      7, 1, 11, 17, current.binding_generation, 0, 0},
          [&] { ++bound_frames; });
        return transition ? stateful::stateful_error_t::none : stateful::stateful_error_t::conflict;
    };
    ASSERT_TRUE (gateway.record_bound_session_route (
      actor, owner->status ().routing_id (),
      zlink::routing_id_t::from (old_connection.connection_id), 1, 11, 17, 29));
    auto authoritative =
      owner->sessions ().bind_remote (next_connection, object, 1, 17, publish_current, 23);
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::conflict);
    EXPECT_EQ (owner->sessions ().current_binding (object.key), old_binding);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               29u);
    EXPECT_EQ (bound_frames, 0);
    authoritative =
      owner->sessions ().bind_remote (next_connection, object, 1, 17, publish_current, 31);
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::none);
    EXPECT_EQ (owner->sessions ().current_binding (object.key)->binding_generation, 31u);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               31u);
    EXPECT_EQ (bound_frames, 1);
    ASSERT_TRUE (gateway.record_bound_session_route (
      actor, owner->status ().routing_id (),
      zlink::routing_id_t::from (next_connection.connection_id), 1, 11, 17, 45));
    authoritative =
      owner->sessions ().bind_remote (next_connection, object, 1, 17, publish_current, 37);
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::conflict);
    EXPECT_EQ (owner->sessions ().current_binding (object.key)->binding_generation, 31u);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               45u);
    EXPECT_EQ (bound_frames, 1);
    authoritative =
      owner->sessions ().bind_remote (next_connection, object, 1, 17, publish_current, 47);
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::none);
    EXPECT_EQ (owner->sessions ().current_binding (object.key)->binding_generation, 47u);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               47u);
    EXPECT_EQ (bound_frames, 2);
    const auto republished = gateway.record_bound_session_route_transition (
      actor, *gateway.bound_session_route_async (actor).result ().value (),
      [&] { ++bound_frames; });
    ASSERT_TRUE (republished);
    EXPECT_FALSE (republished.value ().changed);
    EXPECT_FALSE (republished.value ().previous);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               47u);
    EXPECT_EQ (bound_frames, 3);
}

TEST (FrameworkRelocationSealContract, relay_drain_keeps_seal_until_native_submissions_are_ordered)
{
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("fifo-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto connection = owner->sessions ().open ("fifo-session");
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "fifo-actor", 7, 11, "mesh", "fifo-target"};
    const auto [error, binding] = owner->sessions ().bind_remote (connection, actor, 1, 17);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    const auto seal = owner->sessions ().seal_remote_route (
      connection.connection_id, binding.binding_generation, actor.key, actor.object_generation,
      binding.owner_lease_generation);
    ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
    auto native_submission = std::make_shared<task_completion_source_t<void>> ();
    // The callbacks are held by `owner`; they own their state.
    struct fifo_state_t
    {
        std::promise<void> first_started;
        std::promise<void> last_submitted;
        std::mutex mutex;
        std::vector<std::uint64_t> order;
    };
    const auto fifo = std::make_shared<fifo_state_t> ();
    auto first = fifo->first_started.get_future ();
    auto last = fifo->last_submitted.get_future ();
    auto &mutex = fifo->mutex;
    auto &order = fifo->order;
    const auto retain = [&] {
        return [owner, native_submission,
                fifo] (std::optional<stateful::stream_dispatch_t> dispatch, message_flow_reason_t,
                       std::optional<result_t<void>>) -> task_t<void> {
            if (!dispatch)
                co_return;
            {
                const std::lock_guard lock (fifo->mutex);
                fifo->order.push_back (dispatch->inbound_sequence);
            }
            if (dispatch->inbound_sequence == 1) {
                fifo->first_started.set_value ();
                co_await native_submission->task ();
            }
            EXPECT_EQ (owner->sessions ().complete_inbound (*dispatch),
                       stateful::stateful_error_t::none);
            if (dispatch->inbound_sequence == 3)
                fifo->last_submitted.set_value ();
            co_return;
        };
    };
    for (std::uint64_t sequence : {1u, 2u}) {
        const auto accepted = owner->sessions ().admit_inbound (
          connection.connection_id, binding.binding_generation, actor.key, sequence,
          std::chrono::milliseconds{0}, retain);
        ASSERT_EQ (accepted.first, stateful::stateful_error_t::none);
        ASSERT_FALSE (accepted.second);
    }
    ASSERT_EQ (owner->sessions ().abort_barrier (seal.barrier), stateful::stateful_error_t::none);
    ASSERT_EQ (first.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE (owner->sessions ().remote_route_sealed (actor.key));
    const auto third =
      owner->sessions ().admit_inbound (connection.connection_id, binding.binding_generation,
                                        actor.key, 3, std::chrono::milliseconds{0}, retain);
    ASSERT_EQ (third.first, stateful::stateful_error_t::none);
    EXPECT_FALSE (third.second);
    native_submission->complete (result_t<void>::success ());
    ASSERT_EQ (last.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    const std::lock_guard lock (mutex);
    EXPECT_EQ (order, (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST (FrameworkRelocationSealContract, terminal_hook_failure_does_not_veto_commit_or_abort_drain)
{
    for (const bool commit : {false, true}) {
        SCOPED_TRACE (commit ? "commit" : "abort");
        auto owner = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("hook-owner"),
            .runtime_failures =
              std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
        const auto connection = owner->sessions ().open ("hook-session");
        const stateful::object_ref_t actor{
          stateful::object_kind_t::actor, "hook-actor", 7, 11, "mesh", "hook-source"};
        const auto [error, binding] = owner->sessions ().bind_remote (connection, actor, 1, 17);
        ASSERT_EQ (error, stateful::stateful_error_t::none);
        const auto seal = owner->sessions ().seal_remote_route (
          connection.connection_id, binding.binding_generation, actor.key, actor.object_generation,
          binding.owner_lease_generation);
        ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
        // The callbacks are held by `owner`; they own the promises.
        const auto delivered = std::make_shared<std::promise<void>> ();
        auto delivery = delivered->get_future ();
        const auto later_delivered = std::make_shared<std::promise<void>> ();
        auto later_delivery = later_delivered->get_future ();
        const auto retain = [&] {
            return [owner, delivered, later_delivered] (
                     std::optional<stateful::stream_dispatch_t> dispatch, message_flow_reason_t,
                     std::optional<result_t<void>>) -> task_t<void> {
                if (dispatch) {
                    EXPECT_EQ (owner->sessions ().complete_inbound (*dispatch),
                               stateful::stateful_error_t::none);
                    if (dispatch->inbound_sequence == 1)
                        delivered->set_value ();
                    else
                        later_delivered->set_value ();
                }
                co_return;
            };
        };
        const auto accepted =
          owner->sessions ().admit_inbound (connection.connection_id, binding.binding_generation,
                                            actor.key, 1, std::chrono::milliseconds{0}, retain);
        ASSERT_EQ (accepted.first, stateful::stateful_error_t::none);
        ASSERT_FALSE (accepted.second);
        const auto throwing_hook = [] (const stateful::stream_route_admission_t &) -> bool {
            throw std::runtime_error ("terminal projection failed");
        };
        if (commit) {
            auto target = actor;
            target.node_id = "hook-target";
            target.authority_owner_generation = 13;
            EXPECT_THROW (owner->sessions ().commit_remote_route (
                            connection.connection_id, binding.binding_generation, actor.key, 7,
                            target, 2, 19, throwing_hook),
                          std::runtime_error);
            EXPECT_EQ (owner->sessions ().current_binding (actor.key)->actor, target);
        } else {
            EXPECT_THROW (owner->sessions ().acknowledge_remote_abort (connection.connection_id,
                                                                       binding.binding_generation,
                                                                       actor.key, 7, throwing_hook),
                          std::runtime_error);
            EXPECT_EQ (owner->sessions ().current_binding (actor.key)->actor, actor);
        }
        EXPECT_EQ (delivery.wait_for (std::chrono::seconds{2}), std::future_status::ready);
        // Empty FIFO release runs in the following registry lane turn.
        const auto later =
          owner->sessions ().admit_inbound (connection.connection_id, binding.binding_generation,
                                            actor.key, 2, std::chrono::milliseconds{0}, retain);
        EXPECT_EQ (later.first, stateful::stateful_error_t::none);
        if (later.second)
            EXPECT_EQ (owner->sessions ().complete_inbound (*later.second),
                       stateful::stateful_error_t::none);
        else
            EXPECT_EQ (later_delivery.wait_for (std::chrono::seconds{2}),
                       std::future_status::ready);
        // Cleanup also breaks the intentional owner retention in a failing red run.
        owner->sessions ().close (connection);
    }
}

TEST (FrameworkRelocationSealContract, drain_failure_only_settles_failed_actor_with_original_reason)
{
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("failed-drain-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto connection = owner->sessions ().open ("failed-drain-session");
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "failed-drain-actor", 7, 11, "mesh", "failed-drain-target"};
    auto other_actor = actor;
    other_actor.key = "other-held-actor";
    const auto [error, binding] = owner->sessions ().bind_remote (connection, actor, 1, 17);
    const auto [other_error, other] =
      owner->sessions ().bind_remote (connection, other_actor, 1, 17);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    ASSERT_EQ (other_error, stateful::stateful_error_t::none);
    const auto seal =
      owner->sessions ().seal_remote_route (connection.connection_id, binding.binding_generation,
                                            actor.key, 7, binding.owner_lease_generation);
    const auto other_seal =
      owner->sessions ().seal_remote_route (connection.connection_id, other.binding_generation,
                                            other_actor.key, 7, other.owner_lease_generation);
    auto failure = std::make_shared<task_completion_source_t<void>> ();
    // The callbacks are held by `owner`; they own the promises.
    const auto settled = std::make_shared<std::promise<message_flow_reason_t>> ();
    auto settlement = settled->get_future ();
    const auto other_delivered = std::make_shared<std::promise<bool>> ();
    auto other_delivery = other_delivered->get_future ();
    const auto retain = [&] {
        return
          [owner, failure, settled] (std::optional<stateful::stream_dispatch_t> dispatch,
                                     message_flow_reason_t reason, auto... errors) -> task_t<void> {
              if constexpr (sizeof...(errors) != 0) {
                  if (std::get<0> (std::tuple{errors...}))
                      co_return;
              }
              if (!dispatch) {
                  settled->set_value (reason);
                  co_return;
              }
              // Production relay's ingress guard completes this same active record.
              EXPECT_EQ (owner->sessions ().complete_inbound (*dispatch),
                         stateful::stateful_error_t::none);
              co_await failure->task ();
          };
    };
    for (std::uint64_t sequence : {1u, 2u})
        ASSERT_EQ (owner->sessions ()
                     .admit_inbound (connection.connection_id, binding.binding_generation,
                                     actor.key, sequence, std::chrono::milliseconds{0}, retain)
                     .first,
                   stateful::stateful_error_t::none);
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (connection.connection_id, other.binding_generation,
                                 other_actor.key, 1, std::chrono::milliseconds{0},
                                 [&] {
                                     return [owner, other_delivered] (
                                              std::optional<stateful::stream_dispatch_t> dispatch,
                                              message_flow_reason_t, auto...) -> task_t<void> {
                                         if (dispatch)
                                             (void) owner->sessions ().complete_inbound (*dispatch);
                                         other_delivered->set_value (dispatch.has_value ());
                                         co_return;
                                     };
                                 })
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (owner->sessions ().abort_barrier (seal.barrier), stateful::stateful_error_t::none);
    failure->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "relay route unavailable"));
    ASSERT_EQ (settlement.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ (settlement.get (), message_flow_reason_t::stale_target);
    EXPECT_FALSE (owner->sessions ().remote_route_sealed (actor.key));
    EXPECT_EQ (other_delivery.wait_for (std::chrono::milliseconds{50}),
               std::future_status::timeout);
    EXPECT_TRUE (owner->sessions ().remote_route_sealed (other_actor.key));
    ASSERT_EQ (owner->sessions ().abort_barrier (other_seal.barrier),
               stateful::stateful_error_t::none);
    ASSERT_EQ (other_delivery.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE (other_delivery.get ());
}

TEST (FrameworkRelocationSealContract,
      settlement_callback_failure_reports_once_without_second_settlement)
{
    // The settlement callback is held by `owner` and the error observer by `state->options`; each
    // owns its state. The observer state is separate so state and options do not form a cycle.
    struct report_state_t
    {
        std::vector<message_dispatch_error_event_t> errors;
        std::promise<void> reported;
    };
    struct settlement_state_t
    {
        dispatch_options_t options;
        int settlements = 0;
    };
    const auto reports = std::make_shared<report_state_t> ();
    const auto state = std::make_shared<settlement_state_t> ();
    state->options.message_flow (message_flow_log_mode_t::normal);
    auto &errors = reports->errors;
    auto report = reports->reported.get_future ();
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      state->options, [reports] (const auto &event) {
          reports->errors.push_back (event);
          reports->reported.set_value ();
      });
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("settlement-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto connection = owner->sessions ().open ("settlement-session");
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "settlement-actor", 7, 11, "mesh", "settlement-target"};
    const auto [error, binding] = owner->sessions ().bind_remote (connection, actor, 1, 17);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    const auto seal =
      owner->sessions ().seal_remote_route (connection.connection_id, binding.binding_generation,
                                            actor.key, 7, binding.owner_lease_generation);
    ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    const auto retain = [&] {
        return [owner, completion, state] (std::optional<stateful::stream_dispatch_t> dispatch,
                                           message_flow_reason_t reason,
                                           auto... failures) -> task_t<void> {
            if constexpr (sizeof...(failures) != 0) {
                if (auto failed = std::get<0> (std::tuple{failures...})) {
                    dispatch_error_reporter_t (state->options).report_lazy ([&] {
                        message_dispatch_error_event_t event{};
                        event.surface = dispatch_error_surface_t::stream_session;
                        event.reason = dispatch_reason_from_error (failed->error ());
                        event.exception = std::make_exception_ptr (*failed->error ());
                        return event;
                    });
                    co_return;
                }
            }
            EXPECT_FALSE (dispatch);
            EXPECT_EQ (reason, message_flow_reason_t::target_closed);
            ++state->settlements;
            co_await completion->task ();
        };
    };
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (connection.connection_id, binding.binding_generation, actor.key, 1,
                                 std::chrono::milliseconds{0}, retain)
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_TRUE (owner->sessions ().close (connection));
    // Existing observer throws here when no scheduler is installed; some schedulers swallow it.
    try {
        completion->complete (result_t<void>::failure (framework_error_kind_t::unavailable,
                                                       "settlement delivery failed"));
    }
    catch (const framework_exception_t &) {
    }
    EXPECT_EQ (state->settlements, 1);
    ASSERT_EQ (report.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    ASSERT_EQ (errors.size (), 1u);
    EXPECT_EQ (errors.front ().error_message, "settlement delivery failed");
}

TEST (FrameworkRelocationSealContract, old_drain_failure_preserves_new_binding_seal)
{
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("rebound-drain-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "rebound-drain-actor", 7, 11, "mesh", "rebound-drain-target"};
    const auto first_connection = owner->sessions ().open ("first-drain-session");
    const auto [error, first_binding] =
      owner->sessions ().bind_remote (first_connection, actor, 1, 17);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    const auto first_seal = owner->sessions ().seal_remote_route (
      first_connection.connection_id, first_binding.binding_generation, actor.key, 7,
      first_binding.owner_lease_generation);
    auto failure = std::make_shared<task_completion_source_t<void>> ();
    // The callbacks are held by `owner`; they own the promises.
    const auto started = std::make_shared<std::promise<void>> ();
    const auto reported = std::make_shared<std::promise<void>> ();
    auto start = started->get_future ();
    auto report = reported->get_future ();
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (first_connection.connection_id, first_binding.binding_generation,
                                 actor.key, 1, std::chrono::milliseconds{0},
                                 [&] {
                                     return
                                       [owner, failure, started, reported] (
                                         std::optional<stateful::stream_dispatch_t> dispatch,
                                         message_flow_reason_t,
                                         std::optional<result_t<void>> failed) -> task_t<void> {
                                           if (failed) {
                                               reported->set_value ();
                                               co_return;
                                           }
                                           EXPECT_TRUE (dispatch);
                                           if (!dispatch)
                                               co_return;
                                           (void) owner->sessions ().complete_inbound (*dispatch);
                                           started->set_value ();
                                           co_await failure->task ();
                                       };
                                 })
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (owner->sessions ().abort_barrier (first_seal.barrier),
               stateful::stateful_error_t::none);
    ASSERT_EQ (start.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    const auto next_connection = owner->sessions ().open ("next-drain-session");
    const auto [next_error, next_binding] =
      owner->sessions ().bind_remote (next_connection, actor, 1, 17);
    ASSERT_EQ (next_error, stateful::stateful_error_t::none);
    const auto next_seal = owner->sessions ().seal_remote_route (
      next_connection.connection_id, next_binding.binding_generation, actor.key, 7,
      next_binding.owner_lease_generation);
    ASSERT_EQ (next_seal.error, stateful::stateful_error_t::none);
    const auto delivered = std::make_shared<std::promise<bool>> ();
    auto delivery = delivered->get_future ();
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (next_connection.connection_id, next_binding.binding_generation,
                                 actor.key, 1, std::chrono::milliseconds{0},
                                 [&] {
                                     return [owner, delivered] (
                                              std::optional<stateful::stream_dispatch_t> dispatch,
                                              message_flow_reason_t,
                                              std::optional<result_t<void>>) -> task_t<void> {
                                         if (dispatch)
                                             (void) owner->sessions ().complete_inbound (*dispatch);
                                         delivered->set_value (dispatch.has_value ());
                                         co_return;
                                     };
                                 })
                 .first,
               stateful::stateful_error_t::none);
    failure->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "old drain failed"));
    ASSERT_EQ (report.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ (delivery.wait_for (std::chrono::milliseconds{50}), std::future_status::timeout);
    EXPECT_TRUE (owner->sessions ().remote_route_sealed (actor.key));
    ASSERT_EQ (owner->sessions ().abort_barrier (next_seal.barrier),
               stateful::stateful_error_t::none);
    ASSERT_EQ (delivery.wait_for (std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE (delivery.get ());
}

TEST (FrameworkRelocationSealContract, restored_actor_owns_active_capacity_until_destroy)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "target",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    objects.configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) { return true; });
    const stateful::object_ref_t source{
      stateful::object_kind_t::actor, "restored", 1, 1, "mesh", "source"};
    auto target = source;
    target.node_id = "target";
    ++target.authority_owner_generation;
    const stateful::relocation_restore_identity_t identity{"restore-capacity", 1, {}};
    ASSERT_EQ (objects.restore_relocation ({source, "actor"}, target, identity).result ().value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_relocation_restore (target, identity),
               stateful::stateful_error_t::none);
    EXPECT_EQ (objects.commit_relocation_restore (target, identity),
               stateful::stateful_error_t::already_exists);
    EXPECT_EQ (objects.destroy_actor (target), stateful::stateful_error_t::none);
    const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                             .key = "next",
                                             .stable_type = "actor",
                                             .mesh_name = std::string ("mesh"),
                                             .exclusive = true});
    ASSERT_EQ (next.status, stateful::create_status_t::reserved);
    EXPECT_EQ (objects.commit_create (next.attempt), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.destroy_actor (next.object), stateful::stateful_error_t::none);
}

TEST (FrameworkRelocationSealContract, restored_aggregate_releases_each_active_object_once)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "target",
                                            .stable_types = {"actor", "spot"},
                                            .active_capacity = 2,
                                            .pending_capacity = 2}});
    objects.configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) { return true; });
    const std::vector<stateful::frozen_object_state_t> frozen{
      {{stateful::object_kind_t::user_spot, "room", 1, 1, "mesh", "source"}, "spot"},
      {{stateful::object_kind_t::actor, "member", 1, 1, "mesh", "source"}, "actor"}};
    std::vector<stateful::object_ref_t> targets;
    for (const auto &entry : frozen) {
        auto target = entry.owner;
        target.node_id = "target";
        ++target.authority_owner_generation;
        targets.push_back (target);
    }
    const stateful::relocation_restore_identity_t identity{"aggregate-capacity", 1, {}};
    ASSERT_EQ (objects.restore_relocation_aggregate (frozen, targets, identity).result ().value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_relocation_restore_aggregate (targets, identity),
               stateful::stateful_error_t::none);
    EXPECT_EQ (objects.destroy_actor (targets[1]), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.close_spot (targets[0]).first, stateful::stateful_error_t::none);
    for (int index = 0; index != 2; ++index) {
        const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                 .key = "next-" + std::to_string (index),
                                                 .stable_type = "actor",
                                                 .mesh_name = std::string ("mesh"),
                                                 .exclusive = true});
        ASSERT_EQ (next.status, stateful::create_status_t::reserved);
        EXPECT_EQ (objects.commit_create (next.attempt), stateful::stateful_error_t::none);
    }
}

TEST (FrameworkRelocationSealContract, finalized_source_releases_capacity_and_retains_follow)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                .key = "moving",
                                                .stable_type = "actor",
                                                .mesh_name = std::string ("mesh"),
                                                .exclusive = true});
    ASSERT_EQ (created.status, stateful::create_status_t::reserved);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({created.object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    int followed = 0;
    ASSERT_EQ (objects
                 .begin_relocation_boundary (sealed.seal.token,
                                             [&] (const auto &, const auto &) { ++followed; })
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                             .key = "next",
                                             .stable_type = "actor",
                                             .mesh_name = std::string ("mesh"),
                                             .exclusive = true});
    EXPECT_EQ (next.status, stateful::create_status_t::reserved);
    EXPECT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
               stateful::stateful_error_t::none);
    EXPECT_EQ (followed, 1);
    EXPECT_TRUE (objects.finalized_relocation_source (created.object.kind, created.object.key,
                                                      created.object.object_generation));
}

TEST (FrameworkRelocationSealContract, session_seal_ack_precedes_source_dispatch_stop)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "node-a",
                                            .stable_types = {"actor", "spot"},
                                            .weight = 100,
                                            .active_capacity = 100,
                                            .pending_capacity = 100}});
    std::vector<stateful::object_ref_t> participants;
    for (const auto kind : {stateful::object_kind_t::actor, stateful::object_kind_t::user_spot}) {
        const bool actor = kind == stateful::object_kind_t::actor;
        const auto created = objects.begin_create ({.kind = kind,
                                                    .key = actor ? "seal-actor" : "seal-spot",
                                                    .stable_type = actor ? "actor" : "spot",
                                                    .mesh_name = std::string ("mesh"),
                                                    .exclusive = true});
        ASSERT_EQ (created.status, stateful::create_status_t::reserved);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
        participants.push_back (created.object);
        ASSERT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
                   stateful::stateful_error_t::none);
    }

    std::promise<void> seal_requested;
    auto requested = seal_requested.get_future ();
    std::promise<void> acknowledge_session_seal;
    auto acknowledgement = acknowledge_session_seal.get_future ();
    stateful::aggregate_relocation_seal_attempt_t result;
    std::thread source ([&] {
        result = objects
                   .try_seal_relocation_aggregate (participants, {},
                                                   [&] () -> task_t<bool> {
                                                       seal_requested.set_value ();
                                                       acknowledgement.get ();
                                                       co_return result_t<bool>::success (true);
                                                   })
                   .result ()
                   .value ();
    });
    requested.get ();
    // Command 43 has not acknowledged the Session seal. Both source lanes
    // must still execute work accepted before the relocation request.
    for (const auto &participant : participants) {
        const auto [error, claim] =
          objects.try_claim (participant, stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::none);
        EXPECT_TRUE (claim);
        if (claim) {
            EXPECT_EQ (claim->sequence, 1u);
            EXPECT_EQ (objects.complete_claim (participant, stateful::turn_domain_t::application),
                       stateful::stateful_error_t::none);
        }
        EXPECT_EQ (objects.enqueue (participant, stateful::turn_domain_t::application, {2, {2}}),
                   stateful::stateful_error_t::none);
    }
    acknowledge_session_seal.set_value ();
    source.join ();

    ASSERT_EQ (result.error, stateful::stateful_error_t::none);
    ASSERT_EQ (result.seal.participants.size (), participants.size ());
    for (std::size_t index = 0; index != participants.size (); ++index) {
        const auto &pending = result.seal.participants[index].pending_application;
        ASSERT_EQ (pending.size (), 1u);
        EXPECT_EQ (pending.front ().sequence, 2u);
        const auto [error, claim] =
          objects.try_claim (participants[index], stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::moving);
        EXPECT_FALSE (claim);
    }
    EXPECT_EQ (objects.abort_relocation_before_cutover (result.seal.token).result ().value (),
               stateful::stateful_error_t::none);

    const auto refused = objects
                           .try_seal_relocation_aggregate (
                             participants, {}, [] () -> task_t<bool> { co_return false; })
                           .result ()
                           .value ();
    EXPECT_EQ (refused.error, stateful::stateful_error_t::conflict);
    for (const auto &participant : participants) {
        const auto [error, claim] =
          objects.try_claim (participant, stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::none);
        ASSERT_TRUE (claim);
        EXPECT_EQ (claim->sequence, 2u);
        EXPECT_EQ (objects.complete_claim (participant, stateful::turn_domain_t::application),
                   stateful::stateful_error_t::none);
    }

    // The Session request can suspend after submission. Its participant set
    // must remain the submitted set even if the caller releases its input.
    const auto submitted = participants;
    task_completion_source_t<void> session_ack;
    const std::function<task_t<bool> ()> seal_sessions = [&] () -> task_t<bool> {
        co_await session_ack.task ();
        co_return true;
    };
    auto pending = objects.try_seal_relocation_aggregate (participants, {}, seal_sessions);
    EXPECT_FALSE (pending.await_ready ());
    participants.clear ();
    (void) session_ack.complete (result_t<void>::success ());
    const auto captured = pending.result ().value ();
    EXPECT_EQ (captured.error, stateful::stateful_error_t::none);
    EXPECT_EQ (captured.seal.participants.size (), submitted.size ());
    for (const auto &participant : submitted) {
        const auto [error, claim] =
          objects.try_claim (participant, stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::moving);
        EXPECT_FALSE (claim);
    }
    EXPECT_EQ (objects.abort_relocation_before_cutover (captured.seal.token).result ().value (),
               stateful::stateful_error_t::none);
}

TEST (FrameworkRelocationSealContract, sealed_scheduled_timer_keeps_pending_count_without_failure)
{
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "sealed-timer-node");
    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    serializer_registry_t serializers;
    context->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    context->channel_runtime->serializers = &serializers;
    context->spot_instance = std::make_shared<int> (1);
    auto timer = std::make_shared<timer_state_t> ();
    timer->name = "pending-sealed-tick";
    timer->period = std::chrono::milliseconds (1);
    auto handler = std::make_shared<int> (1);
    timer->handler_instance = handler;
    std::vector<timer_tick_t> delivered;
    timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                  const timer_tick_t &tick) -> task_t<zlink::message_t> {
        delivered.push_back (tick);
        co_return zlink::message_t{};
    };
    context->node->lane.run ([&] { context->admission_sealed = true; }).get ();
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 1);
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 1);
    context->drain_serial ();
    EXPECT_TRUE (delivered.empty ());
    EXPECT_EQ (timer->pending_fire_count, 2u);
    EXPECT_EQ (timer->delivery_index, 0u);
    EXPECT_TRUE (timer->failure_events.empty ());

    context->node->lane.run ([&] { context->admission_sealed = false; }).get ();
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 0);
    context->drain_serial ();
    ASSERT_EQ (delivered.size (), 1u);
    EXPECT_EQ (delivered.front ().delivery_index, 1u);
    EXPECT_EQ (delivered.front ().scheduled_index, 2u);
    EXPECT_EQ (delivered.front ().skipped_ticks, 1u);
    EXPECT_EQ (timer->pending_fire_count, 0u);
    EXPECT_TRUE (timer->failure_events.empty ());
}

TEST (FrameworkRelocationSealContract, source_fenced_relay_survives_target_authority_commit)
{
    for (const auto kind : {stateful::object_kind_t::actor, stateful::object_kind_t::user_spot}) {
        auto target_host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("relay-target"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        target_host->configure_stateful_dispatch (
          [] (const stateful::accepted_record_authority_query_t &)
            -> task_t<std::optional<stateful::accepted_record_authority_t>> {
              co_return std::nullopt;
          });
        const auto source_rid = zlink::routing_id_t::from ("relay-source").to_bytes ();
        const auto target_rid = zlink::routing_id_t::from ("relay-target").to_bytes ();
        const stateful::object_ref_t source{kind, "relay-object",         3,
                                            4,    "relocation-seal-test", "relay-source"};
        auto target = source;
        target.node_id = "relay-target";
        protocol::relocation_prepare_t prepare{};
        prepare.source_node_routing_id = source_rid;
        prepare.source_node_generation = 1;
        prepare.target = {target_rid, 1, "target-owner", 5};
        prepare.coordinator = {"source-owner", 7, source_rid, 1, "captured-version"};
        protocol::actor_message_header_t actor_header;
        actor_header.target = {source.key, 3, target_rid, 1, 4, 5};
        protocol::spot_message_header_t spot_header;
        spot_header.target = {source.key, 3, target_rid, 1, 4, 5};
        std::variant<protocol::actor_message_header_t, protocol::spot_message_header_t> header =
          actor_header;
        if (kind != stateful::object_kind_t::actor)
            header = spot_header;
        const auto address = [&] (std::uint64_t authority, std::uint64_t lease) {
            std::visit (
              [&] (auto &value) {
                  value.target.authority_owner_generation = authority;
                  value.target.owner_lease_generation = lease;
              },
              header);
        };
        const auto accept = [&] (const auto &rid) {
            return std::visit (
              [&] (const auto &value) {
                  return host::public_host_runtime_test_access_t::accept_relay (*target_host,
                                                                                target, value, rid)
                    .first;
              },
              header);
        };
        host::public_host_runtime_test_access_t::stage_relay (*target_host, prepare, source,
                                                              target);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        // The Store assigns an unrelated larger generation; the captured source route is unchanged.
        target.authority_owner_generation = 19;
        host::public_host_runtime_test_access_t::stage_relay (*target_host, prepare, source,
                                                              target);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        EXPECT_NE (accept (zlink::routing_id_t::from ("unrelated-source").to_bytes ()),
                   stateful::stateful_error_t::none);
        address (8, 5);
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        address (19, 5);
        EXPECT_EQ (accept (source_rid), stateful::stateful_error_t::none);
        address (19, 99);
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        address (4, 5);
        target.authority_owner_generation = 23;
        EXPECT_NE (accept (source_rid), stateful::stateful_error_t::none);
        target_host->close ();
    }
}

TEST (FrameworkRelocationSealContract, native_follow_increments_wire_hop_and_stops_at_eight)
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("hop-source"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("hop-target"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_node = source.topology ().local_descriptor ();
    const auto target_node = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_node));
    const auto ready = [&] {
        const auto one = source.topology ().peer (target_node.node_routing_id);
        const auto two = target.topology ().peer (source_node.node_routing_id);
        return one && one->ready && two && two->ready;
    };
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!ready () && std::chrono::steady_clock::now () < deadline) {
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) source.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    ASSERT_TRUE (ready ());
    stateful::stateful_object_runtime_t objects;
    stateful::raw_stateful_dispatch_t dispatch (objects, source);
    const protocol::application_payload_t payload{
      "hop-test", "application/octet-stream", {1, 2, 3}};
    std::uint64_t sequence = 1;
    for (const auto kind :
         {protocol::frozen_record_kind_t::actor_send, protocol::frozen_record_kind_t::spot_send,
          protocol::frozen_record_kind_t::actor_request,
          protocol::frozen_record_kind_t::spot_request}) {
        SCOPED_TRACE (static_cast<int> (kind));
        const bool actor = kind == protocol::frozen_record_kind_t::actor_send
                           || kind == protocol::frozen_record_kind_t::actor_request;
        const bool request = kind == protocol::frozen_record_kind_t::actor_request
                             || kind == protocol::frozen_record_kind_t::spot_request;
        const stateful::object_ref_t owner{actor ? stateful::object_kind_t::actor
                                                 : stateful::object_kind_t::user_spot,
                                           "moving",
                                           7,
                                           1,
                                           "mesh",
                                           "source"};
        protocol::frozen_application_record_t record;
        record.kind = kind;
        record.source_kind =
          actor ? protocol::frozen_source_kind_t::node : protocol::frozen_source_kind_t::spot;
        if (!actor)
            record.source_spot_id = "producer";
        record.source = {"producer", 3, source_node.node_routing_id,
                         source_node.lifecycle_generation};
        record.operation = {33, sequence++};
        if (request) {
            record.operation_kind = actor ? 4 : 3;
            record.reply_route_id = 900 + sequence;
        }
        record.message_follow_hop_count = 7;
        if (actor)
            record.body = protocol::frozen_actor_application_body_t{
              {owner.key, 7, source_node.node_routing_id, source_node.lifecycle_generation, 1, 3},
              payload};
        else
            record.body = protocol::frozen_spot_application_body_t{
              {owner.key, 7, source_node.node_routing_id, source_node.lifecycle_generation, 1, 3},
              3,
              payload};
        stateful::turn_record_t turn;
        turn.application_record = record;
        ASSERT_TRUE (dispatch
                       .forward_accepted (owner, turn, target_node.node_routing_id,
                                          target_node.lifecycle_generation, 3,
                                          std::chrono::seconds (1), 2)
                       .result ()
                       .value ());
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        const auto receive_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
            (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
            claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 65536);
            std::this_thread::yield ();
        }
        ASSERT_TRUE (claim);
        ASSERT_EQ (claim->records.size (), 1u);
        const auto &received = (*claim->records.front ());
        const auto bytes = protocol::message_bytes (received.parts.front ());
        if (actor) {
            const auto header = protocol::decode_actor_message_header (
              bytes, request ? protocol::command::actorRequest : protocol::command::actorSend);
            EXPECT_EQ (header.message_follow_hop_count, 8);
            EXPECT_EQ (header.operation, record.operation);
            EXPECT_EQ (header.target.object_generation, 7u);
        } else {
            const auto header = protocol::decode_spot_message_header (
              bytes, request ? protocol::command::spotRequest : protocol::command::spotSend);
            EXPECT_EQ (header.message_follow_hop_count, 8);
            EXPECT_EQ (header.operation, record.operation);
            EXPECT_EQ (header.target.object_generation, 7u);
        }
        EXPECT_EQ (
          protocol::decode_application_payload (protocol::message_bytes (received.parts[1])),
          payload);
        if (request)
            ASSERT_TRUE (target.reply (received, payload));
        target.mailbox ().release (*claim);
        turn.application_record->message_follow_hop_count = 8;
        const auto extra =
          dispatch
            .forward_accepted (owner, turn, target_node.node_routing_id,
                               target_node.lifecycle_generation, 3, std::chrono::seconds (1), 2)
            .result ()
            .value ();
        EXPECT_FALSE (extra);
        // Drain an unexpected send so each regression row observes its own packet.
        if (extra) {
            std::shared_ptr<mesh::service_mailbox_claim_t> unexpected;
            const auto drain_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (5);
            while (!unexpected && std::chrono::steady_clock::now () < drain_deadline) {
                (void) target.pump_one (std::chrono::steady_clock::now ()).result ().value ();
                unexpected = target.mailbox ().try_claim (
                  mesh::service_mailbox_domain_t::application, 1, 65536);
            }
            ASSERT_TRUE (unexpected);
            if (request)
                ASSERT_TRUE (target.reply ((*unexpected->records.front ()), payload));
            target.mailbox ().release (*unexpected);
        }
    }
    source.close ();
    target.close ();
}

TEST (FrameworkRelocationSealContract, exhausted_follow_request_settles_original_reply_once)
{
    mesh::raw_mesh_node_owner_t transport (mesh::raw_mesh_node_options_t{
      .descriptor = test_descriptor ("exhausted-follow"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (owner, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (objects, transport);
    protocol::actor_message_header_t header;
    header.target = {owner.key, 7, {1}, 1, 1, 3};
    header.operation = {31, 41};
    header.correlation = 901;
    header.message_follow_hop_count = 8;
    auto accepted = std::make_shared<runtime::serial_application_record_t> ();
    accepted->payload ().emplace ();
    accepted->payload ()->application_record = stateful::accepted_actor_record (
      header, {"producer", 3, {1}, 1}, {"payload", "application/json", {1}}, true);
    int terminals = 0;
    std::optional<mesh::service_mailbox_record_t> taken_original;
    ASSERT_EQ (dispatch
                 .accept_local (owner, accepted, mesh::service_mailbox_record_t{}, taken_original,
                                [&] (const auto &reply) {
                                    ++terminals;
                                    EXPECT_FALSE (reply);
                                    return true;
                                })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({owner}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    ASSERT_EQ (sealed.seal.participants.front ().pending_application.size (), 1u);
    const auto turn = sealed.seal.participants.front ().pending_application.front ();
    const auto forwarded =
      dispatch.forward_accepted (owner, turn, {2}, 1, 4, std::chrono::seconds (1), 2).result ();
    ASSERT_TRUE (forwarded);
    EXPECT_TRUE (forwarded.value ());
    EXPECT_EQ (terminals, 1);
    EXPECT_FALSE (dispatch.forward_accepted (owner, turn, {2}, 1, 4, std::chrono::seconds (1), 2)
                    .result ()
                    .value ());
    EXPECT_EQ (terminals, 1);
}

TEST (FrameworkRelocationSealContract, accepted_follow_preserves_actor_and_spot_hop)
{
    protocol::actor_message_header_t actor;
    actor.message_follow_hop_count = 7;
    actor.target = {"actor", 7, {1}, 1, 2, 3};
    actor.operation = {1, 2};
    protocol::spot_message_header_t spot;
    spot.message_follow_hop_count = 8;
    spot.target = {"spot", 7, {1}, 1, 2, 3};
    spot.operation = {1, 3};
    spot.source_spot_id = "producer";
    const protocol::request_source_fence_t source{"owner", 3, {1}, 1};
    const protocol::application_payload_t payload{"hop-test", "application/json", {1}};
    const auto actor_record = stateful::accepted_actor_record (actor, source, payload, false);
    const auto spot_record = stateful::accepted_spot_record (spot, source, payload, false);
    EXPECT_EQ (actor_record.message_follow_hop_count, 7);
    EXPECT_EQ (spot_record.message_follow_hop_count, 8);
    for (const auto &record : {actor_record, spot_record}) {
        const auto frozen = protocol::encode_frozen_application_record (record);
        const auto decoded = protocol::decode_frozen_record (frozen.canonical_bytes);
        EXPECT_EQ (decoded.message_follow_hop_count,
                   protocol::summarize_frozen_application_record (record).message_follow_hop_count);
        EXPECT_EQ (decoded.operation, record.operation);
        auto oversized = record;
        oversized.metadata.push_back ({"oversized", std::string (protocol::metadataBytes, 'x')});
        EXPECT_THROW (protocol::encode_frozen_application_record (oversized),
                      protocol::service_wire_error_t);
        auto invalid = record;
        invalid.message_follow_hop_count = 9;
        EXPECT_THROW (protocol::encode_frozen_application_record (invalid),
                      protocol::service_wire_error_t);
    }
    actor.message_follow_hop_count = 0;
    EXPECT_TRUE (stateful::accepted_actor_record (actor, {}, {}, false).metadata.empty ());
}

TEST (FrameworkRelocationSealContract, retained_follow_rejects_previous_object_generation)
{
    auto descriptor = test_descriptor ("follow-generation");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (), [&] (const auto &query) {
          return host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    protocol::actor_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, recorded_source, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, recorded_source, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::generation_stale);
    host->close ();
}

TEST (FrameworkRelocationSealContract, retained_spot_rejects_previous_object_generation)
{
    auto descriptor = test_descriptor ("follow-generation");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::user_spot, "moving", 7, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (), [&] (const auto &query) {
          return host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    protocol::spot_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, recorded_source,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, recorded_source,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::generation_stale);
    host->close ();
}

// Location §7.3 and relocation §10 require native Follow without a fresh Store read.
// A raw ingress or claim waits for the object state lane without holding the raw dispatch
// mutex, so pending-entry work for another owner continues meanwhile.
// Relocation flow §4.2 producer cut at the mailbox: a claimed original not taken yet leaves with
// its claim credit and an unclaimed one leaves the FIFO; a late take finds nothing, a refused
// original leaves the owner FIFO untouched, and a record enqueued afterwards waits for a claim.
TEST (FrameworkRelocationSealContract, mailbox_cut_takes_each_untaken_original_once)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    mesh::service_mailbox_t mailbox;
    const auto record = [domain] (std::uint8_t id) {
        mesh::service_mailbox_record_t value;
        value.owner = "spot:cut";
        value.domain = domain;
        value.parts = detail::backend::materialize_binding_parts ({{id}});
        return value;
    };
    const auto id_of = [] (const mesh::service_mailbox_record_t &value) {
        return value.parts.front ().to_bytes ().front ();
    };
    for (const std::uint8_t id : {1, 2, 3})
        ASSERT_TRUE (mailbox.try_enqueue (record (id)));
    const auto claim = mailbox.try_claim_owner (domain, "spot:cut", 2, 4096);
    ASSERT_TRUE (claim);
    ASSERT_EQ (claim->claimed_messages, 2u);
    auto first = mailbox.take ({claim, 0});
    ASSERT_TRUE (first);
    bool cut = false;
    EXPECT_FALSE (mailbox.cut_owners (
      {"spot:cut"}, [&] (const auto &value) { return id_of (value) != 3; }, [&] { cut = true; }));
    EXPECT_FALSE (cut);
    EXPECT_EQ (mailbox.pending_messages (domain), 1u);
    auto taken = mailbox.cut_owners ({"spot:cut", "spot:other"}, {}, [&] { cut = true; });
    ASSERT_TRUE (taken);
    EXPECT_TRUE (cut);
    ASSERT_EQ (taken->size (), 2u);
    ASSERT_EQ ((*taken)[0].size (), 2u);
    EXPECT_TRUE ((*taken)[1].empty ());
    EXPECT_EQ (id_of ((*taken)[0][0]), 2);
    EXPECT_EQ (id_of ((*taken)[0][1]), 3);
    EXPECT_TRUE ((*taken)[0][0].transferred);
    EXPECT_FALSE ((*taken)[0][1].transferred);
    EXPECT_FALSE (mailbox.take ({claim, 1}));
    EXPECT_EQ (mailbox.pending_messages (domain), 0u);
    ASSERT_TRUE (mailbox.try_enqueue (record (4)));
    EXPECT_FALSE (mailbox.try_claim_owner (domain, "spot:cut", 8, 4096));
    EXPECT_TRUE (first->transferred ());
    EXPECT_TRUE ((*taken)[0][0].transferred ());
    EXPECT_FALSE ((*taken)[0][0].transferred ());
    EXPECT_EQ (claim->outstanding (), 0u);
    const auto next = mailbox.try_claim_owner (domain, "spot:cut", 8, 4096);
    ASSERT_TRUE (next);
    ASSERT_EQ (next->claimed_messages, 1u);
    EXPECT_EQ (id_of (*next->records.front ()), 4);
}

TEST (FrameworkRelocationSealContract, ordinary_claim_transfers_batch_before_cut)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    mesh::service_mailbox_t mailbox;
    std::vector<mesh::service_mailbox_record_t> batch;
    batch.reserve (64);
    for (std::uint8_t index = 0; index < 64; ++index) {
        mesh::service_mailbox_record_t original;
        original.owner = "ordinary";
        original.domain = domain;
        original.parts = detail::backend::materialize_binding_parts ({{index}});
        ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    }
    const auto claim = mailbox.try_claim (domain, 64, 65536, [&] (const auto &claimed) noexcept {
        for (auto *original : claimed->records)
            batch.push_back (std::move (*original));
    });
    ASSERT_TRUE (claim);
    ASSERT_EQ (batch.size (), 64u);
    const auto cut = mailbox.cut_owners ({"ordinary"}, {}, {});
    ASSERT_TRUE (cut);
    EXPECT_TRUE (cut->front ().empty ());
    for (std::size_t index = 0; index < batch.size (); ++index) {
        EXPECT_EQ (batch[index].parts.front ().to_bytes ().front (), index);
        EXPECT_FALSE (mailbox.take ({claim, index}));
        EXPECT_TRUE (batch[index].transferred ());
        EXPECT_FALSE (batch[index].transferred ());
    }
    EXPECT_EQ (claim->outstanding (), 0u);
    EXPECT_FALSE (mailbox.release (*claim));
}

TEST (FrameworkRelocationSealContract, throwing_claim_consumer_returns_all_credits)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    for (const bool by_owner : {false, true}) {
        mesh::service_mailbox_t mailbox;
        const auto enqueue = [&] {
            mesh::service_mailbox_record_t original;
            original.owner = "ordinary";
            original.domain = domain;
            original.parts = detail::backend::materialize_binding_parts ({{1}});
            ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
        };
        enqueue ();
        enqueue ();
        std::optional<mesh::service_mailbox_record_t> transferred;
        const mesh::service_mailbox_t::claimed_t fail = [&] (const auto &claim) {
            transferred.emplace (std::move (*claim->records.front ()));
            throw std::bad_alloc ();
        };
        if (by_owner)
            EXPECT_THROW (mailbox.try_claim_owner (domain, "ordinary", 2, 4096, fail),
                          std::bad_alloc);
        else
            EXPECT_THROW (mailbox.try_claim (domain, 2, 4096, fail), std::bad_alloc);
        ASSERT_TRUE (transferred);
        EXPECT_FALSE (transferred->transferred ());
        EXPECT_EQ (mailbox.pending_messages (domain), 0u);
        enqueue ();
        const auto next = mailbox.try_claim (domain, 1, 4096);
        ASSERT_TRUE (next);
        EXPECT_TRUE (mailbox.release (*next));
    }
}

TEST (FrameworkRelocationSealContract, ordinary_dispatch_failure_returns_transferred_suffix)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("ordinary-batch"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &mailbox = host->transport ().mailbox ();
    const auto enqueue = [&] {
        mesh::service_mailbox_record_t original;
        original.owner = "node";
        original.domain = mesh::service_mailbox_domain_t::application;
        original.application = std::make_shared<host::local_application_dispatch_t> ();
        original.application->owner.owner_kind = host::owner_kind_t::node;
        original.application->record.kind = host::record_kind_t::channel_send;
        original.application->parts = detail::backend::materialize_binding_parts ({{1}});
        ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    };
    for (int index = 0; index < 64; ++index)
        enqueue ();
    unsigned started = 0, rejected = 0;
    auto failed = host->dispatch_application_owner (
      "node",
      [] (const auto &, const auto &, auto) { throw std::runtime_error ("dispatch failed"); },
      [&] { ++started; }, [&] { ++rejected; });
    const auto &failure = failed.result ();
    EXPECT_FALSE (failure);
    EXPECT_THROW (failure.value (), std::runtime_error);
    EXPECT_EQ (failure.error (), nullptr);
    EXPECT_EQ (started, 64u);
    EXPECT_EQ (rejected, 64u);
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    enqueue ();
    unsigned dispatched = 0;
    auto next = host->dispatch_application_owner (
      "node", [&] (const auto &, const auto &, auto) { ++dispatched; }, [] {}, [] {});
    EXPECT_TRUE (next.result ().value ());
    EXPECT_EQ (dispatched, 1u);
    host->close ();
}

TEST (FrameworkRelocationSealContract, source_store_reads_return_before_store_lane_completes)
{
    runtime::in_memory_location_repository_t store;
    const auto verify = [&] (auto read) {
        auto release = runtime::in_memory_location_repository_test_access_t::hold_lane (store);
        auto returned = std::async (std::launch::async, read);
        EXPECT_EQ (returned.wait_for (std::chrono::milliseconds (100)), std::future_status::ready);
        release.set_value ();
        auto completed = returned.get ();
        EXPECT_TRUE (completed.result ());
    };
    verify ([&] { return store.list_mesh_nodes ("mesh"); });
    verify ([&] { return store.read_owner_lease ("owner"); });
}

TEST (FrameworkRelocationSealContract, source_lookup_is_shared_by_claim_and_cut)
{
    struct deferred_store_t : runtime::in_memory_location_repository_t
    {
        int reads = 0;
        task_completion_source_t<location_page_t<mesh_node_descriptor_t>> listed;
        task_t<location_page_t<mesh_node_descriptor_t>>
        list_mesh_nodes (std::string, location_page_request_t = {}) override
        {
            ++reads;
            return listed.task ();
        }
    } store;
    const auto source_owner =
      std::get<owner_lease_claimed_t> (
        store.claim_owner_lease ("source-owner", std::chrono::seconds (15)).result ().value ())
        .token;
    runtime::live_location_reader_t reader (store);
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("source-lookup"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const auto local =
      std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3});
    host->configure_stateful_dispatch ([local] { return local; }, &reader);
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    auto &mailbox = host->transport ().mailbox ();
    auto original = spot_send (*host, spot, 1, 2);
    const auto key = original.owner;
    ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    auto claim =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, key, 1, 4096);
    ASSERT_TRUE (claim);
    auto prepared = mailbox.prepare ({claim, 0}, [&] (auto &record) {
        return stateful::claimed_ingress_t::prepare (record, &reader, "mesh");
    });
    ASSERT_TRUE (prepared);
    EXPECT_EQ (store.reads, 0);
    stateful::accepted_record_authority_query_t query;
    query.target = spot;
    query.source_node_routing_id = {2};
    query.source_node_generation = 1;
    query.source_original = prepared->source_original;
    const auto descriptor = host->transport ().topology ().local_descriptor ();
    query.target_node_routing_id = descriptor.node_routing_id;
    query.target_node_generation = descriptor.lifecycle_generation;
    auto first = host->accepted_record_authority (query, local, &reader);
    EXPECT_EQ (store.reads, 1);
    EXPECT_FALSE (first.await_ready ());
    auto cut = mailbox.cut_owners ({key}, {}, [] {});
    ASSERT_TRUE (cut);
    ASSERT_EQ ((*cut)[0].size (), 1u);
    auto cut_query = stateful::claimed_ingress_t::prepare ((*cut)[0][0], &reader, "mesh");
    query.source_original = cut_query.source_original;
    auto second = host->accepted_record_authority (query, local, &reader);
    EXPECT_EQ (store.reads, 1);
    EXPECT_FALSE (second.await_ready ());
    location_page_t<mesh_node_descriptor_t> page;
    page.items.push_back ({.mesh_name = "mesh",
                           .rid = zlink::routing_id_t::from (std::vector<std::uint8_t>{2}),
                           .lifecycle_generation = 1,
                           .descriptor_revision = 1,
                           .endpoint = "tcp://127.0.0.1:1",
                           .state = framework_runtime_state_t::serving,
                           .security_identity = "test",
                           .owner_id = source_owner.owner_id,
                           .lease_generation = source_owner.lease_generation});
    store.listed.complete (
      result_t<location_page_t<mesh_node_descriptor_t>>::success (std::move (page)));
    const auto accepted = first.result ().value ();
    const auto captured = second.result ().value ();
    ASSERT_TRUE (accepted);
    ASSERT_TRUE (captured);
    EXPECT_EQ (accepted->source, captured->source);
    EXPECT_EQ (captured->source.owner_id, source_owner.owner_id);
    EXPECT_EQ (captured->source.lease_generation,
               static_cast<std::uint64_t> (source_owner.lease_generation));
    EXPECT_EQ (store.reads, 1);
    if (const auto credit = (*cut)[0][0].transferred)
        EXPECT_TRUE (credit ());
    host->close ();
}

TEST (FrameworkRelocationSealContract, unavailable_source_read_keeps_unavailable_result)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("source-read-error"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    stateful::raw_stateful_dispatch_t dispatch (
      host->objects (), host->transport (), [] (const auto &) {
          return task_t<std::optional<stateful::accepted_record_authority_t>> (
            result_t<std::optional<stateful::accepted_record_authority_t>>::failure (
              framework_error_kind_t::unavailable, "source repository is unavailable"));
      });
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "actor", 1, 1, "mesh", "source"};
    protocol::actor_message_header_t header;
    header.target = {actor.key, 1, {1}, 1, 1, 3};
    header.operation = {1, 1};
    const auto [error, authority] =
      dispatch.accepted_actor_authority (actor, header, {2}, 1, std::nullopt, false, std::nullopt)
        .result ()
        .value ();
    EXPECT_EQ (error, stateful::stateful_error_t::unavailable);
    EXPECT_FALSE (authority);
    host->close ();
}

TEST (FrameworkRelocationSealContract, relocation_callbacks_are_immutable_after_transport_start)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("startup-relocation-callbacks"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    EXPECT_NO_THROW (host->configure_relocation_gate ({}));
    host->start ();
    // Force the former write/read overlap boundary: transport is already running.
    // Even clearing callbacks must be rejected before mutating the published set.
    EXPECT_THROW (host->configure_relocation_gate ({}), std::logic_error);
    host->close ();
}

TEST (FrameworkRelocationSealContract, aggregate_cut_uses_spot_gate_after_actor)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto gate = std::make_shared<runtime::serial_execution_queue_t> (
      worker, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("aggregate-cut"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    host->configure_relocation_gate (
      [gate] (const auto &owner) -> task_t<std::shared_ptr<runtime::serial_execution_queue_t>> {
          co_return owner.kind == stateful::object_kind_t::actor ? nullptr : gate;
      });
    runtime::serial_execution_queue_t::async_completion_t finish;
    ASSERT_TRUE (
      gate->try_post_async ("active", [&] (auto complete) { finish = std::move (complete); }));
    worker.run_all ();
    ASSERT_TRUE (finish);
    auto record = std::make_shared<runtime::serial_application_record_t> ();
    record->payload ().emplace (stateful::turn_record_t{42, {1}});
    runtime::serial_work_options_t options;
    options.application_record = record;
    bool ran = false;
    ASSERT_TRUE (gate->try_post ("suffix", [&] { ran = true; }, std::move (options)));
    worker.run_all ();
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "actor", 1, 1, "mesh", "source"};
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    auto cutting = host::public_host_runtime_test_access_t::cut (*host, {actor, spot});
    worker.run_until (cutting);
    ASSERT_TRUE (cutting.await_ready ());
    const auto exported = cutting.result ().value ();
    ASSERT_TRUE (exported);
    ASSERT_EQ (exported->size (), 2u);
    EXPECT_TRUE ((*exported)[0].empty ());
    ASSERT_EQ ((*exported)[1].size (), 1u);
    EXPECT_EQ ((*exported)[1][0].sequence, 42u);
    finish ([] {});
    worker.run_all ();
    EXPECT_FALSE (ran);
    gate->drain ();
    worker.run_all ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, cut_exception_preserves_cause_after_abort)
{
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    objects.configure_relocation_cut (
      [] (auto, auto,
          auto) -> task_t<std::optional<std::vector<std::vector<stateful::turn_record_t>>>> {
          throw std::runtime_error ("source cut failed");
          co_return std::nullopt;
      });
    const auto failed = objects.try_seal_relocation_aggregate ({spot}).result ();
    ASSERT_FALSE (failed.has_value ());
    try {
        (void) failed.value ();
        FAIL () << "cut failure must retain its exception";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "source cut failed");
    }
    EXPECT_TRUE (objects.find (spot.kind, spot.key, stateful::object_state_t::ready));
}

TEST (FrameworkRelocationSealContract, cut_local_exception_reports_original_cause)
{
    auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    int reports = 0;
    failures->bind_runtime_reporter ([&] (std::exception_ptr cause) {
        ++reports;
        try {
            std::rethrow_exception (cause);
        }
        catch (const std::runtime_error &error) {
            EXPECT_STREQ (error.what (), "source lookup failed");
        }
    });
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-error"), .runtime_failures = failures}});
    host->configure_stateful_dispatch (
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          throw std::runtime_error ("source lookup failed");
          co_return std::nullopt;
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    auto application = std::make_shared<host::local_application_dispatch_t> ();
    application->owner.spot_id = spot.key;
    application->record.kind = host::record_kind_t::spot_request;
    application->record.spot_route = protocol::spot_route_fence_t{spot.key, 1, {1}, 1, 1, 3};
    application->record.operation_id = {1, 1};
    application->record.reply_route_id = 1;
    mesh::service_mailbox_record_t original;
    original.domain = mesh::service_mailbox_domain_t::application;
    original.application = application;
    ASSERT_TRUE (
      host::public_host_runtime_test_access_t::ingest_cut (*host, spot, std::move (original))
        .result ());
    EXPECT_EQ (reports, 1);
    host->close ();
}

// Relocation flow §4.2 cut S: an accepted, unstarted original is saved exactly once wherever it
// waits — the source FIFO (B), a claim suspended on its source authority (C) or the mailbox
// (D). The started turn (A) reaches its terminal on the source first, and a record published
// after the cut (E) takes the ingress hold.
TEST (FrameworkRelocationSealContract, seal_cut_saves_each_unstarted_original_once)
{
    using namespace std::chrono_literals;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-owner"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
    const auto claim_authority = std::make_shared<task_completion_source_t<void>> ();
    const auto suspended = std::make_shared<std::atomic_int> (0);
    host->configure_stateful_dispatch (
      [fence, claim_authority, suspended] (const stateful::accepted_record_authority_query_t &query)
        -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          // C's claim, the first query from source 3, waits at an explicit barrier.
          if (query.source_node_routing_id == std::vector<std::uint8_t>{3}
              && suspended->fetch_add (1) == 0)
              co_await claim_authority->task ();
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    auto &objects = host->objects ();
    auto &dispatch = access_t::dispatch (*host);
    auto &mailbox = host->transport ().mailbox ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "cut-spot", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    std::vector<mesh::service_mailbox_record_t> claimed;
    claimed.push_back (spot_send (*host, spot, 1, 2));
    claimed.push_back (spot_send (*host, spot, 2, 2));
    claimed.push_back (spot_send (*host, spot, 3, 3));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (claimed));
    const auto retained = zlink::framework::test::retained_ingress_snapshot (objects, spot);
    const auto slot = [&] (std::size_t index) {
        return mesh::service_mailbox_t::claim_handle_t{claim, index};
    };
    ASSERT_EQ (dispatch.ingest_claimed (spot, slot (0), retained).result ().value (),
               stateful::stateful_error_t::none);
    // A starts: the state lane hands its turn to the execution owner.
    const auto [started_error, started] = dispatch.try_claim (spot);
    ASSERT_EQ (started_error, stateful::stateful_error_t::none);
    ASSERT_TRUE (started);
    // B waits in the source FIFO, C in its claim, D in the mailbox behind that claim.
    ASSERT_EQ (dispatch.ingest_claimed (spot, slot (1), retained).result ().value (),
               stateful::stateful_error_t::none);
    auto late = dispatch.ingest_claimed (spot, slot (2), retained);
    ASSERT_FALSE (late.await_ready ());
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 4, 2)));
    auto sealing = std::async (std::launch::async, [&] {
        return objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    });
    // The cut moves C and D behind B in the source FIFO; the seal then waits for A.
    EXPECT_TRUE (eventually (
      [&] { return objects.pending (spot, stateful::turn_domain_t::application) == 3u; }));
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 5, 2)));
    EXPECT_EQ (sealing.wait_for (100ms), std::future_status::timeout);
    // C's claim lost its take: no second admission and no settlement of the cut credit.
    const auto outstanding = claim->outstanding ();
    claim_authority->complete (result_t<void>::success ());
    EXPECT_EQ (late.result ().value (), stateful::stateful_error_t::not_found);
    EXPECT_EQ (claim->outstanding (), outstanding);
    // A's terminal releases the seal.
    ASSERT_EQ (dispatch.complete (*started, std::nullopt), stateful::stateful_error_t::none);
    const auto sealed = sealing.get ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    EXPECT_EQ (operations_of (sealed.seal.participants.front ().pending_application),
               (std::vector<std::uint64_t>{2, 3, 4}));
    // The saved credits return once relocation owns their payloads; E is claimed next and
    // takes the ingress hold.
    dispatch.release_relocated_payloads (spot,
                                         sealed.seal.participants.front ().pending_application);
    EXPECT_EQ (zlink::framework::test::ingest_stateful_mailbox_record (objects, host->transport (),
                                                                       dispatch, spot)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto [boundary_error, batch] = objects.begin_relocation_boundary (sealed.seal.token);
    ASSERT_EQ (boundary_error, stateful::stateful_error_t::none);
    ASSERT_EQ (batch.participants.size (), 1u);
    EXPECT_EQ (operations_of (batch.participants.front ().records),
               (std::vector<std::uint64_t>{5}));
    host->close ();
}

// Relocation flow §4.2 step 10: when the second saved record fails its conversion, the attempt
// aborts and every original returns to the same source FIFO in order, with no reply and with
// its own mailbox credit still outstanding.
void verify_native_spot_seal (bool fail_second,
                              bool per_actor = false,
                              bool detached = false,
                              bool without_ingress = false)
{
    using namespace zlink::framework::runtime;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-cut"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester-A", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "native-spot", 1, 1, "mesh", "source"};
    auto &objects = host->objects ();
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "native-cut-test");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      per_actor ? serial_lane_policy_t::per_actor_spot () : serial_lane_policy_t::spot_wide ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    auto located_gate = std::make_shared<std::shared_ptr<serial_execution_queue_t>> (gate);
    std::vector<std::shared_ptr<serial_application_record_t>> accepted;
    int handlers = 0;
    std::vector<std::uint64_t> executed;
    const auto native_wire = [&] (std::uint64_t operation,
                                  std::optional<std::uint64_t> correlation) {
        return native_spot_record (*host, spot, operation, correlation);
    };
    const mesh::service_mailbox_t::application_dispatch_t publish =
      [&] (const auto &, const host::receive_record_t &record,
           std::vector<zlink::message_t> parts) {
          ASSERT_TRUE (record.application_record);
          auto owner = record.application_record;
          if (fail_second && record.operation_id.low == 2) {
              auto invalid = protocol::decode_spot_message_header (
                protocol::message_bytes (native_wire (2, 77).parts.front ()),
                protocol::command::spotRequest);
              invalid.correlation = 0;
              owner->set_spot_identity ({std::move (invalid), source}, parts.front ().copy ());
          }
          owner->set_invocation (parts.back ().copy (), {});
          accepted.push_back (owner);
          serial_work_options_t options;
          options.application_record = owner;
          ASSERT_TRUE (gate->try_post (
            "typed-native",
            [&, owner, operation = record.operation_id.low] {
                ++handlers;
                executed.push_back (operation);
                (void) owner->complete_native (std::nullopt);
            },
            std::move (options)));
      };
    host->configure_relocation_gate (
      [located_gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return *located_gate;
      },
      [located_gate] (std::string, std::optional<protocol::spot_route_fence_t>)
        -> task_t<std::shared_ptr<serial_application_target_t>> {
          if (!*located_gate)
              throw detail::make_framework_origin_exception (framework_error_kind_t::not_found,
                                                             "source activation was retired");
          co_return nullptr;
      },
      without_ingress ? publish : mesh::service_mailbox_t::application_dispatch_t{});
    for (std::uint64_t operation : {1u, 2u}) {
        const auto claim = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot,
          native_wire (operation,
                       operation == 2 ? std::optional<std::uint64_t>{77} : std::nullopt));
        ASSERT_EQ (access_t::dispatch_claim (*host, claim.claim, publish).result ().value (), 1u);
        // Admission can publish before the gate imports its FIFO. The next claim
        // requires that import to return this original's native credit first.
        (void) gate->admit_native_application ({}).result ().value ();
        EXPECT_EQ (claim.claim->outstanding (), 0u);
    }
    ASSERT_EQ (accepted.size (), 2u);
    for (const auto &record : accepted)
        EXPECT_EQ (record->identity (), nullptr);
    EXPECT_EQ (objects.pending (spot, stateful::turn_domain_t::application), 0u);
    EXPECT_EQ (handlers, 0);
    const auto sealed = objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    if (fail_second) {
        EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    } else {
        ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
        ASSERT_EQ (sealed.seal.participants.size (), 1u);
        const auto &saved = sealed.seal.participants.front ().pending_application;
        ASSERT_EQ (saved.size (), 2u);
        EXPECT_EQ (operations_of (saved), (std::vector<std::uint64_t>{1, 2}));
        for (const auto &turn : saved) {
            ASSERT_TRUE (turn.frozen_record);
            EXPECT_FALSE (turn.payload.empty ());
            EXPECT_EQ (turn.frozen_record->source, source);
        }
        EXPECT_EQ (saved.back ().frozen_record->reply_route_id, 77u);
        std::vector<std::uint64_t> forwarded;
        if (detached) {
            const protocol::request_source_fence_t retained{"source-owner", 3, {1}, 1};
            ASSERT_EQ (objects
                         .begin_relocation_boundary (
                           sealed.seal.token,
                           [&] (const auto &, const auto &turn) {
                               forwarded.push_back (operation_of (turn));
                           },
                           retained)
                         .first,
                       stateful::stateful_error_t::none);
            located_gate->reset ();
        }
        const auto late = zlink::framework::test::publish_stateful_claim (host->transport (), spot,
                                                                          native_wire (3, 78));
        ASSERT_EQ (access_t::dispatch_claim (*host, late.claim, publish).result ().value (), 1u);
        EXPECT_EQ (accepted.size (), 2u);
        if (detached) {
            EXPECT_EQ (forwarded, (std::vector<std::uint64_t>{3}));
            *located_gate = gate;
        } else {
            const auto [error, boundary] = objects.begin_relocation_boundary (sealed.seal.token);
            ASSERT_EQ (error, stateful::stateful_error_t::none);
            EXPECT_EQ (operations_of (boundary.participants.front ().records),
                       (std::vector<std::uint64_t>{3}));
        }
        EXPECT_EQ (handlers, 0);
        ASSERT_EQ (objects.abort_relocation_before_cutover (sealed.seal.token).result ().value (),
                   stateful::stateful_error_t::none);
    }
    if (without_ingress) {
        // A production host binds its mailbox dispatcher and has no inventory scan.
        host->transport ().mailbox ().bind_application_dispatch ([] (auto &) {},
                                                                 [] (const auto &) {});
        (void) host->dispatch_ready (publish).result ().value ();
    } else {
        // Fresh native ingress may not pass the originals restored by this abort.
        const auto fresh = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot, native_wire (4, std::nullopt));
        ASSERT_EQ (access_t::dispatch_claim (*host, fresh.claim, publish).result ().value (), 1u);
        while (access_t::dispatch_owner (*host, spot, publish).result ().value ()) {
        }
    }
    gate->set_application_start_policy ([] (const auto &) { return true; });
    gate->drain ();
    EXPECT_EQ (executed, without_ingress ? (std::vector<std::uint64_t>{1, 2, 3})
                         : fail_second   ? (std::vector<std::uint64_t>{1, 2, 4})
                                         : (std::vector<std::uint64_t>{1, 2, 3, 4}));
    EXPECT_EQ (objects.pending (spot, stateful::turn_domain_t::application), 0u);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_spot_abort_executes_prefix_without_new_ingress)
{
    verify_native_spot_seal (false, false, false, true);
}

TEST (FrameworkRelocationSealContract,
      native_spot_materializes_only_at_seal_and_holds_later_ingress)
{
    verify_native_spot_seal (false);
}

TEST (FrameworkRelocationSealContract,
      native_spot_retained_ingress_survives_activation_gate_removal)
{
    verify_native_spot_seal (false, false, true);
    verify_native_spot_seal (false, true, true);
}

TEST (FrameworkRelocationSealContract, native_spot_second_conversion_failure_restores_originals)
{
    verify_native_spot_seal (true);
}

TEST (FrameworkRelocationSealContract, per_actor_spot_abort_restores_originals_before_fresh_native)
{
    verify_native_spot_seal (false, true);
    verify_native_spot_seal (true, true);
}

TEST (FrameworkRelocationSealContract, native_spot_commit_releases_source_gate)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-commit"),
        .runtime_failures = std::make_shared<runtime_failure_collector_t> ()}});
    host->configure_stateful_dispatch (
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "committed-spot", 1, 1, "mesh", "source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "source-commit");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().begin_relocation_boundary (sealed.seal.token).first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    int executed = 0;
    ASSERT_TRUE (gate->try_post ("ordinary-after-commit", [&] { ++executed; }));
    std::promise<void> observed;
    auto completed = observed.get_future ();
    ASSERT_TRUE (worker.try_submit_internal ([&] { observed.set_value (); }));
    completed.get ();
    EXPECT_EQ (executed, 1);
    gate->cancel_pending ();
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_spot_fallback_rereads_remote_and_local_original)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool reject_first_authority : {false, true}) {
            SCOPED_TRACE (local);
            SCOPED_TRACE (reject_first_authority);
            auto host = std::make_shared<host::public_host_runtime_t> (
              host::host_options_t{mesh::raw_mesh_node_options_t{
                .descriptor = test_descriptor ("native-fallback"),
                .runtime_failures = std::make_shared<runtime_failure_collector_t> ()}});
            const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
            int resolutions = 0;
            host->configure_stateful_dispatch (
              [&] (const stateful::accepted_record_authority_query_t &query)
                -> task_t<std::optional<stateful::accepted_record_authority_t>> {
                  EXPECT_EQ (query.source_node_routing_id, (std::vector<std::uint8_t>{2}));
                  ++resolutions;
                  if (reject_first_authority && resolutions == 1)
                      co_return std::nullopt;
                  co_return stateful::accepted_record_authority_t{source, 3};
              });
            const stateful::object_ref_t spot{
              stateful::object_kind_t::user_spot, "fallback-spot", 1, 1, "mesh", "source"};
            const auto created = host->objects ().begin_reserved_object (spot, "room", {});
            ASSERT_EQ (host->objects ().commit_create (created.attempt),
                       stateful::stateful_error_t::none);
            auto original = native_spot_record (*host, spot, 91, std::nullopt);
            if (local) {
                auto application = std::make_shared<host::local_application_dispatch_t> ();
                application->owner.owner_kind = host::owner_kind_t::spot;
                application->owner.spot_id = spot.key;
                application->record.kind = host::record_kind_t::spot_send;
                application->record.operation_id = {1, 91};
                application->record.source_node_rid =
                  zlink::routing_id_t::from (original.source_routing_id);
                application->record.spot_route =
                  protocol::spot_route_fence_t{spot.key, 1, {1}, 1, 1, 3};
                application->parts =
                  protocol::decode_application_parts (protocol::decode_application_payload (
                    protocol::message_bytes (original.parts.back ())));
                original.application = std::move (application);
                original.parts.clear ();
            }
            const auto slot = zlink::framework::test::publish_stateful_claim (
              host->transport (), spot, std::move (original));
            int dispatched = 0;
            EXPECT_EQ (host::public_host_runtime_test_access_t::dispatch_claim (
                         *host, slot.claim,
                         [&] (const auto &, const host::receive_record_t &record,
                              std::vector<zlink::message_t> parts) {
                             ++dispatched;
                             EXPECT_EQ (record.operation_id.low, 91u);
                             ASSERT_EQ (parts.size (), 2u);
                             EXPECT_EQ (parts.back ().to_string (), "{}");
                             if (record.complete_stateful_dispatch)
                                 record.complete_stateful_dispatch ();
                             if (record.release_mailbox_reservation)
                                 record.release_mailbox_reservation ();
                         })
                         .result ()
                         .value (),
                       1u);
            EXPECT_EQ (resolutions, 2);
            EXPECT_EQ (dispatched, 1);
            EXPECT_EQ (slot.claim->outstanding (), 0u);
            host->close ();
        }
    }
}

TEST (FrameworkRelocationSealContract, native_spot_target_wait_keeps_original_and_gate_free)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-target"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "target-spot", 1, 1, "mesh", "source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "target-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    task_completion_source_t<void> entered;
    task_completion_source_t<std::shared_ptr<serial_application_target_t>> release;
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      },
      [&] (std::string, std::optional<protocol::spot_route_fence_t>)
        -> task_t<std::shared_ptr<serial_application_target_t>> {
          entered.complete (result_t<void>::success ());
          co_return co_await release.task ();
      });
    const auto slot = zlink::framework::test::publish_stateful_claim (
      host->transport (), spot, native_spot_record (*host, spot, 1, std::nullopt));
    int dispatched = 0;
    auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
      *host, slot.claim,
      [&] (const auto &, const host::receive_record_t &record, std::vector<zlink::message_t>) {
          ++dispatched;
          record.release_mailbox_reservation ();
      });
    entered.task ().result ().value ();
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_TRUE (host->transport ()
                   .mailbox ()
                   .prepare (slot, [] (const auto &original) { return !original.parts.empty (); })
                   .value_or (false));
    auto pulse = gate->admit_native_application ({});
    // A pending target projection must not retain the execution gate.
    EXPECT_TRUE (pulse.result ());
    EXPECT_EQ (dispatched, 0);
    release.complete (result_t<std::shared_ptr<serial_application_target_t>>::success (nullptr));
    EXPECT_EQ (pending.result ().value (), 1u);
    EXPECT_EQ (dispatched, 1);
    EXPECT_EQ (slot.claim->outstanding (), 0u);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_preparation_outcomes_preserve_context_and_settlement)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool deferred : {false, true}) {
            for (const bool failed : {false, true}) {
                SCOPED_TRACE (::testing::Message () << local << "/" << deferred << "/" << failed);
                auto host = std::make_shared<host::public_host_runtime_t> (
                  host::host_options_t{mesh::raw_mesh_node_options_t{
                    .descriptor = test_descriptor ("prepare-outcome"),
                    .runtime_failures =
                      std::make_shared<runtime::runtime_failure_collector_t> ()}});
                host->configure_stateful_dispatch ([] (const auto &) {
                    return task_t<std::optional<stateful::accepted_record_authority_t>> (
                      result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                        stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
                });
                const stateful::object_ref_t spot{
                  stateful::object_kind_t::user_spot, "target", 1, 1, "mesh", "source"};
                const auto created = host->objects ().begin_reserved_object (spot, "room", {});
                ASSERT_EQ (created.error, stateful::stateful_error_t::none);
                ASSERT_EQ (host->objects ().commit_create (created.attempt),
                           stateful::stateful_error_t::none);
                using target_t = std::shared_ptr<serial_application_target_t>;
                task_completion_source_t<target_t> release;
                auto expected = std::make_shared<serial_application_target_t> ();
                auto context = std::make_shared<int> (17);
                expected->context = context;
                unsigned preparations = 0;
                host->configure_relocation_gate (
                  {}, [&] (std::string, std::optional<protocol::spot_route_fence_t>) {
                      ++preparations;
                      if (deferred)
                          return release.task ();
                      if (failed)
                          return task_t<target_t> (result_t<target_t>::failure (
                            framework_error_kind_t::not_found, "target no longer exists"));
                      return task_t<target_t> (result_t<target_t>::success (expected));
                  });
                const auto slot = zlink::framework::test::publish_stateful_claim (
                  host->transport (), spot,
                  local ? local_native_spot_record (*host, spot, 1)
                        : native_spot_record (*host, spot, 1, std::nullopt));
                unsigned dispatched = 0;
                auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
                  *host, slot.claim, [&] (const auto &, const auto &record, auto) {
                      ++dispatched;
                      ASSERT_TRUE (record.application_record);
                      const auto target = record.application_record->target;
                      ASSERT_TRUE (target);
                      if (failed) {
                          const auto error = std::get_if<framework_exception_t> (&target->context);
                          ASSERT_NE (error, nullptr);
                          EXPECT_EQ (error->kind (), framework_error_kind_t::not_found);
                      } else {
                          EXPECT_EQ (target, expected);
                          EXPECT_EQ (std::get<std::shared_ptr<void>> (target->context), context);
                      }
                      record.application_record->release_native_claim ();
                  });
                EXPECT_EQ (preparations, 1u);
                if (deferred) {
                    EXPECT_FALSE (pending.await_ready ());
                    EXPECT_EQ (dispatched, 0u);
                    EXPECT_EQ (slot.claim->outstanding (), 1u);
                    EXPECT_TRUE (host->transport ()
                                   .mailbox ()
                                   .prepare (slot,
                                             [] (const auto &record) {
                                                 return record.application
                                                          ? !record.application->parts.empty ()
                                                          : !record.parts.empty ();
                                             })
                                   .value_or (false));
                    release.complete (
                      failed ? result_t<target_t>::failure (framework_error_kind_t::not_found,
                                                            "target no longer exists")
                             : result_t<target_t>::success (expected));
                }
                EXPECT_EQ (pending.result ().value (), 1u);
                EXPECT_EQ (dispatched, 1u);
                EXPECT_EQ (slot.claim->outstanding (), 0u);
                EXPECT_FALSE (host->transport ().mailbox ().release (*slot.claim));
                host->close ();
            }
        }
    }
}

TEST (FrameworkRelocationSealContract,
      native_claim_suffix_failure_settles_once_after_ready_or_pending)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool deferred : {false, true}) {
            auto host = std::make_shared<host::public_host_runtime_t> (
              host::host_options_t{mesh::raw_mesh_node_options_t{
                .descriptor = test_descriptor ("native-suffix"),
                .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
            host->configure_stateful_dispatch ([] (const auto &) {
                return task_t<std::optional<stateful::accepted_record_authority_t>> (
                  result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                    stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
            });
            const stateful::object_ref_t spot{
              stateful::object_kind_t::user_spot, "suffix", 1, 1, "mesh", "source"};
            const auto created = host->objects ().begin_reserved_object (spot, "room", {});
            ASSERT_EQ (created.error, stateful::stateful_error_t::none);
            ASSERT_EQ (host->objects ().commit_create (created.attempt),
                       stateful::stateful_error_t::none);
            using target_t = std::shared_ptr<serial_application_target_t>;
            task_completion_source_t<target_t> release;
            unsigned preparations = 0;
            host->configure_relocation_gate (
              {},
              [&] (std::string, std::optional<protocol::spot_route_fence_t>) -> task_t<target_t> {
                  ++preparations;
                  if (preparations == 2)
                      throw std::runtime_error ("suffix target failed");
                  if (deferred)
                      return release.task ();
                  return task_t<target_t> (
                    result_t<target_t>::success (std::make_shared<serial_application_target_t> ()));
              });
            std::vector<mesh::service_mailbox_record_t> originals;
            for (unsigned index = 1; index <= 3; ++index)
                originals.push_back (local ? local_native_spot_record (*host, spot, index)
                                           : native_spot_record (*host, spot, index, std::nullopt));
            const auto claim = zlink::framework::test::publish_stateful_claims (
              host->transport (), spot, std::move (originals));
            unsigned dispatched = 0;
            auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
              *host, claim, [&] (const auto &, const auto &record, auto) {
                  ++dispatched;
                  ASSERT_TRUE (record.application_record);
                  record.application_record->release_native_claim ();
              });
            if (deferred) {
                EXPECT_FALSE (pending.await_ready ());
                EXPECT_EQ (claim->outstanding (), 3u);
                release.complete (
                  result_t<target_t>::success (std::make_shared<serial_application_target_t> ()));
            }
            EXPECT_THROW (pending.result ().value (), std::runtime_error);
            EXPECT_EQ (preparations, 2u);
            EXPECT_EQ (dispatched, 1u);
            EXPECT_EQ (claim->outstanding (), 0u);
            EXPECT_FALSE (host->transport ().mailbox ().release (*claim));
            EXPECT_EQ (host->transport ().mailbox ().pending_messages (
                         mesh::service_mailbox_domain_t::application),
                       0u);
            host->close ();
        }
    }
}

TEST (FrameworkRelocationSealContract, per_actor_native_publication_and_cut_share_one_owner)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-publication"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "publication-spot", 1, 1, "mesh", "source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "publication-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::per_actor_spot ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    std::promise<void> entered, release;
    auto entering = entered.get_future ();
    auto held = release.get_future ().share ();
    const auto slot = zlink::framework::test::publish_stateful_claim (
      host->transport (), spot, native_spot_record (*host, spot, 1, std::nullopt));
    auto publishing = std::async (std::launch::async, [&] {
        return host::public_host_runtime_test_access_t::dispatch_claim (
                 *host, slot.claim,
                 [&] (const auto &, const host::receive_record_t &record,
                      std::vector<zlink::message_t> parts) {
                     auto original = record.application_record;
                     original->set_invocation (parts.back ().copy (), {});
                     entered.set_value ();
                     held.wait ();
                     serial_work_options_t options;
                     options.application_record = std::move (original);
                     EXPECT_TRUE (gate->try_post ("accepted-native", [] {}, std::move (options)));
                 })
          .result ()
          .value ();
    });
    const auto observed = entering.wait_for (5s) == std::future_status::ready;
    EXPECT_TRUE (observed);
    if (!observed) {
        release.set_value ();
        (void) publishing.get ();
        return;
    }
    auto binding = std::make_shared<stateful::relocation_seal_binding_t> ();
    auto cut = gate->take_application_suffix (
      [binding] { binding->bound.store (true, std::memory_order_release); },
      [] (const std::function<void ()> &step) {
          step ();
          return true;
      },
      binding);
    EXPECT_FALSE (cut.await_ready ());
    release.set_value ();
    EXPECT_EQ (publishing.get (), 1u);
    const auto saved = cut.result ().value ();
    EXPECT_EQ (saved.size (), 1u);
    gate->cancel_pending ();
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, seal_conversion_failure_restores_fifo_once)
{
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-restore"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [fence] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    auto &objects = host->objects ();
    auto &dispatch = access_t::dispatch (*host);
    auto &mailbox = host->transport ().mailbox ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "restore-spot", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    std::vector<mesh::service_mailbox_record_t> claimed;
    claimed.push_back (spot_send (*host, spot, 1, 2, 11));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (claimed));
    ASSERT_EQ (dispatch
                 .ingest_claimed (spot, mesh::service_mailbox_t::claim_handle_t{claim, 0},
                                  zlink::framework::test::retained_ingress_snapshot (objects, spot))
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    // The second record has no canonical form: its reply route ID is zero.
    protocol::spot_message_header_t header;
    header.target = {spot.key, spot.object_generation, {1}, 1, 1, 3};
    header.operation = {1, 2};
    header.correlation = 12;
    auto invalid = stateful::accepted_spot_record (header, fence,
                                                   {"Probe", "application/json", {'{', '}'}}, true);
    invalid.reply_route_id = 0;
    ASSERT_EQ (objects.enqueue (spot, stateful::turn_domain_t::application,
                                stateful::turn_record_t{1000, {}, 1, std::move (invalid)}),
               stateful::stateful_error_t::none);
    // D waits in the mailbox behind the active claim.
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 3, 2, 13)));
    const auto outstanding = claim->outstanding ();
    const auto sealed = objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    // Abort restored the source FIFO with the cut original after the earlier ones; the claim
    // credit of B is still outstanding and the binding no longer holds new ingress.
    EXPECT_EQ (claim->outstanding (), outstanding);
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    std::vector<std::uint64_t> order;
    while (true) {
        const auto [error, turn] = objects.try_claim (spot, stateful::turn_domain_t::application);
        ASSERT_EQ (error, stateful::stateful_error_t::none);
        if (!turn)
            break;
        order.push_back (operation_of (*turn));
        ASSERT_EQ (objects.complete_claim (spot, stateful::turn_domain_t::application),
                   stateful::stateful_error_t::none);
    }
    EXPECT_EQ (order, (std::vector<std::uint64_t>{1, 2, 3}));
    host->close ();
}

TEST (FrameworkRelocationSealContract, lookup_owner_ports_suspend_without_blocking)
{
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    auto release = stateful::stateful_object_runtime_test_access_t::hold_lane (objects);
    auto find = objects.find_async (spot.kind, spot.key);
    auto resolve = objects.resolve_spot_async (spot.key);
    EXPECT_FALSE (find.await_ready ());
    EXPECT_FALSE (resolve.await_ready ());
    release.set_value ();
    EXPECT_EQ (find.result ().value (), spot);
    EXPECT_EQ (resolve.result ().value (), spot);
}

TEST (FrameworkRelocationSealContract, relocated_stage_returns_before_owner_lane_completes)
{
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("async-stage"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (objects, host->transport ());
    protocol::spot_message_header_t header;
    header.target = {spot.key, 1, {1}, 1, 1, 3};
    header.operation = {1, 1};
    const auto accepted = stateful::accepted_spot_record (
      header, {"sender", 3, {2}, 1}, {"Probe", "application/json", {'{', '}'}}, false);
    const auto as_task = []<typename T> (T value) -> task_t<stateful::stateful_error_t> {
        if constexpr (std::is_same_v<T, task_t<stateful::stateful_error_t>>)
            return std::move (value);
        else
            return task_t<stateful::stateful_error_t> (
              result_t<stateful::stateful_error_t>::success (value));
    };
    auto release = stateful::stateful_object_runtime_test_access_t::hold_lane (objects);
    auto returned = std::async (std::launch::async, [&] {
        return as_task (dispatch.stage_relocated (
          spot, {0, protocol::encode_frozen_application_record (accepted).canonical_bytes},
          [] (const auto &) { return true; }));
    });
    EXPECT_EQ (returned.wait_for (100ms), std::future_status::ready);
    release.set_value ();
    auto completed = returned.get ();
    EXPECT_EQ (completed.result ().value (), stateful::stateful_error_t::none);
    host->close ();
}

TEST (FrameworkRelocationSealContract, raw_dispatch_waits_for_state_lane_outside_its_mutex)
{
    using namespace std::chrono_literals;
    using access_t = stateful::stateful_object_runtime_test_access_t;
    for (const bool ingest : {false, true}) {
        SCOPED_TRACE (ingest ? "ingress commit" : "claim");
        auto host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("lane-wait"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        auto &objects = host->objects ();
        const stateful::object_ref_t object{
          stateful::object_kind_t::actor, "lane-owner", 1, 1, "mesh", "source"};
        const auto created = objects.begin_reserved_object (object, "actor", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
        const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
        stateful::raw_stateful_dispatch_t dispatch (
          objects, host->transport (),
          [&] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
              co_return stateful::accepted_record_authority_t{fence, 3};
          });
        std::optional<mesh::service_mailbox_t::claim_handle_t> slot;
        std::optional<stateful::retained_relocation_source_t> retained;
        if (ingest) {
            const protocol::actor_route_fence_t route{object.key, 1, {1}, 1, 1, 3};
            mesh::service_mailbox_record_t record;
            record.source_routing_id = {2};
            record.source_node_generation = 1;
            record.parts = detail::backend::materialize_binding_parts (
              {protocol::encode_actor_message_header (protocol::command::actorSend, std::nullopt,
                                                      route, {1, 1}),
               protocol::encode_application_payload ({"Probe", "application/json", {'{', '}'}})});
            slot = zlink::framework::test::publish_stateful_claim (host->transport (), object,
                                                                   std::move (record));
            retained = zlink::framework::test::retained_ingress_snapshot (objects, object);
        }
        auto release = access_t::hold_lane (objects);
        auto waiting = std::async (std::launch::async, [&] {
            if (ingest)
                return dispatch.ingest_claimed (object, *slot, retained).result ().value ();
            return dispatch.try_claim (object).first;
        });
        // The waiting call has reached the held lane.
        std::this_thread::sleep_for (100ms);
        const stateful::object_ref_t other{
          stateful::object_kind_t::actor, "other-owner", 1, 1, "mesh", "source"};
        auto unrelated =
          std::async (std::launch::async, [&] { dispatch.release_relocated_payloads (other, {}); });
        EXPECT_EQ (unrelated.wait_for (2s), std::future_status::ready);
        release.set_value ();
        unrelated.get ();
        EXPECT_EQ (waiting.get (), stateful::stateful_error_t::none);
        host->close ();
    }
}

// A seal that waits for an active turn returns to its caller; the turn's terminal resumes it.
TEST (FrameworkRelocationSealContract, seal_waiting_for_active_turn_does_not_hold_its_caller)
{
    using namespace std::chrono_literals;
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                .key = "busy",
                                                .stable_type = "actor",
                                                .mesh_name = std::string ("mesh"),
                                                .exclusive = true});
    ASSERT_EQ (created.status, stateful::create_status_t::reserved);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    ASSERT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
               stateful::stateful_error_t::none);
    ASSERT_TRUE (objects.try_claim (created.object, stateful::turn_domain_t::application).second);
    auto started = std::async (
      std::launch::async, [&] { return objects.try_seal_relocation_aggregate ({created.object}); });
    EXPECT_EQ (started.wait_for (2s), std::future_status::ready);
    EXPECT_EQ (objects.complete_claim (created.object, stateful::turn_domain_t::application),
               stateful::stateful_error_t::none);
    auto seal = started.get ();
    const auto sealed = seal.result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    EXPECT_EQ (sealed.seal.participants.front ().pending_application.size (), 0u);
}

// One failed participant capture aborts the aggregate: every participant returns to Ready
// with its own queued turn.
TEST (FrameworkRelocationSealContract, aggregate_participant_failure_aborts_every_participant)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 2,
                                            .pending_capacity = 2}});
    std::vector<stateful::object_ref_t> participants;
    for (const auto *key : {"first", "second"}) {
        const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                    .key = key,
                                                    .stable_type = "actor",
                                                    .mesh_name = std::string ("mesh"),
                                                    .exclusive = true});
        ASSERT_EQ (created.status, stateful::create_status_t::reserved);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
        ASSERT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
                   stateful::stateful_error_t::none);
        participants.push_back (created.object);
    }
    int captures = 0;
    objects.configure_relocation_state (
      [&] (const stateful::object_ref_t &owner, const std::string &stable_type,
           std::stop_token) -> task_t<stateful::frozen_object_state_t> {
          ++captures;
          if (owner.key == "second")
              throw std::runtime_error ("second participant capture failed");
          co_return stateful::frozen_object_state_t{.owner = owner, .stable_type = stable_type};
      },
      [] (const auto &, const auto &, std::stop_token) { return true; });
    const auto sealed = objects.try_seal_relocation_aggregate (participants).result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    EXPECT_EQ (captures, 2);
    for (const auto &participant : participants) {
        SCOPED_TRACE (participant.key);
        const auto [error, turn] =
          objects.try_claim (participant, stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::none);
        ASSERT_TRUE (turn);
        EXPECT_EQ (turn->sequence, 1u);
    }
    for (const auto &participant : participants)
        EXPECT_EQ (objects.complete_claim (participant, stateful::turn_domain_t::application),
                   stateful::stateful_error_t::none);
}

TEST (FrameworkRelocationSealContract, retained_target_keeps_recorded_sender_without_store)
{
    auto descriptor = test_descriptor ("authority-order");
    descriptor.node_routing_id = {1};
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = std::move (descriptor),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    int resolves = 0;
    auto sender = std::make_shared<mesh_node_descriptor_t> ();
    sender->owner_id = "actual-sender";
    sender->lease_generation = 11;
    const mesh::request_source_t recorded_source{sender};
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (),
      [&] (const auto &query) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          ++resolves;
          EXPECT_EQ (query.target.object_generation, object.object_generation);
          co_return co_await host->accepted_record_authority (
            query,
            std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
      });
    protocol::actor_message_header_t header;
    header.target = {object.key, 6, {1}, 1, 1, 3};
    // Ordinary Ready delivery keeps the existing current-object normalization.
    std::optional<stateful::retained_relocation_source_t> retained;
    ASSERT_TRUE (objects.find (object.kind, object.key, std::nullopt, &retained));
    EXPECT_EQ (
      dispatch.accepted_actor_authority (object, header, {2}, 1, recorded_source, false, retained)
        .result ()
        .value ()
        .first,
      stateful::stateful_error_t::none);
    EXPECT_EQ (resolves, 1);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, fence)
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_TRUE (objects.find (object.kind, object.key, std::nullopt, &retained));
    ASSERT_TRUE (retained);
    header.target.object_generation = 7;
    resolves = 0;
    const auto accept = [&] {
        return dispatch
          .accepted_actor_authority (object, header, {2}, 1, recorded_source, false, retained)
          .result ()
          .value ();
    };
    const auto accepted = accept ();
    ASSERT_EQ (accepted.first, stateful::stateful_error_t::none);
    ASSERT_TRUE (accepted.second);
    EXPECT_EQ (accepted.second->source.owner_id, "actual-sender");
    EXPECT_EQ (accepted.second->source.lease_generation, 11u);
    EXPECT_EQ (accepted.second->target_owner_lease_generation, 3u);
    EXPECT_EQ (accepted.second->source.node_routing_id, (std::vector<std::uint8_t>{2}));
    EXPECT_EQ (resolves, 1);
    ++header.target.authority_owner_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.authority_owner_generation;
    ++header.target.target_node_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.target_node_generation;
    ++header.target.owner_lease_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    EXPECT_EQ (resolves, 1);
    host->close ();
}

TEST (FrameworkRelocationSealContract,
      source_cleanup_failure_is_reported_with_flow_off_and_continues)
{
    auto state = std::make_shared<spot_node_builder_state_t> ("cleanup-failure");
    state->snapshot.routing_id = zlink::routing_id_t::from ("source");
    state->actor_types_by_id["bad"] = "actor";
    state->actor_generations["actor:bad"] = 2;
    state->actor_types_by_id["good"] = "actor";
    state->actor_generations["actor:good"] = 1;
    state->actor_instances["actor:bad"] = std::make_shared<int> (1);
    state->actor_instances["actor:good"] = std::make_shared<int> (2);
    state->dispatch.message_flow (message_flow_log_mode_t::off);
    state->monitoring = std::make_shared<monitoring_runtime_state_t> ();
    state->monitoring->runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    std::vector<std::exception_ptr> failures;
    state->monitoring->runtime_failures->bind_runtime_reporter (
      [&] (auto failure) { failures.push_back (failure); });
    detail::spot_node_runtime_t runtime (state);
    const auto source = zlink::routing_id_t::from ("source").to_string ();
    const std::vector<stateful::object_ref_t> sources{
      {stateful::object_kind_t::actor, "bad", 1, 1, "mesh", source},
      {stateful::object_kind_t::actor, "good", 1, 1, "mesh", source}};
    auto native = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cleanup-native"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = native->objects ();
    const auto created = objects.begin_reserved_object (sources.front (), "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed =
      objects.try_seal_relocation_aggregate ({sources.front ()}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (
      objects.begin_relocation_boundary (sealed.seal.token, [] (const auto &, const auto &) {})
        .first,
      stateful::stateful_error_t::none);
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    const auto retired = runtime.retire_relocation_source (sources).result ();
    // Shutdown's ordinary Actor destroy cannot reclaim a retained moving source.
    EXPECT_EQ (objects.destroy_actor (sources.front ()), stateful::stateful_error_t::moving);
    native->close ();
    EXPECT_TRUE (retired);
    ASSERT_EQ (failures.size (), 1u);
    EXPECT_NE (
      diagnostic_event_sink_t::exception_summary (failures.front ()).message.find ("generation"),
      std::string::npos);
    EXPECT_TRUE (state->actor_instances.contains ("actor:bad"));
    EXPECT_FALSE (state->actor_instances.contains ("actor:good"));
}

// The invariant is asserted by the runtime in Debug builds.
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
TEST (FrameworkRelocationSealContract, source_capacity_underflow_is_an_invariant_failure)
{
    EXPECT_DEATH (
      ([] {
          stateful::stateful_object_runtime_t objects;
          stateful::placement_candidate_t candidate{.mesh_name = "mesh",
                                                    .node_id = "source",
                                                    .stable_types = {"actor"},
                                                    .active_capacity = 1,
                                                    .pending_capacity = 1};
          objects.replace_placement_candidates ({candidate});
          const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                      .key = "underflow",
                                                      .stable_type = "actor",
                                                      .mesh_name = std::string ("mesh"),
                                                      .exclusive = true});
          (void) objects.commit_create (created.attempt);
          // Fault injection: discard the live object's capacity accounting.
          objects.replace_placement_candidates ({candidate});
          const auto sealed =
            objects.try_seal_relocation_aggregate ({created.object}).result ().value ();
          (void) objects.begin_relocation_boundary (sealed.seal.token,
                                                    [] (const auto &, const auto &) {});
          (void) objects.finalize_relocation_cutover (sealed.seal.token).result ().value ();
      }()),
      "active_count");
}

#endif

TEST (FrameworkRelocationSealContract, committed_payload_release_keeps_later_follow_request)
{
    using namespace zlink::framework::runtime;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("committed-source"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"original-requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "committed-spot", 1, 1, "mesh", "committed-source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto turn = [&] (std::uint64_t operation) {
        auto wire = native_spot_record (*host, spot, operation, operation + 70);
        const auto header = protocol::decode_spot_message_header (
          protocol::message_bytes (wire.parts.front ()), protocol::command::spotRequest);
        const auto application =
          protocol::decode_application_payload (protocol::message_bytes (wire.parts.back ()));
        stateful::turn_record_t record;
        record.sequence = operation;
        record.payload = protocol::encode_frozen_application_record (
                           stateful::accepted_spot_record (header, source, application, true))
                           .canonical_bytes;
        return record;
    };
    auto &dispatch = access_t::dispatch (*host);
    int committed_completions = 0, follow_completions = 0;
    ASSERT_EQ (dispatch
                 .stage_relocated (spot, turn (1),
                                   [&] (const auto &) {
                                       ++committed_completions;
                                       return true;
                                   })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ()
                 .begin_relocation_boundary (
                   sealed.seal.token, [] (const auto &, const auto &) {}, source)
                 .first,
               stateful::stateful_error_t::none);
    stateful::relocation_ingress_batch_t committed;
    ASSERT_EQ (host->objects ()
                 .finalize_relocation_cutover (sealed.seal.token, std::nullopt, &committed)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (committed.participants.size (), 1u);
    EXPECT_EQ (operations_of (committed.participants.front ().records),
               (std::vector<std::uint64_t>{1}));
    // This request belongs to the later Message Follow path, after the committed prefix.
    ASSERT_EQ (dispatch
                 .stage_relocated (spot, turn (2),
                                   [&] (const auto &) {
                                       ++follow_completions;
                                       return true;
                                   })
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    dispatch.release_relocated_payloads (spot, committed.participants.front ().records);
    auto forwarded = protocol::decode_frozen_record (turn (2).payload);
    const auto relay = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 7}, 1, {"source", 3, {1}, 1, "committed-authority"}, 1, forwarded);
    EXPECT_TRUE (dispatch.complete_relocated_source_async (spot, 2, relay, forwarded.application)
                   .result ()
                   .value ());
    EXPECT_EQ (follow_completions, 1);
    const auto captured = protocol::decode_frozen_record (turn (1).payload);
    const auto retired = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 7}, 1, {"source", 3, {1}, 1, "committed-authority"}, 1, captured);
    // Source payload release preserves the original request terminal (Location runtime §9.3).
    EXPECT_TRUE (dispatch.complete_relocated_source_async (spot, 1, retired, captured.application)
                   .result ()
                   .value ());
    EXPECT_EQ (committed_completions, 1);
    EXPECT_FALSE (dispatch.complete_relocated_source_async (spot, 1, retired, captured.application)
                    .result ()
                    .value ());
    EXPECT_EQ (committed_completions, 1);
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_public_requester_terminal_uses_registry_after_seal)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{.descriptor = test_descriptor ("native-requester"),
                                      .runtime_failures =
                                        std::make_shared<runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot"}});
    auto lease = std::make_shared<const location_owner_token_t> (
      location_owner_token_t{"native-requester-owner", 3});
    host->configure_stateful_dispatch ([lease] { return lease; });
    host->configure_spot_route_fence_resolver (
      [] (const auto &, const auto &, auto) -> task_t<std::optional<host::route_fence_t>> {
          co_return host::route_fence_t{1, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "native-local", 1, 1, "mesh", "native-requester"};
    const auto created = host->objects ().begin_reserved_object (spot, "framework.spot", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "native-requester-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    host->start ();
    EXPECT_THROW (host->configure_relocation_gate ({}), std::logic_error);
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::request;
    header.channel_name = "mesh";
    header.message_name = "NativeSourceRequest";
    std::vector<zlink::message_t> parts;
    parts.push_back (messaging::envelope_codec_t{}.encode_header (header));
    parts.push_back (zlink::message_t::from ("{}"));
    host::pending_operation_t pending;
    ASSERT_EQ (host->entry_spot ()
                 .request_to_spot (host->status ().routing_id (), spot.key, spot.object_generation,
                                   parts, pending, zlink::send_flags_t::none, 15s)
                 .result ()
                 .value (),
               zlink::submit_result_t::ok);
    const auto claim = host->transport ().mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mesh::service_mailbox_t::application_owner (host::owner_kind_t::spot, spot.key), 1, 65536);
    ASSERT_TRUE (claim);
    ASSERT_TRUE (claim->records.front ()->application);
    const auto *original_parts = claim->records.front ()->application->parts.data ();
    ASSERT_EQ (host::public_host_runtime_test_access_t::dispatch_claim (
                 *host, claim,
                 [gate, original_parts] (const auto &, const host::receive_record_t &record,
                                         std::vector<zlink::message_t> application) {
                     EXPECT_EQ (application.data (), original_parts);
                     record.application_record->set_invocation (application.back ().copy (), {});
                     serial_work_options_t options;
                     options.application_record = record.application_record;
                     EXPECT_TRUE (gate->try_post ("native-local", [] {}, std::move (options)));
                 })
                 .result ()
                 .value (),
               1u);
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const auto &saved = sealed.seal.participants.front ().pending_application;
    ASSERT_EQ (saved.size (), 1u);
    EXPECT_EQ (saved.front ().sequence, 0u);
    ASSERT_TRUE (saved.front ().frozen_record);
    const protocol::relocation_coordinator_fence_t coordinator{
      lease->owner_id, 3, host->status ().routing_id ().to_bytes (),
      host->status ().lifecycle_generation (), "native-local-authority"};
    const auto relay = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 6}, 1, coordinator, 1, *saved.front ().frozen_record);
    header.kind = messaging::message_kind_t::response;
    std::vector<zlink::message_t> response;
    response.push_back (messaging::envelope_codec_t{}.encode_header (header));
    response.push_back (zlink::message_t::from ("{}"));
    // The production source registration uses this completion port. Native originals
    // have no raw pending entry; their original public operation owns the reply.
    const bool completed =
      host::public_host_runtime_test_access_t::dispatch (*host)
        .complete_relocated_source_async (spot, saved.front ().sequence, relay,
                                          protocol::application_payload_t::from_parts (response))
        .result ()
        .value ();
    EXPECT_TRUE (completed);
    const bool ready =
      completed && eventually ([&] {
          (void) host
            ->dispatch_ready ([] (const auto &, const auto &, std::vector<zlink::message_t>) {})
            .result ();
          return pending.completion->task ().await_ready ();
      });
    EXPECT_TRUE (ready);
    if (ready) {
        const auto result = pending.completion->task ().result ();
        ASSERT_TRUE (result);
        EXPECT_EQ (result.value ().parts.back ().to_string (), "{}");
        EXPECT_EQ (host->transport ().pending_operation_count (), 0u);
    }
    EXPECT_EQ (
      host->objects ().abort_relocation_before_cutover (sealed.seal.token).result ().value (),
      stateful::stateful_error_t::none);
    EXPECT_EQ (host->objects ().discard_application (spot, 0), stateful::stateful_error_t::none);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract,
      public_requester_receives_relocated_terminal_without_old_owner)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    const auto make_host = [] (std::string rid) {
        return std::make_shared<host::public_host_runtime_t> (host::host_options_t{
          .mesh =
            mesh::raw_mesh_node_options_t{
              .descriptor = test_descriptor (std::move (rid)),
              .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()},
          .object_stable_types = {"framework.spot"}});
    };
    auto requester = make_host ("requester-A");
    auto previous = make_host ("previous-B");
    auto target = make_host ("target-C");
    auto lease =
      std::make_shared<const location_owner_token_t> (location_owner_token_t{"requester-owner", 3});
    requester->configure_stateful_dispatch ([lease] { return lease; });
    requester->configure_spot_route_fence_resolver (
      [] (const auto &, const auto &, auto) -> task_t<std::optional<host::route_fence_t>> {
          co_return host::route_fence_t{1, 1};
      });
    requester->start ();
    previous->start ();
    target->start ();
    ASSERT_TRUE (requester->connect_peer (previous->transport ().endpoint (),
                                          previous->status ().routing_id ()));
    ASSERT_TRUE (
      requester->connect_peer (target->transport ().endpoint (), target->status ().routing_id ()));
    ASSERT_TRUE (eventually ([&] {
        for (auto node : {requester, previous, target})
            (void) node->transport ().pump_one (std::chrono::steady_clock::now ()).result ();
        auto b =
          requester->transport ().topology ().peer (previous->status ().routing_id ().to_bytes ());
        auto c =
          requester->transport ().topology ().peer (target->status ().routing_id ().to_bytes ());
        return b && b->ready && c && c->ready;
    }));
    messaging::envelope_header_t envelope;
    envelope.kind = messaging::message_kind_t::request;
    envelope.channel_name = "mesh";
    envelope.message_name = "OriginalRequest";
    std::vector<zlink::message_t> parts;
    parts.push_back (messaging::envelope_codec_t{}.encode_header (envelope));
    parts.push_back (zlink::message_t::from ("{}"));
    host::pending_operation_t pending;
    auto submitted =
      requester->entry_spot ().request_to_spot (previous->status ().routing_id (), "relocated-spot",
                                                1, parts, pending, zlink::send_flags_t::none, 15s);
    ASSERT_EQ (submitted.result ().value (), zlink::submit_result_t::ok);
    ASSERT_TRUE (eventually ([&] {
        (void) previous->transport ().pump_one (std::chrono::steady_clock::now ()).result ();
        return previous->transport ().mailbox ().pending_messages (
                 mesh::service_mailbox_domain_t::application)
               != 0;
    }));
    auto claim = previous->transport ().mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 65536);
    ASSERT_TRUE (claim);
    auto accepted = previous->transport ().mailbox ().take ({claim, 0});
    ASSERT_TRUE (accepted);
    auto header = protocol::decode_spot_message_header (
      protocol::message_bytes (accepted->parts.front ()), protocol::command::spotRequest);
    ASSERT_TRUE (header.correlation);
    EXPECT_EQ (header.operation.high, pending.id.high);
    EXPECT_EQ (header.operation.low, pending.id.low);
    const protocol::request_source_fence_t source{lease->owner_id, 3,
                                                  requester->status ().routing_id ().to_bytes (),
                                                  requester->status ().lifecycle_generation ()};
    const protocol::relocation_coordinator_fence_t coordinator{
      "previous-owner", 4, previous->status ().routing_id ().to_bytes (),
      previous->status ().lifecycle_generation (), "authority-v1"};
    protocol::reply_relay_t relay{header.operation,
                                  *header.correlation,
                                  {2119, 5},
                                  1,
                                  coordinator,
                                  1,
                                  1,
                                  0,
                                  protocol::framework_error_code::none};
    const auto reply = protocol::application_payload_t::from_parts (parts);
    const auto relay_record = [&] (const protocol::reply_relay_t &value) {
        mesh::service_mailbox_record_t message;
        message.domain = mesh::service_mailbox_domain_t::infrastructure;
        message.source_routing_id = target->status ().routing_id ().to_bytes ();
        message.source_node_generation = target->status ().lifecycle_generation ();
        message.parts = detail::backend::materialize_binding_parts (
          {protocol::encode_reply_relay (value), protocol::encode_application_payload (reply)});
        return message;
    };
    auto wrong = relay;
    ++wrong.operation.high;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    wrong = relay;
    ++wrong.operation.low;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    wrong = relay;
    ++wrong.reply_route_id;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    std::atomic_int acknowledgements{0};
    ASSERT_TRUE (target->relocation_wire ().register_terminal_target ({relay, source, reply,
                                                                       [&] (auto) {
                                                                           ++acknowledgements;
                                                                           return true;
                                                                       },
                                                                       [] { return true; }}));
    // Only A and C run: B cannot proxy the reply or acknowledge on A's behalf.
    const auto dispatch = [] (const auto &, const auto &, std::vector<zlink::message_t>) {};
    const bool received = eventually ([&] {
        (void) target->dispatch_ready (dispatch).result ();
        (void) requester->dispatch_ready (dispatch).result ();
        return pending.completion->task ().await_ready () && acknowledgements == 1;
    });
    EXPECT_TRUE (received);
    if (received) {
        const auto result = pending.completion->task ().result ();
        ASSERT_TRUE (result);
        ASSERT_EQ (result.value ().parts.size (), 2u);
        EXPECT_EQ (result.value ().parts.back ().to_string (), "{}");
        EXPECT_EQ (target->relocation_wire ().pending_terminal_relays (), 0u);
        EXPECT_EQ (requester->relocation_wire ().process (relay_record (relay)).result ().value (),
                   stateful::raw_relocation_replay_result_t::terminal_duplicate);
        EXPECT_EQ (requester->transport ().pending_operation_count (), 0u);
        (void) target->dispatch_ready (dispatch).result ();
        EXPECT_EQ (acknowledgements, 1);
    }
    if (accepted->transferred)
        accepted->transferred ();
    requester->close ();
    previous->close ();
    target->close ();
}

TEST (FrameworkRelocationSealContract, bound_session_close_compares_the_current_descriptor_identity)
{
    // Session-Actor binding §4: the Session owner identity is the host's descriptor
    // (OwnerId, LeaseGeneration). The receiving Session owner compares command 54
    // with its current values exactly.
    auto session_owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("close-session-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const location_owner_token_t descriptor_owner{"session-host-7f3a", 6};
    session_owner->configure_session_route_owner (
      [descriptor_owner] { return std::optional<location_owner_token_t> (descriptor_owner); });
    int physical_closes = 0;
    const auto session_rid = zlink::routing_id_t::from ("close-session-rid");
    const auto connection =
      session_owner->sessions ().open (session_rid.to_hex (), {}, {}, 0, [&] () -> task_t<void> {
          ++physical_closes;
          co_return;
      });
    const stateful::object_ref_t session_actor{
      stateful::object_kind_t::actor, "close-actor", 7, 11, "player", "close-actor-owner"};
    const auto [bind_error, binding] =
      session_owner->sessions ().bind_remote (connection, session_actor, 1, 17);
    ASSERT_EQ (bind_error, stateful::stateful_error_t::none);
    const auto local = session_owner->status ();
    const auto close = [&] (std::string owner_id, std::uint64_t lease_generation) {
        const protocol::bound_session_close_t request{
          23,
          session_actor.key,
          session_actor.object_generation,
          {local.routing_id ().to_bytes (), local.lifecycle_generation (), std::move (owner_id),
           lease_generation, session_rid.to_bytes (), binding.binding_generation}};
        ASSERT_TRUE (
          session_owner->transport ().mailbox ().try_enqueue (mesh::service_mailbox_record_t{
            "bound-session-close", mesh::service_mailbox_domain_t::infrastructure,
            zlink::framework::detail::backend::materialize_binding_parts (
              {protocol::encode_bound_session_close (request)}),
            zlink::routing_id_t::from ("close-actor-owner").to_bytes (), std::nullopt, std::nullopt,
            1}));
        const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                       std::vector<zlink::message_t>) {};
        (void) session_owner->dispatch_ready (noop_dispatch).result ();
    };

    close ("session-host-other", 6);
    close ("session-host-7f3a", local.lifecycle_generation ());
    EXPECT_EQ (physical_closes, 0);
    close ("session-host-7f3a", 6);
    EXPECT_EQ (physical_closes, 1);
}

TEST (FrameworkRelocationSealContract, malformed_command_42_records_invalid_frame_dispatch_error)
{
    dispatch_options_t dispatch;
    dispatch.message_flow (message_flow_log_mode_t::normal);
    std::vector<message_dispatch_error_event_t> errors;
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [&] (const auto &event) { errors.push_back (event); });

    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("malformed-session-seal-owner"),
        .dispatch = dispatch,
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto source_rid = zlink::routing_id_t::from ("malformed-command-42-source");
    const std::vector<std::uint8_t> malformed{0x5a, 0x4d, 0x01, 0x2a, 0x00};
    ASSERT_TRUE (owner->transport ().mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      "malformed-command-42", mesh::service_mailbox_domain_t::infrastructure,
      zlink::framework::detail::backend::materialize_binding_parts ({malformed}),
      source_rid.to_bytes (), std::nullopt, std::nullopt, 1}));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>) {};
    try {
        (void) owner->dispatch_ready (noop_dispatch).result ();
    }
    catch (const protocol::service_wire_error_t &) {
    }

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

TEST (FrameworkRelocationSealContract, native_protocol_rejection_preserves_following_claim_record)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-protocol"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    host->configure_stateful_dispatch ([] (const auto &) {
        return task_t<std::optional<stateful::accepted_record_authority_t>> (
          result_t<std::optional<stateful::accepted_record_authority_t>>::success (
            stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
    });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "protocol", 1, 1, "mesh", "source"};
    const auto created = host->objects ().begin_reserved_object (spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    unsigned preparations = 0;
    host->configure_relocation_gate (
      {}, [&] (std::string, std::optional<protocol::spot_route_fence_t>) {
          ++preparations;
          return task_t<std::shared_ptr<serial_application_target_t>> (
            result_t<std::shared_ptr<serial_application_target_t>>::success (
              std::make_shared<serial_application_target_t> ()));
      });
    auto invalid = native_spot_record (*host, spot, 1, std::nullopt);
    invalid.parts.back () = zlink::message_t::from ("invalid wire payload");
    std::vector<mesh::service_mailbox_record_t> originals;
    originals.push_back (std::move (invalid));
    originals.push_back (native_spot_record (*host, spot, 2, std::nullopt));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (originals));
    unsigned dispatched = 0;
    auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
      *host, claim, [&] (const auto &, const auto &record, auto) {
          ++dispatched;
          EXPECT_EQ (record.operation_id.low, 2u);
          ASSERT_TRUE (record.application_record);
          record.application_record->release_native_claim ();
      });
    EXPECT_EQ (pending.result ().value (), 1u);
    EXPECT_EQ (preparations, 1u);
    EXPECT_EQ (dispatched, 1u);
    EXPECT_EQ (claim->outstanding (), 0u);
    EXPECT_FALSE (host->transport ().mailbox ().release (*claim));
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_pending_locator_retains_prepared_target_and_original)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        SCOPED_TRACE (local);
        auto host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("pending-locate"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        host->configure_stateful_dispatch ([] (const auto &) {
            return task_t<std::optional<stateful::accepted_record_authority_t>> (
              result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
        });
        const stateful::object_ref_t spot{
          stateful::object_kind_t::user_spot, std::string (197, 'p'), 1, 1, "mesh", "source"};
        const auto created = host->objects ().begin_reserved_object (spot, "room", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        auto expected = std::make_shared<serial_application_target_t> ();
        auto context = std::make_shared<int> (17);
        expected->context = context;
        task_completion_source_t<std::shared_ptr<serial_execution_queue_t>> release;
        unsigned preparations = 0, locations = 0, dispatched = 0;
        host->configure_relocation_gate (
          [&] (const auto &object) {
              ++locations;
              EXPECT_EQ (object.key, spot.key);
              EXPECT_EQ (object.object_generation, spot.object_generation);
              return release.task ();
          },
          [&] (std::string key, std::optional<protocol::spot_route_fence_t> route) {
              ++preparations;
              EXPECT_EQ (key, spot.key);
              EXPECT_TRUE (route);
              if (route)
                  EXPECT_EQ (route->spot_id, spot.key);
              return task_t<std::shared_ptr<serial_application_target_t>> (
                result_t<std::shared_ptr<serial_application_target_t>>::success (expected));
          });
        const auto slot = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot,
          local ? local_native_spot_record (*host, spot, 91)
                : native_spot_record (*host, spot, 91, std::nullopt));
        // The initiating callback object and its stack have returned before locate completes.
        auto pending = [&] {
            mesh::service_mailbox_t::application_dispatch_t dispatch =
              [&] (const auto &owner, const auto &record, auto parts) {
                  ++dispatched;
                  EXPECT_EQ (owner.spot_id, spot.key);
                  EXPECT_EQ (record.operation_id.low, 91u);
                  ASSERT_TRUE (record.spot_route);
                  EXPECT_EQ (record.spot_route->spot_id, spot.key);
                  EXPECT_EQ (record.spot_route->owner_lease_generation, 3u);
                  ASSERT_EQ (parts.size (), 2u);
                  EXPECT_EQ (parts.back ().to_bytes (), (std::vector<std::uint8_t>{'{', '}'}));
                  ASSERT_TRUE (record.application_record);
                  ASSERT_TRUE (record.application_record->target);
                  EXPECT_EQ (record.application_record->target, expected);
                  EXPECT_EQ (
                    std::get<std::shared_ptr<void>> (record.application_record->target->context),
                    context);
                  record.application_record->release_native_claim ();
              };
            return host::public_host_runtime_test_access_t::dispatch_claim (*host, slot.claim,
                                                                            std::move (dispatch));
        }();
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_EQ (preparations, 1u);
        EXPECT_EQ (locations, 1u);
        EXPECT_EQ (dispatched, 0u);
        EXPECT_EQ (slot.claim->outstanding (), 1u);
        EXPECT_TRUE (host->transport ()
                       .mailbox ()
                       .prepare (slot,
                                 [] (const auto &record) {
                                     return record.application ? !record.application->parts.empty ()
                                                               : !record.parts.empty ();
                                 })
                       .value_or (false));
        release.complete (result_t<std::shared_ptr<serial_execution_queue_t>>::success (nullptr));
        EXPECT_EQ (pending.result ().value (), 1u);
        EXPECT_EQ (dispatched, 1u);
        EXPECT_EQ (preparations, 1u);
        EXPECT_EQ (locations, 1u);
        EXPECT_EQ (slot.claim->outstanding (), 0u);
        EXPECT_FALSE (host->transport ().mailbox ().release (*slot.claim));
        host->close ();
    }
}
