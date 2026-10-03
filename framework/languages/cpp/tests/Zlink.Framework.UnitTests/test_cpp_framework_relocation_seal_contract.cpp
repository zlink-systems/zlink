/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("seal-session-owner")}});
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
            {protocol::encode_bound_session_send (send),
             protocol::encode_application_payload (application)},
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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("publication-owner")}});
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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("fifo-owner")}});
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
          host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("hook-owner")}});
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
                            connection.connection_id, binding.binding_generation, actor.key, 7, 11,
                            target, 2, 19, throwing_hook),
                          std::runtime_error);
            EXPECT_EQ (owner->sessions ().current_binding (actor.key)->actor, target);
        } else {
            EXPECT_THROW (owner->sessions ().acknowledge_remote_abort (
                            connection.connection_id, binding.binding_generation, actor.key, 7, 11,
                            throwing_hook),
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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("failed-drain-owner")}});
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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("settlement-owner")}});
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
      host::host_options_t{mesh::raw_mesh_node_options_t{test_descriptor ("rebound-drain-owner")}});
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
