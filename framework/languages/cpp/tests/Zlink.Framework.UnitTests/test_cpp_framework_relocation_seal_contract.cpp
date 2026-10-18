/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
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
        if constexpr (std::is_same_v<Header, protocol::actor_message_header_t>)
            return host._stateful_dispatch
              ->accepted_actor_authority (target, header, std::move (source), 1, false)
              .result ()
              .value ();
        else
            return host._stateful_dispatch
              ->accepted_spot_authority (target, header, std::move (source), 1)
              .result ()
              .value ();
    }
};
} // namespace zlink::framework::runtime::host

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
      [] (const auto &, const auto &, std::stop_token) {
          return stateful::frozen_object_state_t{};
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
      [] (const auto &, const auto &, std::stop_token) {
          return stateful::frozen_object_state_t{};
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
    EXPECT_EQ (objects.abort_relocation_before_cutover (result.seal.token),
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
    EXPECT_EQ (objects.abort_relocation_before_cutover (captured.seal.token),
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
    ASSERT_EQ (objects.begin_relocation_boundary (
                 sealed.seal.token, [] (const auto &, const auto &) {}).first,
               stateful::stateful_error_t::none);
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    stateful::raw_stateful_dispatch_t dispatch (
      objects, host->transport (),
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      },
      [&] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    protocol::actor_message_header_t header;
    header.target.actor_id = object.key;
    header.target.object_generation = object.object_generation;
    header.target.authority_owner_generation = object.authority_owner_generation;
    header.target.owner_lease_generation = 3;
    EXPECT_EQ (dispatch.accepted_actor_authority (object, header, {1}, 1).result ().value ().first,
               stateful::stateful_error_t::none);
    --header.target.object_generation;
    EXPECT_NE (dispatch.accepted_actor_authority (object, header, {1}, 1).result ().value ().first,
               stateful::stateful_error_t::none);
    host->close ();
}

TEST (FrameworkRelocationSealContract, local_follow_checks_store_before_retained_source)
{
    auto owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("authority-order"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = owner->objects ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::actor, "moving", 1, 1, "mesh", "source"};
    const auto created = objects.begin_reserved_object (object, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (
      objects.begin_relocation_boundary (sealed.seal.token, [] (const auto &, const auto &) {})
        .first,
      stateful::stateful_error_t::none);
    int reads = 0;
    int local_reads = 0;
    bool current = false;
    const protocol::request_source_fence_t fence{"source-owner", 3, {1}, 1};
    stateful::raw_stateful_dispatch_t dispatch (
      objects, owner->transport (),
      [&] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          ++reads;
          co_return current ? std::optional{stateful::accepted_record_authority_t{fence, 3}}
                            : std::nullopt;
      },
      [&] (const auto &query) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          EXPECT_EQ (reads, 1);
          ++local_reads;
          auto source = fence;
          source.node_routing_id = query.source_node_routing_id;
          co_return stateful::accepted_record_authority_t{std::move (source), 3};
      });
    protocol::actor_message_header_t header;
    header.target.actor_id = object.key;
    header.target.object_generation = 1;
    header.target.authority_owner_generation = 1;
    header.target.owner_lease_generation = 3;
    auto accepted = dispatch.accepted_actor_authority (object, header, {1}, 1).result ().value ();
    EXPECT_EQ (accepted.first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
    EXPECT_EQ (local_reads, 1);
    current = true;
    reads = local_reads = 0;
    accepted = dispatch.accepted_actor_authority (object, header, {1}, 1).result ().value ();
    EXPECT_EQ (accepted.first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
    EXPECT_EQ (local_reads, 0);
    current = false;
    reads = local_reads = 0;
    accepted = dispatch.accepted_actor_authority (object, header, {2}, 1).result ().value ();
    EXPECT_EQ (accepted.first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
    EXPECT_EQ (local_reads, 1);
    ASSERT_TRUE (accepted.second);
    EXPECT_EQ (accepted.second->source.node_routing_id, std::vector<std::uint8_t>{2});
    header.target.authority_owner_generation = 2;
    reads = local_reads = 0;
    accepted = dispatch.accepted_actor_authority (object, header, {2}, 1).result ().value ();
    EXPECT_NE (accepted.first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
    EXPECT_EQ (local_reads, 0);
    header.target.authority_owner_generation = 1;
    auto unsealed = object;
    unsealed.key = "unsealed";
    const auto other = objects.begin_reserved_object (unsealed, "actor", {});
    ASSERT_EQ (other.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (other.attempt), stateful::stateful_error_t::none);
    header.target.actor_id = unsealed.key;
    reads = local_reads = 0;
    accepted = dispatch.accepted_actor_authority (unsealed, header, {2}, 1).result ().value ();
    EXPECT_NE (accepted.first, stateful::stateful_error_t::none);
    EXPECT_EQ (reads, 1);
    EXPECT_EQ (local_reads, 0);
    owner->close ();
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
