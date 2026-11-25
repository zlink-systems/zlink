/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/session_disconnect_probe_spot.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int common_session_disconnect_uses_actor_packet_route ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    serializer_registry_t serializers;
    auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "disconnect-test");
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("disconnect-node");
    spot->spot_id = spot_id_t ("disconnect-spot");
    spot->spot_name = "disconnect";
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    // The callbacks live in node state and can run after this scope ends, so they own their state.
    auto callbacks_counter = std::make_shared<std::atomic_int> (0);
    const auto &callbacks = *callbacks_counter;
    spot_actor_admission_callbacks_t admission;
    admission.on_disconnect_actor = [callbacks_counter] (void *, void *) -> task_t<void> {
        ++*callbacks_counter;
        co_return;
    };
    test::register_session_disconnect_probe (spot, std::move (admission.on_disconnect_actor));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("disconnect-node", "player", "player-one", 1);
    node->actor_instances.emplace ("player:player-one", std::make_shared<int> (7));
    node->actor_spot_ids.emplace ("player:player-one", spot->spot_id);
    node->actor_generations.emplace ("player:player-one", 1);
    node->actor_types_by_id.emplace ("player-one", "player");
    record_actor_instance_index_unlocked (*node, actor,
                                          node->actor_instances.at ("player:player-one").get ());
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    // Canonical shortRid vector shared by .NET and Node. The runtime has no binding.
    const std::vector<std::uint8_t> bytes{6, 'n', 'o', 'd', 'e', '-', 'a', 0, 0, 0, 0, 0,
                                          0, 0,   2,   0,   0,   0,   0,   0, 0, 0, 7};
    auto delivered =
      spot_node_runtime_t (node)
        .relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::request,
                             runtime::protocol::session_disconnected_packet_name,
                             zlink::message_t::from (bytes), provider, serializers)
        .result ();
    std::cerr << "common Session disconnect: delivered=" << bool (delivered)
              << " callbacks=" << callbacks << std::endl;
    if (!delivered && delivered.error ())
        std::cerr << "common Session disconnect failure: " << delivered.error ()->what ()
                  << std::endl;
    if (!delivered || !delivered.value () || !delivered.value ()->is_empty () || callbacks != 1)
        return 1;
    const auto deliver = [&] (const std::vector<std::uint8_t> &body) {
        // A Message Follow hop has a different transport source from the body owner.
        return spot_node_runtime_t (node)
          .relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::request,
                               runtime::protocol::session_disconnected_packet_name,
                               zlink::message_t::from (body), provider, serializers, {}, nullptr,
                               {}, {}, zlink::routing_id_t::from ("forwarding-node"))
          .result ();
    };
    const auto empty_success = [] (const auto &result) {
        return result && result.value () && result.value ()->is_empty ();
    };
    if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("node-a"),
                                             zlink::routing_id_t::from ("session-a"), 2, 1, 1, 7)
        || !empty_success (deliver (bytes)) || callbacks != 2)
        return 2;
    for (const auto offset : {1u, 14u, 22u}) {
        auto stale = bytes;
        ++stale[offset];
        if (!empty_success (deliver (stale)) || callbacks != 2)
            return 3;
    }
    auto errors_owner =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    node->dispatch.message_flow (message_flow_log_mode_t::normal);
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      node->dispatch, [errors_owner] (const auto &event) { errors_owner->record (event); });
    auto trailing = bytes;
    trailing.push_back (0);
    auto truncated = bytes;
    truncated.pop_back ();
    auto zero_owner = bytes;
    zero_owner[14] = 0;
    auto zero_binding = bytes;
    zero_binding[22] = 0;
    for (const auto &malformed :
         {std::vector<std::uint8_t>{}, trailing, truncated, zero_owner, zero_binding}) {
        if (!empty_success (deliver (malformed)) || callbacks != 2)
            return 4;
    }
    test::acknowledge_delivery ();
    const auto errors = errors_owner->snapshot ();
    if (errors.size () != 5)
        return 5;
    for (const auto &error : errors) {
        if (error.message_kind != dispatch_message_kind_t::actor_send) {
            std::cerr << "Session disconnect malformed diagnostic kind mismatch: expected "
                         "actor_send, actual="
                      << static_cast<int> (error.message_kind) << std::endl;
            return 6;
        }
        if (error.reason != dispatch_error_reason_t::invalid_frame
            || error.action != dispatch_error_action_t::drop
            || dispatch_error_reporter_t::default_log_level (error) != log_level_t::warn)
            return 6;
        try {
            std::rethrow_exception (error.exception);
        }
        catch (const framework_exception_t &failure) {
            if (failure.kind () != framework_error_kind_t::protocol_error)
                return 7;
        }
    }
    // Block the Actor with an explicitly completed handler; admit the notification,
    // replace the binding, and release the handler. The comparison belongs to the
    // notification's Actor turn, after replacement, rather than its queue admission.
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto entered = std::make_shared<std::promise<bool>> ();
    auto entered_once = std::make_shared<std::atomic_bool> (false);
    auto started = entered->get_future ();
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "DisconnectHold", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [release, entered, entered_once] (
        void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
        const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          if (!entered_once->exchange (true))
              entered->set_value (true);
          co_await release->task ();
          co_return zlink::message_t{};
      });
    auto held = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "DisconnectHold",
      zlink::message_t{}, provider, serializers);
    detail::observe_task_completion (held, [entered, entered_once] (const auto &) {
        if (!entered_once->exchange (true))
            entered->set_value (false);
    });
    if (!started.get ()) {
        release->complete (result_t<void>::success ());
        (void) held.result ();
        return 8;
    }
    auto enqueued = std::make_shared<std::promise<bool>> ();
    auto enqueued_once = std::make_shared<std::atomic_bool> (false);
    auto accepted = enqueued->get_future ();
    auto queued = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      runtime::protocol::session_disconnected_packet_name, zlink::message_t::from (bytes), provider,
      serializers, {}, nullptr, {}, [enqueued, enqueued_once] {
          if (!enqueued_once->exchange (true))
              enqueued->set_value (true);
      });
    detail::observe_task_completion (queued, [enqueued, enqueued_once] (const auto &) {
        if (!enqueued_once->exchange (true))
            enqueued->set_value (false);
    });
    if (!accepted.get ()) {
        release->complete (result_t<void>::success ());
        (void) held.result ();
        (void) queued.result ();
        return 8;
    }
    const bool premature = queued.await_ready () || callbacks != 2;
    const auto replaced =
      gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("node-a"),
                                          zlink::routing_id_t::from ("session-b"), 2, 1, 1, 8);
    release->complete (result_t<void>::success ());
    const auto held_result = held.result ();
    const auto queued_result = queued.result ();
    if (!held_result || !queued_result || premature || !replaced || callbacks != 2)
        return 8;
    auto current_body = bytes;
    current_body[22] = 8;
    if (!empty_success (deliver (current_body)) || callbacks != 3)
        return 9;
    // Seal strictly after transfer admission and before Actor FIFO submission.
    // The final admission recheck must preserve the untouched body for replay.
    spot_node_runtime_t spots (node);
    std::shared_ptr<deferred_barrier_t> barrier;
    bool reserved = false;
    node->actor_factories.at ("player").configure_instance = [&] (void *,
                                                                  const actor_ref_t &current_actor,
                                                                  void *) {
        if (reserved)
            return;
        reserved = true;
        auto result = spots.reserve_actor_join_barrier (current_actor);
        if (result) {
            barrier = std::move (result.value ());
            const auto sealed =
              spots
                .seal_actor_application_suffix (
                  current_actor,
                  [&] {
                      if (!node->actor_transfer_coordinator->try_begin_local ("player:player-one"))
                          throw std::runtime_error ("disconnect wire source did not seal");
                  })
                .result ();
            if (!sealed)
                throw framework_exception_t (sealed.error_kind (), sealed.error ()->what ());
        }
    };
    auto capture =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                runtime::protocol::session_disconnected_packet_name,
                                zlink::message_t::from (current_body), provider, serializers);
    const bool terminal_before_release = capture.await_ready ();
    if (!terminal_before_release) {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move ("player:player-one");
        (void) capture.result ();
        return 10;
    }
    const auto preserved = capture.result ();
    auto backlog = node->actor_transfer_coordinator->take_backlog ("player:player-one");
    const bool captured =
      preserved && barrier && callbacks == 3 && backlog.size () == 1
      && backlog.front ().packet_name == runtime::protocol::session_disconnected_packet_name
      && backlog.front ().payload == current_body && !backlog.front ().is_request;
    if (barrier)
        barrier->cancel ();
    node->actor_transfer_coordinator->cancel_move ("player:player-one");
    if (!captured)
        return 10;
    if (!empty_success (deliver (backlog.front ().payload)) || callbacks != 4)
        return 11;
    auto callback_terminal = std::make_shared<task_completion_source_t<void>> ();
    auto callback_entered = std::make_shared<std::promise<bool>> ();
    auto callback_started = callback_entered->get_future ();
    auto callback_entered_once = std::make_shared<std::atomic_bool> (false);
    spot->actor_admissions.at (std::type_index (typeid (int))).on_disconnect_actor =
      [callback_terminal, callback_entered, callback_entered_once] (void *,
                                                                    void *) -> task_t<void> {
        if (!callback_entered_once->exchange (true))
            callback_entered->set_value (true);
        co_await callback_terminal->task ();
    };
    auto awaiting_callback = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request,
      runtime::protocol::session_disconnected_packet_name, zlink::message_t::from (current_body),
      provider, serializers);
    detail::observe_task_completion (awaiting_callback,
                                     [callback_entered, callback_entered_once] (const auto &) {
                                         if (!callback_entered_once->exchange (true))
                                             callback_entered->set_value (false);
                                     });
    const bool began = callback_started.get ();
    const bool acknowledged_before_callback = awaiting_callback.await_ready ();
    callback_terminal->complete (result_t<void>::success ());
    const auto callback_reply = awaiting_callback.result ();
    if (!began || acknowledged_before_callback || !callback_reply || !callback_reply.value ()
        || !callback_reply.value ()->is_empty ())
        return 12;
    spot->actor_admissions.at (std::type_index (typeid (int))).on_disconnect_actor =
      [] (void *, void *) -> task_t<void> {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                      "disconnect callback failed"));
    };
    const auto failed_callback = deliver (current_body);
    if (failed_callback
        || failed_callback.error_kind () != framework_error_kind_t::invalid_operation)
        return 13;
    node->worker_executor->drain ();
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
