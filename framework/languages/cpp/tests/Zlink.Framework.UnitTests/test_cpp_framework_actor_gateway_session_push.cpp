/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include <zlink/framework/contracts/configuration/app.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
zlink::framework::task_t<zlink::framework::result_t<void>>
invoke_app_bound_session_sender_with_temporary_arguments (
  const zlink::framework::detail::actor_gateway_state_t::bound_session_sender_t &sender,
  const zlink::framework::node_rid_t &node_rid,
  const std::string &actor_id)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto actor =
      std::make_unique<actor_ref_t> (actor_ref_access_t::make (node_rid, "player", actor_id, 7));
    auto header = std::make_unique<stream_header_t> (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      std::string (96, 'h'));
    auto payload =
      std::make_unique<zlink::message_t> (zlink::message_t::from (std::string (256, 'p')));
    auto local_sender = sender;
    return local_sender (*actor, 19, *header, *payload);
}

int app_bound_session_sender_keeps_arguments_alive_after_route_lookup_suspends ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto app = app_t::create ();
    const auto node_id = "bound-session-lifetime-node";
    const auto framework_node_rid = node_rid_t::from_string (node_id);
    const auto node_rid = zlink::routing_id_t::from (node_id);
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
        options.add_route_mesh ("bound-session-lifetime-mesh")
          .set_object_role (object_role_t::none)
          .set_routing_id (node_rid)
          .listen (0);
    });

    auto provider = app.advanced ().services ().build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    const auto state = gateway.weak_state ().lock ();
    if (!state || !state->bound_session_sender) {
        provider.close ();
        return 1;
    }

    const std::string actor_id = "bound-session-lifetime-actor-" + std::string (48, 'a');
    const auto actor = actor_ref_access_t::make (framework_node_rid, "player", actor_id, 7);
    if (!gateway.record_bound_session_route (
          actor, node_rid, zlink::routing_id_t::from ("bound-session-lifetime-session"), 11, 13, 17,
          19)) {
        provider.close ();
        return 2;
    }

    const char *const raw_arguments[] = {"actor-gateway-coroutine-lifetime"};
    auto **const arguments = const_cast<char **> (raw_arguments);
    int app_exit_code = -1;
    std::exception_ptr app_failure;
    std::thread app_thread ([&] {
        try {
            app_exit_code = app.run (1, arguments);
        }
        catch (const std::exception &) {
            app_failure = std::current_exception ();
        }
    });
    const auto app_ready_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!app.is_ready () && app.runtime_state () != framework_runtime_state_t::error
           && std::chrono::steady_clock::now () < app_ready_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (!app.is_ready ()) {
        app.stop ();
        app_thread.join ();
        provider.close ();
        return 3;
    }

    std::promise<void> lane_entered;
    auto lane_has_entered = lane_entered.get_future ();
    std::promise<void> lane_release;
    auto release_lane = lane_release.get_future ().share ();
    std::exception_ptr lane_failure;
    std::thread lane_thread ([&] {
        try {
            state->lane
              .run ([&] {
                  lane_entered.set_value ();
                  release_lane.wait ();
              })
              .get ();
        }
        catch (const std::exception &) {
            lane_failure = std::current_exception ();
        }
    });
    if (lane_has_entered.wait_for (std::chrono::seconds (1)) != std::future_status::ready) {
        lane_release.set_value ();
        lane_thread.join ();
        app.stop ();
        app_thread.join ();
        provider.close ();
        return 4;
    }

    const auto sender = state->bound_session_sender;
    auto pending = invoke_app_bound_session_sender_with_temporary_arguments (
      sender, framework_node_rid, actor_id);
    const bool suspended_on_route_lookup = !pending.result_for (std::chrono::milliseconds (0));
    lane_release.set_value ();
    lane_thread.join ();
    const auto sent = pending.result ();

    app.stop ();
    app_thread.join ();
    provider.close ();
    if (app_failure || app_exit_code != 0 || lane_failure) {
        return 5;
    }
    // The native Session may not exist in this unit scenario. The send outcome
    // is immaterial here: the registered app callback must resume and consume
    // its actor, header, and payload after route lookup has yielded.
    (void) sent;
    return suspended_on_route_lookup ? 0 : 6;
}

int bound_session_push_detaches_before_direct_sink_entry ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "player", "detached-bound-session", 7);
    // The second detached delivery can trace its completion after this test
    // returns, so the observer owns the flags it writes.
    auto saw_fifo_session = std::make_shared<std::atomic_bool> (false);
    auto saw_completion_session = std::make_shared<std::atomic_bool> (false);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options,
      [saw_fifo_session, saw_completion_session] (const message_flow_event_t &event) {
          if (event.surface != dispatch_error_surface_t::stream_session
              || event.stream_session_id
                   != std::optional<std::string> (
                     zlink::routing_id_t::from ("session-rid").to_hex ()))
              return;
          if (event.detail_stage == std::optional<std::string> ("fifo_accepted"))
              saw_fifo_session->store (true, std::memory_order_release);
          if (event.detail_stage == std::optional<std::string> ("detached_delivery_complete"))
              saw_completion_session->store (true, std::memory_order_release);
      });
    gateway.set_dispatch (std::move (trace_options));

    // The detached delivery can run after this scope ends, so the sink owns its gate.
    struct sender_gate_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        int started = 0;
        bool release = false;
    };
    auto gate = std::make_shared<sender_gate_t> ();
    auto direct_stream_sink = [gate] (std::string, stream_codec_t,
                                      const zlink::message_t &) -> task_t<void> {
        const auto ordinal = [&] {
            std::unique_lock lock (gate->mutex);
            const auto value = ++gate->started;
            gate->changed.notify_all ();
            if (value == 1)
                gate->changed.wait (lock, [&] { return gate->release; });
            return value;
        }();
        if (ordinal < 1)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "invalid detached sink order");
        co_return;
    };
    const auto installed = gateway.replace_session_route (
      actor, std::move (direct_stream_sink),
      actor_bound_session_route_t{zlink::routing_id_t::from ("session-owner"),
                                  zlink::routing_id_t::from ("session-rid"), 7, 11, 13, 17, 19, 23,
                                  0});
    if (!installed) {
        return 1;
    }

    auto submitted = std::async (std::launch::async, [&] {
        return gateway.actor_context (actor)
          .bound_session ()
          .send (std::string ("joined"))
          .async ()
          .result ();
    });
    const auto immediate = submitted.wait_for (std::chrono::milliseconds (100));
    auto second =
      gateway.actor_context (actor).bound_session ().send (std::string ("joined-again")).async ();
    {
        std::unique_lock lock (gate->mutex);
        if (!gate->changed.wait_for (lock, std::chrono::seconds (1),
                                     [&] { return gate->started == 1; })) {
            gate->release = true;
            gate->changed.notify_all ();
            return 2;
        }
        if (gate->started != 1 || !second.await_ready ()) {
            gate->release = true;
            gate->changed.notify_all ();
            return 3;
        }
        gate->release = true;
    }
    gate->changed.notify_all ();
    if (immediate != std::future_status::ready)
        return 4;
    if (!submitted.get () || !second.result ())
        return 5;
    {
        std::unique_lock lock (gate->mutex);
        if (!gate->changed.wait_for (lock, std::chrono::seconds (1),
                                     [&] { return gate->started == 2; })) {
            return 6;
        }
    }
    test::acknowledge_delivery ();
    if (!saw_fifo_session->load (std::memory_order_acquire)
        || !saw_completion_session->load (std::memory_order_acquire))
        return 8;
    return 0;
}

int admitted_bound_session_delivery_keeps_original_session_after_rebind ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "admitted-rebind", 7);
    const auto owner = zlink::routing_id_t::from ("session-owner");
    const auto original_rid = zlink::routing_id_t::from ("original-session");
    const auto replacement_rid = zlink::routing_id_t::from ("replacement-session");
    auto observations = std::make_shared<test::telemetry_observations_t<message_flow_event_t>> ();
    std::vector<message_flow_event_t> events;
    auto live_mode =
      std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::detailed);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_live_mode (trace_options, live_mode);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options,
      [observations] (const message_flow_event_t &event) { observations->record (event); });
    gateway.set_dispatch (std::move (trace_options));

    const auto original_route =
      actor_bound_session_route_t{owner, original_rid, 7, 11, 13, 17, 19, 0, 0};
    auto replacement_route =
      actor_bound_session_route_t{owner, replacement_rid, 7, 11, 13, 17, 23, 0, 0};
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          original_route))
        return 1;
    auto admitted = gateway.admit_bound_session_delivery (actor, 19);
    if (!admitted)
        return 2;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 3;
    if (!(*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body"))
           .result ())
        return 4;
    test::acknowledge_delivery ();
    events = observations->snapshot ();
    const auto original_hex = original_rid.to_hex ();
    const auto completion = std::find_if (events.begin (), events.end (), [&] (const auto &event) {
        return event.detail_stage
                 == std::optional<std::string> ("session_node_stream_write_terminal")
               && event.detail_result == std::optional<std::string> ("ok");
    });
    if (completion == events.end () || completion->stream_session_id != original_hex)
        return 5;

    replacement_route.session_rid = zlink::routing_id_t::from ("failed-session");
    replacement_route.binding_generation = 29;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (
                result_t<void>::failure (framework_error_kind_t::unavailable, "sink failure"));
          },
          replacement_route))
        return 6;
    admitted = gateway.admit_bound_session_delivery (actor, 29);
    if (!admitted)
        return 7;
    replacement_route.session_rid = zlink::routing_id_t::from ("later-session");
    replacement_route.binding_generation = 31;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 8;
    if ((*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body"))
          .result ())
        return 9;
    test::acknowledge_delivery ();
    events = observations->snapshot ();
    const auto failed_hex = zlink::routing_id_t::from ("failed-session").to_hex ();
    const auto failure = std::find_if (events.begin (), events.end (), [&] (const auto &event) {
        return event.detail_stage == std::optional<std::string> ("detached_delivery")
               && event.detail_result
               && event.detail_result->starts_with ("session_node_stream_write_terminal failed")
               && event.outcome == message_flow_outcome_t::dropped;
    });
    if (failure == events.end () || failure->stream_session_id != failed_hex)
        return 10;
    const auto failure_stage =
      std::find_if (events.begin (), events.end (), [&] (const auto &event) {
          return event.detail_stage
                   == std::optional<std::string> ("session_node_stream_write_terminal")
                 && event.outcome == message_flow_outcome_t::admitted && event.detail_result
                 && event.detail_result->starts_with ("failed");
      });
    if (failure_stage == events.end () || failure_stage->stream_session_id != failed_hex)
        return 11;

    live_mode->store (message_flow_log_mode_t::off, std::memory_order_relaxed);
    const auto event_count_before_off = events.size ();
    admitted = gateway.admit_bound_session_delivery (actor, 31);
    test::acknowledge_delivery ();
    events = observations->snapshot ();
    if (!admitted || events.size () != event_count_before_off)
        return 12;
    replacement_route.session_rid = zlink::routing_id_t::from ("newest-session");
    replacement_route.binding_generation = 37;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 13;
    live_mode->store (message_flow_log_mode_t::detailed, std::memory_order_relaxed);
    if (!(*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body"))
           .result ())
        return 14;
    test::acknowledge_delivery ();
    events = observations->snapshot ();
    const auto switched_completion = std::find_if (
      events.begin () + static_cast<std::ptrdiff_t> (event_count_before_off), events.end (),
      [&] (const auto &event) {
          return event.detail_stage
                   == std::optional<std::string> ("session_node_stream_write_terminal")
                 && event.detail_result == std::optional<std::string> ("ok");
      });
    return switched_completion != events.end ()
               && switched_completion->stream_session_id
                    == zlink::routing_id_t::from ("later-session").to_hex ()
             ? 0
             : 15;
}
} // namespace zlink::framework::test::actor_gateway
