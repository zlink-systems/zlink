/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/timers/async_delay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_application_relocation_remote_production_path (test_context_t &test,
                                                         bool resolver_failure,
                                                         bool post_seal_lookup_failure,
                                                         bool missing_target_session_peer,
                                                         bool command44_order,
                                                         bool binding_absent)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;
    namespace host = zlink::framework::runtime::host;

    const auto core_context = std::make_shared<zlink::context_t> ();
    auto resolver_observations = std::make_shared<
      framework::test::telemetry_observations_t<framework::message_flow_event_t>> ();
    auto resolver_logs =
      std::make_shared<framework::test::telemetry_observations_t<framework::log_record_t>> ();
    const auto make_state = [core_context] (const std::string &rid) {
        auto state = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::mesh_node_builder_state_t> ("production-relocation-mesh");
        state->core_context = core_context;
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_state->snapshot.actor_types.push_back ("production.actor");
        return state;
    };
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto source_state = make_state ("production-source");
    source_state->spot_state->dispatch.message_flow (framework::message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      source_state->spot_state->dispatch, [resolver_observations] (const auto &event) {
          if (event.surface == framework::dispatch_error_surface_t::actor_relocation)
              resolver_observations->record (event);
      });
    framework::logging_builder_t resolver_logging;
    resolver_logging.use_provider (
      "resolver-cause",
      [resolver_logs] (const framework::log_record_t &record) { resolver_logs->record (record); });
    detail::dispatch_options_access_t::set_logger (
      source_state->spot_state->dispatch, resolver_logging.create_logger ("resolver-cause"));
    std::mutex delay_mutex;
    detail::delay_owner_t delays (delay_mutex);
    detail::mesh_node_runtime_t source (source_state);
    detail::mesh_node_runtime_t target (make_state ("production-target"));
    detail::mesh_node_runtime_t session_owner (make_state ("production-session-owner"));
    detail::actor_gateway_runtime_t target_gateway (
      std::make_shared<detail::actor_gateway_state_t> ());
    target.configure_bound_session_operations (host::bound_session_operations_t{
      .bind =
        [] (const protocol::bound_session_bind_t &bind, const zlink::routing_id_t &, std::uint64_t,
            const framework::location_owner_token_t &,
            std::function<bool (std::uint32_t, std::uint32_t,
                                std::optional<protocol::stream_bind_reply_tail_t>)>
              submit_terminal_reply) {
            (void) submit_terminal_reply (
              static_cast<std::uint32_t> (protocol::request_terminal_result::ok),
              static_cast<std::uint32_t> (protocol::framework_error_code::none),
              protocol::stream_bind_reply_tail_t{bind.binding.generation,
                                                 bind.actor.authority_owner_generation});
            using bind_result_t = host::bound_session_bind_operation_result_t;
            return framework::task_t<bind_result_t> (
              framework::result_t<bind_result_t>::success (bind_result_t{}));
        },
      .send = [] (const protocol::bound_session_send_t &,
                  std::vector<zlink::message_t>) { return stateful_error_t::conflict; },
      .replaced = [] (const protocol::bound_session_replaced_t &) {},
      .commit_relocation_route = [] (const protocol::session_relocation_route_t &,
                                     const stream_binding_t &,
                                     const stream_binding_t &) { return false; },
      .prepare_relocation_target_route =
        [target_gateway] (const protocol::session_relocation_route_t &route,
                          std::uint64_t owner_lease_generation) mutable {
            return target_gateway.prepare_session_relocation_target_route (route,
                                                                           owner_lease_generation);
        }});
    source.bind_delay_owner (delays);
    target.bind_delay_owner (delays);
    session_owner.bind_delay_owner (delays);
    source.configure_relocation_runtime (authority, roots);
    target.configure_relocation_runtime (authority, roots);
    std::optional<detail::bound_session_relocation_route_t> bound_session_route;
    framework::task_completion_source_t<std::optional<detail::bound_session_relocation_route_t>>
      first_route_resolution;
    std::promise<void> first_route_started;
    auto route_started = first_route_started.get_future ();
    bool resolve_first_route = true;
    source.configure_bound_session_relocation_resolver (
      [&bound_session_route, &first_route_resolution, &first_route_started, &resolve_first_route,
       resolver_failure, post_seal_lookup_failure] (const object_ref_t &candidate)
        -> framework::task_t<std::optional<detail::bound_session_relocation_route_t>> {
          using route_t = std::optional<detail::bound_session_relocation_route_t>;
          if (!bound_session_route || candidate.key != "production-remote-actor"
              || candidate.object_generation != 1 || candidate.authority_owner_generation != 1)
              return framework::task_t<route_t> (
                framework::result_t<route_t>::success (std::nullopt));
          if (resolver_failure)
              throw std::runtime_error ("production Session owner lookup failed");
          if (std::exchange (resolve_first_route, false)) {
              first_route_started.set_value ();
              return first_route_resolution.task ();
          }
          if (post_seal_lookup_failure)
              throw std::runtime_error ("source Session route observation is unavailable");
          return framework::task_t<route_t> (
            framework::result_t<route_t>::success (*bound_session_route));
      });
    source.configure_stateful_dispatch (
      [] (accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<accepted_record_authority_t>> {
          co_return accepted_record_authority_t{
            {"target-owner", 9, query.source_node_routing_id, query.source_node_generation}, 1};
      });
    target.configure_stateful_dispatch (
      [] (accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<accepted_record_authority_t>> {
          co_return accepted_record_authority_t{
            {"source-owner", 1, query.source_node_routing_id, query.source_node_generation}, 9};
      });
    std::atomic_int late_routes{0};
    if (binding_absent)
        session_owner.configure_late_session_route_update ([&] (const auto &) { ++late_routes; });
    source.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"source-owner", 1}}; });
    target.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"target-owner", 9}}; });
    std::atomic<bool> route_owner_observed_target_ready{false};
    std::atomic<unsigned> route_commits_before_lifecycle{0};
    std::atomic<bool> command44_observed_open_dispatch{true};
    std::atomic<bool> lifecycle_callback_entered{false};
    std::atomic<bool> relocation_completed{false};
    framework::task_completion_source_t<void> target_reconcile;
    std::atomic<unsigned> target_reconcile_requests{0};
    std::atomic<unsigned> seal_reconcile_requests{0};
    std::atomic<bool> first_push_received{false};
    std::atomic<unsigned> bound_session_callback_errors{0};
    std::promise<void> lifecycle_release;
    auto release = lifecycle_release.get_future ();
    std::promise<void> order_marker_received;
    auto marker_received = order_marker_received.get_future ();
    session_owner.configure_session_route_owner ([&] {
        const auto inventory =
          target.native_node ().objects ().inventory_async ().result ().value ();
        const auto active =
          std::find_if (inventory.begin (), inventory.end (), [] (const auto &item) {
              return item.owner.key == "production-remote-actor";
          });
        if (active != inventory.end ()) {
            route_owner_observed_target_ready.store (active->state == object_state_t::ready,
                                                     std::memory_order_release);
            if (command44_order) {
                ++route_commits_before_lifecycle;
                if (active->state != object_state_t::ready)
                    command44_observed_open_dispatch.store (false, std::memory_order_release);
            }
        }
        return std::optional<framework::location_owner_token_t>{{"session-owner", 17}};
    });
    if (missing_target_session_peer) {
        session_owner.configure_bound_session_operations (
          {.bind =
             [&] (const auto &, const auto &, auto, const auto &, auto) {
                 ++bound_session_callback_errors;
                 using bind_result_t =
                   framework::runtime::host::bound_session_bind_operation_result_t;
                 return framework::task_t<bind_result_t> (
                   framework::result_t<bind_result_t>::success (
                     bind_result_t{stateful_error_t::conflict, std::nullopt}));
             },
           .send =
             [&] (const auto &record, auto parts) {
                 first_push_received.store (
                   record.actor.actor_id == "production-remote-actor" && parts.size () == 1
                     && parts.front ().to_string () == "first-push-payload",
                   std::memory_order_release);
                 return stateful_error_t::none;
             },
           .replaced = [&] (const auto &) { ++bound_session_callback_errors; },
           .commit_relocation_route =
             [&] (const auto &route, const auto &previous, const auto &committed) {
                 const bool projected =
                   previous.actor.node_id == source.status ().routing_id ().to_string ()
                   && committed.actor.node_id == target.status ().routing_id ().to_string ()
                   && committed.actor.key == "production-remote-actor"
                   && committed.actor.authority_owner_generation
                        == route.route.target_authority_owner_generation
                   && previous.connection == committed.connection
                   && previous.binding_generation == committed.binding_generation
                   && committed.binding_generation == route.binding_generation;
                 if (!projected)
                     ++bound_session_callback_errors;
                 return projected;
             },
           .prepare_relocation_target_route =
             [&] (const auto &, auto) {
                 ++bound_session_callback_errors;
                 return false;
             }});
    }
    source.start ().result ().value ();
    target.start ().result ().value ();
    session_owner.start ().result ().value ();
    if (missing_target_session_peer) {
        session_owner.native_node ().configure_peer_reconcile ([&] (const auto &rid) {
            if (rid == target.status ().routing_id ().to_bytes ())
                ++seal_reconcile_requests;
        });
        target.native_node ().configure_peer_reconcile ([&] (const auto &rid) {
            if (rid == session_owner.status ().routing_id ().to_bytes ())
                ++target_reconcile_requests;
            target_reconcile.complete (framework::result_t<void>::success ());
        });
    }
    if (command44_order) {
        target.native_node ().objects ().configure_relocation_materialization (
          [] (const auto &, const auto &, const auto &,
              std::stop_token) -> framework::task_t<bool> { co_return true; },
          [&] (const auto &) {
              lifecycle_callback_entered.store (true, std::memory_order_release);
              release.get ();
              return true;
          },
          [] (const auto &) -> framework::task_t<void> { co_return; });
    }
    source.connect_peer (target.status ().routing_id (), target.status ().local_endpoint ());
    source.connect_peer (session_owner.status ().routing_id (),
                         session_owner.status ().local_endpoint ());
    if (!missing_target_session_peer)
        target.connect_peer (session_owner.status ().routing_id (),
                             session_owner.status ().local_endpoint ());
    const auto admission_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (target.status ().routing_id (),
                                       target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (source.status ().routing_id (),
                                          source.status ().lifecycle_generation ())
            || !source.has_admitted_peer (session_owner.status ().routing_id (),
                                          session_owner.status ().lifecycle_generation ())
            || !session_owner.has_admitted_peer (source.status ().routing_id (),
                                                 source.status ().lifecycle_generation ())
            || (!missing_target_session_peer
                && (!target.has_admitted_peer (session_owner.status ().routing_id (),
                                               session_owner.status ().lifecycle_generation ())
                    || !session_owner.has_admitted_peer (
                      target.status ().routing_id (), target.status ().lifecycle_generation ()))))
           && std::chrono::steady_clock::now () < admission_deadline) {
        (void) await_task (source.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}));
        (void) await_task (target.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}));
        (void) await_task (
          session_owner.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}));
        std::this_thread::yield ();
    }
    test.require (
      source.has_admitted_peer (target.status ().routing_id (),
                                target.status ().lifecycle_generation ())
        && source.has_admitted_peer (session_owner.status ().routing_id (),
                                     session_owner.status ().lifecycle_generation ())
        && (missing_target_session_peer
            || target.has_admitted_peer (session_owner.status ().routing_id (),
                                         session_owner.status ().lifecycle_generation ())),
      "production relocation source must admit the target and Session owner");

    const auto created = source.create_application_actor (
      "production.actor", "production-remote-actor", std::nullopt, 1s);
    test.require (static_cast<bool> (created),
                  "remote production relocation Actor must be created");
    if (!created) {
        source.stop ();
        target.stop ();
        session_owner.stop ();
        return;
    }
    const auto actor = created.value ();
    framework::authority_snapshot_t snapshot{
      .store_version = "authority-remote-v1",
      .payload = {},
      .object_generation = actor.object_generation (),
      .authority_owner_generation = 1,
      .owner = {"source-owner", 1},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {
        framework::placement_allocation_state_t::active,
        framework::placement_object_kind_t::actor,
        "production.actor",
        {"production-relocation-mesh",
         framework::node_rid_t::from_string (source.status ().routing_id ().to_string ()),
         source.status ().lifecycle_generation (),
         {"source-owner", 1}},
        {1, 0, std::nullopt}}};
    framework::mesh_node_descriptor_t target_descriptor;
    target_descriptor.mesh_name = "production-relocation-mesh";
    target_descriptor.rid = target.status ().routing_id ();
    target_descriptor.lifecycle_generation = target.status ().lifecycle_generation ();
    target_descriptor.application_version = 1;
    target_descriptor.owner_id = "target-owner";
    target_descriptor.lease_generation = 9;

    const auto bound_source_object =
      source.native_node ().resolve_actor_async (actor).result ().value ();
    test.require (bound_source_object.has_value (),
                  "production relocation must resolve the bound Actor before Session sealing");
    if (!bound_source_object) {
        source.stop ();
        target.stop ();
        session_owner.stop ();
        return;
    }
    const auto session_connection = session_owner.native_node ().sessions ().open (
      zlink::routing_id_t::from ("production-bound-session").to_hex ());
    const auto [session_bind_error, session_binding] =
      session_owner.native_node ().sessions ().bind_remote (
        session_connection, *bound_source_object, source.status ().lifecycle_generation (), 1);
    const auto [initial_inbound_error, initial_inbound] =
      session_owner.native_node ().sessions ().admit_inbound (session_binding);
    test.require (
      session_bind_error == stateful_error_t::none
        && initial_inbound_error == stateful_error_t::none && initial_inbound
        && initial_inbound->inbound_sequence == 1,
      "production Session must retain active ingress until its owner seals the binding");
    bound_session_route = detail::bound_session_relocation_route_t{
      session_owner.status ().routing_id (),
      session_owner.status ().lifecycle_generation (),
      {"session-owner", 17},
      zlink::routing_id_t::from ("production-bound-session"),
      session_binding.binding_generation};

    const auto initial_inbound_completed =
      initial_inbound
      && session_owner.native_node ().sessions ().complete_inbound (*initial_inbound)
           == stateful_error_t::none;
    test.require (initial_inbound_completed,
                  "the active Session ingress must complete before sealing");

    if (binding_absent) {
        test.require (session_owner.native_node ().sessions ().unbind (session_binding).first
                        == stateful_error_t::none,
                      "the Session binding must be absent before command 42");
    }
    std::atomic<bool> stop_dispatch{false};
    const auto dispatch = [&] (detail::mesh_node_runtime_t &node) {
        while (!stop_dispatch.load (std::memory_order_acquire)) {
            bool saw_marker = false;
            (void) await_task (
              node.dispatch_ready ([&] (const auto &, const auto &, auto parts, auto) {
                  if ((command44_order || missing_target_session_peer) && &node == &session_owner
                      && !parts.empty () && parts.front ().to_string () == "command44-order-marker")
                      saw_marker = true;
              }));
            if (saw_marker) {
                // The preceding turn can pump command 44 before the marker
                // while leaving its infrastructure dispatch to this turn.
                (void) await_task (
                  node.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}));
                order_marker_received.set_value ();
            }
            std::this_thread::yield ();
        }
    };
    {
        /* The schema stream carries no participant stable type: the target
         * derives it from the Location Store authority row. */
        std::lock_guard lock (authority->mutex);
        authority->participant_identities = {relocation_participant_identity_t{
          *bound_source_object, "production.actor", std::nullopt}};
    }
    relocation_result_t result;
    const std::string flow_id = "01930000-0000-7000-8000-000000000001";
    std::promise<bool> relocation_submitted;
    auto submitted = relocation_submitted.get_future ();
    const auto operation_deadline = std::chrono::steady_clock::now () + 5s;
    std::thread relocation_thread ([&] {
        framework::runtime::install_host_context_hooks ();
        framework::runtime::flow_context_t::scope_t flow (
          framework::runtime::flow_value_t{flow_id, framework::flow_origin_t::application,
                                           framework::message_flow_log_mode_t::normal});
        auto relocation = source.relocate_application_actor (actor, target_descriptor, snapshot,
                                                             operation_deadline);
        relocation_submitted.set_value (!relocation.await_ready ());
        result = await_task (std::move (relocation));
        relocation_completed.store (true, std::memory_order_release);
        target_reconcile.complete (framework::result_t<void>::success ());
    });
    const bool relocation_suspended = submitted.get ();
    if (!resolver_failure) {
        test.require (
          relocation_suspended,
          "production relocation must suspend while Session route resolution is pending");
        while (route_started.wait_for (0s) != std::future_status::ready
               && !relocation_completed.load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < operation_deadline)
            std::this_thread::yield ();
        test.require (route_started.wait_for (0s) == std::future_status::ready,
                      "production relocation must reach Session route resolution");
        first_route_resolution.complete (
          framework::result_t<std::optional<detail::bound_session_relocation_route_t>>::success (
            *bound_session_route));
    }
    std::thread source_dispatch ([&] { dispatch (source); });
    std::thread target_dispatch ([&] { dispatch (target); });
    std::thread session_owner_dispatch ([&] { dispatch (session_owner); });
    if (missing_target_session_peer) {
        await_task (target_reconcile.task ());
        test.require (seal_reconcile_requests != 0 && target_reconcile_requests == 1
                        && !relocation_completed.load (std::memory_order_acquire),
                      "the target must request reconciliation and hold relay-ready until release");
        target.connect_peer (session_owner.status ().routing_id (),
                             session_owner.status ().local_endpoint ());
    }
    if (command44_order) {
        while (!lifecycle_callback_entered.load (std::memory_order_acquire)
               && !(relocation_completed.load (std::memory_order_acquire)
                    && result.terminal != relocation_terminal_t::completed)
               && std::chrono::steady_clock::now () < operation_deadline)
            std::this_thread::yield ();
        const auto callback_entered = lifecycle_callback_entered.load (std::memory_order_acquire);
        test.require (callback_entered, "command 44 fixture must reach target lifecycle commit");
        if (callback_entered) {
            protocol::application_payload_t::multipart_t marker;
            marker.push_back (zlink::message_t::from (std::string ("command44-order-marker")));
            const auto sent = await_task (target.native_node ().transport ().send_to_node_result (
              session_owner.status ().routing_id ().to_bytes (),
              protocol::application_payload_t::from_parts (std::move (marker))));
            test.require (sent == zlink::submit_result_t::ok,
                          "command 44 order marker must submit");
            if (sent == zlink::submit_result_t::ok) {
                while (marker_received.wait_for (0s) != std::future_status::ready
                       && std::chrono::steady_clock::now () < operation_deadline)
                    std::this_thread::yield ();
                test.require (marker_received.wait_for (0s) == std::future_status::ready,
                              "command 44 order marker must reach the Session owner");
            }
            test.require (route_commits_before_lifecycle == 0,
                          "command 44 must follow lifecycle completion and dispatch opening");
        }
        lifecycle_release.set_value ();
    }
    relocation_thread.join ();
    if (resolver_failure) {
        stop_dispatch.store (true, std::memory_order_release);
        source_dispatch.join ();
        target_dispatch.join ();
        session_owner_dispatch.join ();
        test.require (result.terminal != relocation_terminal_t::completed,
                      "Session owner lookup failure must reject relocation");
        framework::test::acknowledge_delivery ();
        const auto resolver_failures = resolver_observations->snapshot ();
        std::vector<const framework::message_flow_event_t *> failure_records;
        std::vector<const framework::message_flow_event_t *> terminal_records;
        for (const auto &event : resolver_failures) {
            if (event.detail_stage == "source_terminal")
                terminal_records.push_back (&event);
            else if (event.reason == framework::message_flow_reason_t::location_unavailable)
                failure_records.push_back (&event);
        }
        test.require (resolver_failures.size () == 2 && failure_records.size () == 1
                        && terminal_records.size () == 1,
                      "Session owner lookup failure must retain its source failure and report one "
                      "relocation terminal");
        if (failure_records.size () == 1) {
            const auto &event = *failure_records.front ();
            test.require (
              event.outcome == framework::message_flow_outcome_t::completed
                && event.result == framework::message_flow_result_t::failed
                && event.message_kind == framework::dispatch_message_kind_t::control
                && event.reason == framework::message_flow_reason_t::location_unavailable
                && event.actor_id == "production-remote-actor"
                && event.source_rid == source.status ().routing_id ().to_string ()
                && event.mesh_name == "production-relocation-mesh" && event.flow_id == flow_id
                && event.flow_origin == framework::flow_origin_t::application,
              "Session owner lookup failure must retain the Actor identity and ambient flow");
            bool actual_exception = false;
            if (event.exception) {
                try {
                    std::rethrow_exception (event.exception);
                }
                catch (const std::runtime_error &error) {
                    actual_exception = typeid (error) == typeid (std::runtime_error)
                                       && std::string_view (error.what ())
                                            == "production Session owner lookup failed";
                }
                catch (...) {
                }
            }
            test.require (actual_exception,
                          "Session owner lookup failure must retain the actual exception type and "
                          "message");
            const auto records = resolver_logs->snapshot ();
            const bool owned_cause =
              std::any_of (records.begin (), records.end (), [&] (const auto &record) {
                  const auto has = [&] (std::string_view key, std::string_view value) {
                      return std::any_of (record.fields.begin (), record.fields.end (),
                                          [&] (const auto &field) {
                                              return field.key == key && field.value == value;
                                          });
                  };
                  return has (detail::dispatch_event_field::actor, "production-remote-actor")
                         && has (detail::dispatch_event_field::flow, flow_id)
                         && has (detail::dispatch_event_field::error_type,
                                 typeid (std::runtime_error).name ())
                         && has (detail::dispatch_event_field::error_message,
                                 "production Session owner lookup failed");
              });
            test.require (
              event.exception && owned_cause,
              "Session owner lookup failure must report the owned error type and message");
        }
        if (terminal_records.size () == 1) {
            const auto &event = *terminal_records.front ();
            test.require (
              event.outcome == framework::message_flow_outcome_t::completed
                && event.result == framework::message_flow_result_t::failed
                && event.message_kind == framework::dispatch_message_kind_t::control
                && event.actor_id == "production-remote-actor" && event.flow_id == flow_id
                && event.flow_origin == framework::flow_origin_t::application
                && event.detail_result == "failed",
              "source relocation terminal must retain the Actor, flow pair, and confirmed result");
        }
        source.stop ();
        target.stop ();
        session_owner.stop ();
        return;
    }
    if (missing_target_session_peer) {
        auto published =
          target.native_node ().objects ().find (object_kind_t::actor, "production-remote-actor");
        while (!published && std::chrono::steady_clock::now () < operation_deadline) {
            std::this_thread::yield ();
            published = target.native_node ().objects ().find (object_kind_t::actor,
                                                               "production-remote-actor");
        }
        test.require (published.has_value (),
                      "first push must follow target lifecycle completion and publication");
        const auto push = await_task (target.native_node ().send_bound_session (
          actor, session_owner.status ().routing_id (), session_binding.binding_generation, 2, 9,
          {zlink::message_t::from (std::string ("first-push-payload"))}));
        test.require (push == zlink::submit_result_t::ok,
                      "first bound Actor push after relocation must succeed without a prior "
                      "target-to-Session peer (#1775)");
        if (push == zlink::submit_result_t::ok) {
            const auto marker = await_task (target.native_node ().transport ().send_to_node_result (
              session_owner.status ().routing_id ().to_bytes (),
              protocol::application_payload_t::from_parts (
                {zlink::message_t::from (std::string ("command44-order-marker"))})));
            test.require (marker == zlink::submit_result_t::ok, "first push marker must submit");
            if (marker == zlink::submit_result_t::ok) {
                while (marker_received.wait_for (0s) != std::future_status::ready
                       && std::chrono::steady_clock::now () < operation_deadline)
                    std::this_thread::yield ();
                test.require (marker_received.wait_for (0s) == std::future_status::ready,
                              "first push marker must reach the Session owner");
            }
            test.require (
              first_push_received.load (std::memory_order_acquire),
              "the Session owner must deliver the first push before the following marker");
        }
    }
    const auto source_relocation_gate = source.native_node ().maintenance ()->gate_snapshot ();
    const auto route_deadline = operation_deadline;
    while (!binding_absent && result.terminal == relocation_terminal_t::completed
           && std::chrono::steady_clock::now () < route_deadline) {
        const auto current =
          session_owner.native_node ().sessions ().current_binding ("production-remote-actor");
        if (current && current->actor.node_id == target.status ().routing_id ().to_string ())
            break;
        std::this_thread::yield ();
    }

    // Command 44 is one-way (Session Actor binding §8.2). Observing the new
    // binding is not an acknowledgement that its receive turn has returned.
    // Join the fixture's dispatchers before inspecting the final Session state.
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();
    session_owner_dispatch.join ();

    object_ref_t expected_target{
      object_kind_t::actor,         std::string (actor.actor_id ().value ()),
      actor.object_generation (),   2,
      "production-relocation-mesh", target.status ().routing_id ().to_string ()};
    if (binding_absent) {
        const auto restored = target.native_node ().objects ().find (
          object_kind_t::actor, std::string (actor.actor_id ().value ()));
        test.require (
          result.terminal == relocation_terminal_t::completed
            && restored == std::optional<object_ref_t>{expected_target} && result.target_handoff
            && result.target_handoff->sources == std::vector<object_ref_t>{*bound_source_object}
            && source_relocation_gate == relocation_gate_snapshot_t{},
          "bindingAbsent must preserve the exact Actor handoff and release the source gate");
        test.require (
          !session_owner.native_node ().sessions ().current_binding ("production-remote-actor")
            && late_routes.load () == 0,
          "bindingAbsent must not send command 44 or recreate a Session binding");
        const auto target_actor = detail::actor_ref_access_t::make (
          framework::node_rid_t::from_string (target.status ().routing_id ().to_string ()),
          "production.actor", std::string (actor.actor_id ().value ()), actor.object_generation ());
        test.require (
          !target_gateway.bound_session_route_async (target_actor).result ().value (),
          "bindingAbsent must omit the target Session route from the relocation envelope");
        {
            const std::lock_guard lock (roots->mutex);
            test.require (roots->roots.empty (),
                          "bindingAbsent must not create a durable Session journal");
        }
        source.stop ();
        target.stop ();
        session_owner.stop ();
        return;
    }
    const auto committed_binding =
      session_owner.native_node ().sessions ().current_binding ("production-remote-actor");
    const auto [continued_inbound_error, continued_inbound] =
      committed_binding
        ? session_owner.native_node ().sessions ().admit_inbound (*committed_binding)
        : std::pair{stateful_error_t::not_found, std::optional<stream_dispatch_t>{}};
    const auto continued_inbound_completed =
      continued_inbound
      && session_owner.native_node ().sessions ().complete_inbound (*continued_inbound)
           == stateful_error_t::none;
    if (missing_target_session_peer)
        test.require (
          bound_session_callback_errors.load () == 0,
          "Session callbacks must observe only the registry's committed target binding");

    if (command44_order)
        test.require (command44_observed_open_dispatch.load (std::memory_order_acquire),
                      "command 44 must observe restored target dispatch open");

    const auto restored_target = target.native_node ().objects ().find (
      object_kind_t::actor, std::string (actor.actor_id ().value ()));
    const auto target_actor = detail::actor_ref_access_t::make (
      framework::node_rid_t::from_string (target.status ().routing_id ().to_string ()),
      "production.actor", std::string (actor.actor_id ().value ()), actor.object_generation ());
    const auto target_relay = target_gateway.admit_session_relay (
      target_actor, session_owner.status ().routing_id (), bound_session_route->session,
      session_binding.binding_generation,
      continued_inbound ? continued_inbound->inbound_sequence : 0);
    test.require (static_cast<bool> (target_relay),
                  "generic relocation must restore the target gateway binding before Session "
                  "ingress resumes");
    if (result.terminal != relocation_terminal_t::completed)
        std::cerr << "Session seal relocation terminal=" << static_cast<int> (result.terminal)
                  << " reason=" << static_cast<int> (result.reason)
                  << " post_seal_lookup_failure=" << post_seal_lookup_failure << '\n';
    test.require (
      result.terminal == relocation_terminal_t::completed && !result.authority
        && result.target_handoff
        && result.target_handoff->sources == std::vector<object_ref_t>{*bound_source_object}
        && result.target_handoff->target_node_id == target.status ().routing_id ().to_string (),
      "production relocation must complete the source handoff without committing source authority");
    test.require (restored_target == std::optional<object_ref_t>{expected_target}
                    && roots->roots.size () == 1,
                  "production relocation must restore the target and retain one handoff root");
    // Relocation flow §4.6 requires target dispatch to open before command 44;
    // source completion alone does not order independent completion turns (§11).
    test.require (route_owner_observed_target_ready.load (std::memory_order_acquire),
                  "production relocation must restore the target before the one-way Session route");
    test.require (
      committed_binding && committed_binding->actor == expected_target
        && committed_binding->binding_generation == session_binding.binding_generation
        && committed_binding->target_node_generation == target.status ().lifecycle_generation ()
        && committed_binding->owner_lease_generation == target_descriptor.lease_generation,
      "production relocation must commit the exact target Session route");
    test.require (continued_inbound_error == stateful_error_t::none && continued_inbound_completed
                    && continued_inbound->inbound_sequence == 2,
                  "production relocation must continue Session ingress after the route commit");
    test.require (
      source_relocation_gate == relocation_gate_snapshot_t{},
      "cutover submit terminal must release relocation execution permits while post-cutover "
      "retransmission retention remains active");
    source.stop ();
    target.stop ();
    session_owner.stop ();
}

} // namespace m6c_test
