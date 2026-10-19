/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"

#include <gtest/gtest.h>

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

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static stateful::raw_stateful_dispatch_t &dispatch (public_host_runtime_t &host)
    {
        return *host._stateful_dispatch;
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
    record.source_original = mesh::raw_mesh_node_owner_t::accepted_source (
      record.source_routing_id, record.source_node_generation, {},
      host.transport ().topology ().peer (record.source_routing_id).get ());
    return record;
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

    std::atomic_int deliveries{0};
    std::promise<void> delivered;
    auto delivery = delivered.get_future ();
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
      session_actor.object_generation);
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);

    host::bound_session_operations_t operations;
    operations.bind = [] (const protocol::bound_session_bind_t &, const zlink::routing_id_t &,
                          std::uint64_t, std::function<bool ()> submit_terminal_reply) {
        (void) submit_terminal_reply ();
        return host::bound_session_bind_operation_result_t{};
    };
    operations.send = [&deliveries, &delivered] (const protocol::bound_session_send_t &,
                                                 std::vector<zlink::message_t> parts) {
        if (parts.size () != 1)
            return stateful::stateful_error_t::invalid;
        deliveries.fetch_add (1, std::memory_order_release);
        delivered.set_value ();
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
    EXPECT_EQ (deliveries.load (std::memory_order_acquire), 1);
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
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::none);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               23u);
    EXPECT_EQ (bound_frames, 1);
    ASSERT_TRUE (gateway.record_bound_session_route (
      actor, owner->status ().routing_id (),
      zlink::routing_id_t::from (next_connection.connection_id), 1, 11, 17, 45));
    authoritative =
      owner->sessions ().bind_remote (next_connection, object, 1, 17, publish_current, 24);
    ASSERT_EQ (authoritative.first, stateful::stateful_error_t::none);
    EXPECT_EQ (owner->sessions ().current_binding (object.key)->binding_generation, 24u);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               24u);
    EXPECT_EQ (bound_frames, 2);
    const auto republished = gateway.record_bound_session_route_transition (
      actor, *gateway.bound_session_route_async (actor).result ().value (),
      [&] { ++bound_frames; });
    ASSERT_TRUE (republished);
    EXPECT_FALSE (republished.value ().changed);
    EXPECT_FALSE (republished.value ().previous);
    EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->binding_generation,
               24u);
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
      connection.connection_id, binding.binding_generation, actor.key, actor.object_generation);
    ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
    auto native_submission = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> first_started;
    auto first = first_started.get_future ();
    std::promise<void> last_submitted;
    auto last = last_submitted.get_future ();
    std::mutex mutex;
    std::vector<std::uint64_t> order;
    const auto retain = [&] {
        return [owner, native_submission, &first_started, &last_submitted, &mutex,
                &order] (std::optional<stateful::stream_dispatch_t> dispatch, message_flow_reason_t,
                         std::optional<result_t<void>>) -> task_t<void> {
            if (!dispatch)
                co_return;
            {
                const std::lock_guard lock (mutex);
                order.push_back (dispatch->inbound_sequence);
            }
            if (dispatch->inbound_sequence == 1) {
                first_started.set_value ();
                co_await native_submission->task ();
            }
            EXPECT_EQ (owner->sessions ().complete_inbound (*dispatch),
                       stateful::stateful_error_t::none);
            if (dispatch->inbound_sequence == 3)
                last_submitted.set_value ();
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
          connection.connection_id, binding.binding_generation, actor.key, actor.object_generation);
        ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
        std::promise<void> delivered;
        auto delivery = delivered.get_future ();
        std::promise<void> later_delivered;
        auto later_delivery = later_delivered.get_future ();
        const auto retain = [&] {
            return [owner, &delivered, &later_delivered] (
                     std::optional<stateful::stream_dispatch_t> dispatch, message_flow_reason_t,
                     std::optional<result_t<void>>) -> task_t<void> {
                if (dispatch) {
                    EXPECT_EQ (owner->sessions ().complete_inbound (*dispatch),
                               stateful::stateful_error_t::none);
                    if (dispatch->inbound_sequence == 1)
                        delivered.set_value ();
                    else
                        later_delivered.set_value ();
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
    const auto seal = owner->sessions ().seal_remote_route (
      connection.connection_id, binding.binding_generation, actor.key, 7);
    const auto other_seal = owner->sessions ().seal_remote_route (
      connection.connection_id, other.binding_generation, other_actor.key, 7);
    auto failure = std::make_shared<task_completion_source_t<void>> ();
    std::promise<message_flow_reason_t> settled;
    auto settlement = settled.get_future ();
    std::promise<bool> other_delivered;
    auto other_delivery = other_delivered.get_future ();
    const auto retain = [&] {
        return [owner, failure, &settled] (std::optional<stateful::stream_dispatch_t> dispatch,
                                           message_flow_reason_t reason,
                                           auto... errors) -> task_t<void> {
            if constexpr (sizeof...(errors) != 0) {
                if (std::get<0> (std::tuple{errors...}))
                    co_return;
            }
            if (!dispatch) {
                settled.set_value (reason);
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
                                     return [owner, &other_delivered] (
                                              std::optional<stateful::stream_dispatch_t> dispatch,
                                              message_flow_reason_t, auto...) -> task_t<void> {
                                         if (dispatch)
                                             (void) owner->sessions ().complete_inbound (*dispatch);
                                         other_delivered.set_value (dispatch.has_value ());
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
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    std::vector<message_dispatch_error_event_t> errors;
    std::promise<void> reported;
    auto report = reported.get_future ();
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (options,
                                                                      [&] (const auto &event) {
                                                                          errors.push_back (event);
                                                                          reported.set_value ();
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
    const auto seal = owner->sessions ().seal_remote_route (
      connection.connection_id, binding.binding_generation, actor.key, 7);
    ASSERT_EQ (seal.error, stateful::stateful_error_t::none);
    int settlements = 0;
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    const auto retain = [&] {
        return [owner, completion, &settlements,
                &options] (std::optional<stateful::stream_dispatch_t> dispatch,
                           message_flow_reason_t reason, auto... failures) -> task_t<void> {
            if constexpr (sizeof...(failures) != 0) {
                if (auto failed = std::get<0> (std::tuple{failures...})) {
                    dispatch_error_reporter_t (options).report_lazy ([&] {
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
            ++settlements;
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
    EXPECT_EQ (settlements, 1);
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
      first_connection.connection_id, first_binding.binding_generation, actor.key, 7);
    auto failure = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> started, reported;
    auto start = started.get_future ();
    auto report = reported.get_future ();
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (first_connection.connection_id, first_binding.binding_generation,
                                 actor.key, 1, std::chrono::milliseconds{0},
                                 [&] {
                                     return
                                       [owner, failure, &started, &reported] (
                                         std::optional<stateful::stream_dispatch_t> dispatch,
                                         message_flow_reason_t,
                                         std::optional<result_t<void>> failed) -> task_t<void> {
                                           if (failed) {
                                               reported.set_value ();
                                               co_return;
                                           }
                                           EXPECT_TRUE (dispatch);
                                           if (!dispatch)
                                               co_return;
                                           (void) owner->sessions ().complete_inbound (*dispatch);
                                           started.set_value ();
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
      next_connection.connection_id, next_binding.binding_generation, actor.key, 7);
    ASSERT_EQ (next_seal.error, stateful::stateful_error_t::none);
    std::promise<bool> delivered;
    auto delivery = delivered.get_future ();
    ASSERT_EQ (owner->sessions ()
                 .admit_inbound (next_connection.connection_id, next_binding.binding_generation,
                                 actor.key, 1, std::chrono::milliseconds{0},
                                 [&] {
                                     return [owner, &delivered] (
                                              std::optional<stateful::stream_dispatch_t> dispatch,
                                              message_flow_reason_t,
                                              std::optional<result_t<void>>) -> task_t<void> {
                                         if (dispatch)
                                             (void) owner->sessions ().complete_inbound (*dispatch);
                                         delivered.set_value (dispatch.has_value ());
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
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token),
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
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("follow-generation"),
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
      objects, host->transport (),
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          throw std::runtime_error ("Store is unavailable");
          co_return std::nullopt;
      });
    protocol::actor_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, std::nullopt, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_actor_authority (
                   object, header, {1}, 1, std::nullopt, false,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::generation_stale);
    host->close ();
}

TEST (FrameworkRelocationSealContract, retained_spot_rejects_previous_object_generation)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("follow-generation"),
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
      objects, host->transport (),
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          throw std::runtime_error ("Store is unavailable");
          co_return std::nullopt;
      });
    protocol::spot_message_header_t header;
    header.target = {object.key, object.object_generation, {1}, 1, 1, 3};
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, std::nullopt,
                   zlink::framework::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ()
                 .first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_EQ (dispatch
                 .accepted_spot_authority (
                   object, header, {1}, 1, std::nullopt,
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
    dispatch.release_relocated_payloads (spot);
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
          std::async (std::launch::async, [&] { dispatch.release_relocated_payloads (other); });
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

TEST (FrameworkRelocationSealContract, local_follow_uses_retained_source_without_store)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("authority-order"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 7, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    int reads = 0;
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (),
      [&] (const auto &query) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          ++reads;
          EXPECT_EQ (query.target.object_generation, object.object_generation);
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    protocol::actor_message_header_t header;
    header.target = {object.key, 6, {1}, 1, 1, 3};
    // Ordinary Ready delivery keeps the existing current-object normalization.
    std::optional<stateful::retained_relocation_source_t> retained;
    ASSERT_TRUE (objects.find (object.kind, object.key, std::nullopt, &retained));
    EXPECT_EQ (
      dispatch.accepted_actor_authority (object, header, {2}, 1, std::nullopt, false, retained)
        .result ()
        .value ()
        .first,
      stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
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
    reads = 0;
    const auto accept = [&] {
        return dispatch
          .accepted_actor_authority (object, header, {2}, 1, std::nullopt, false, retained)
          .result ()
          .value ();
    };
    const auto accepted = accept ();
    ASSERT_EQ (accepted.first, stateful::stateful_error_t::none);
    ASSERT_TRUE (accepted.second);
    EXPECT_EQ (accepted.second->source.node_routing_id, (std::vector<std::uint8_t>{2}));
    EXPECT_EQ (reads, 0);
    ++header.target.authority_owner_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.authority_owner_generation;
    ++header.target.target_node_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    --header.target.target_node_generation;
    ++header.target.owner_lease_generation;
    EXPECT_NE (accept ().first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 0);
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
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token),
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
    EXPECT_DEATH (([] {
                      stateful::stateful_object_runtime_t objects;
                      stateful::placement_candidate_t candidate{.mesh_name = "mesh",
                                                                .node_id = "source",
                                                                .stable_types = {"actor"},
                                                                .active_capacity = 1,
                                                                .pending_capacity = 1};
                      objects.replace_placement_candidates ({candidate});
                      const auto created =
                        objects.begin_create ({.kind = stateful::object_kind_t::actor,
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
                      (void) objects.finalize_relocation_cutover (sealed.seal.token);
                  }()),
                  "active_count");
}

#endif
