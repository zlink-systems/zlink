/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "metric_test_reader.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <zlink/stream_connector.hpp>
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
void joined_actor_chain (bool send_after_close)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.send_after_close = send_after_close;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::actor_client_t> ();
    if (!send_after_close)
        evidence.joined_sender = &client;
    auto &spots = services.get_required<fw::spot_manager_t> ();
    for (const auto *id : {local_spot, current_spot})
        ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (id), room_type).async ().result ());
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    const auto actor = std::get<fw::actor_create_created_t> (created.value ()).actor;
    ASSERT_TRUE (services.get_required<fw::detail::actor_gateway_runtime_t> ().bind_session_sink (
      actor,
      [owner = evidence_owner] (std::string, fw::stream_codec_t,
                                const zlink::message_t &) -> fw::task_t<void> {
          {
              std::lock_guard lock (owner->mutex);
              ++owner->session_pushes;
          }
          owner->changed.notify_all ();
          co_return;
      }));
    std::shared_ptr<fw::runtime::serial_execution_queue_t> source_queue;
    if (send_after_close) {
        const auto state =
          fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
            .front ()
            ->spot_state;
        const auto key = std::string (actor_type) + ":" + actor_id;
        const auto context =
          state->lane
            .run ([&] {
                return fw::detail::spot_context_access_t::state (
                  state->spot_contexts_by_id.at (std::string (state->actor_spot_ids.at (key))));
            })
            .get ();
        source_queue = context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
    }
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    if (send_after_close) {
        std::unique_lock entered (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (entered, wait_budget,
                                                [&] { return !evidence.completions.empty (); }));
        entered.unlock ();
        const auto state =
          fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
            .front ()
            ->spot_state;
        const auto key = std::string (actor_type) + ":" + actor_id;
        const auto deadline = std::chrono::steady_clock::now () + wait_budget;
        while (state->actor_transfer_coordinator->phase (key)
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        const bool closed = !state->actor_transfer_coordinator->phase (key);
        ASSERT_TRUE (closed);
        source_queue->drain ();
        evidence.joined_sender = &client;
        ASSERT_TRUE (
          client.send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    }
    std::unique_lock lock (evidence.mutex);
    const auto attempted =
      evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.next_join_attempted; });
    evidence.release_replay_post = true;
    evidence.changed.notify_all ();
    ASSERT_TRUE (attempted);
    ASSERT_TRUE (evidence.next_join_error.empty ())
      << evidence.next_join_error << " completions=" << evidence.completions.size ()
      << " pushes=" << evidence.session_pushes;
    ASSERT_TRUE (evidence.changed.wait_for (
      lock, wait_budget,
      [&] { return evidence.completions.size () >= 2 && evidence.session_pushes == 1; }))
      << "completions=" << evidence.completions.size () << " pushes=" << evidence.session_pushes;
    for (const auto &completion : evidence.completions)
        EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (completion));
}

struct held_session_disconnect_probe_t
{
    std::promise<void> entered;
    std::promise<void> packet_returned;
    fw::task_completion_source_t<void> release;
    std::atomic_int final_callbacks{0};
    std::atomic_int destroyed{0};
};

class held_bound_session_t final : public fw::packet_stream_session_t
{
  public:
    held_bound_session_t (fw::actor_manager_t &actors,
                          std::shared_ptr<observations_t> evidence,
                          std::shared_ptr<held_session_disconnect_probe_t> probe) :
        _relay (actors, std::move (evidence)), _probe (std::move (probe))
    {
    }
    ~held_bound_session_t () override { ++_probe->destroyed; }
    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override
    {
        ++_probe->final_callbacks;
        co_return;
    }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const fw::message_t &payload) override
    {
        co_await _relay.on_packet (stream, dispatch, payload);
        _probe->entered.set_value ();
        co_await _probe->release.task ();
        _probe->packet_returned.set_value ();
    }

  private:
    relay_session_t _relay;
    std::shared_ptr<held_session_disconnect_probe_t> _probe;
};

void physical_disconnect_progresses_while_session_handler_waits (bool websocket)
{
    metric_test::provider_t metrics;
    const auto closed_connections = [&] {
        double total = 0;
        for (const auto &metric : metrics.collect ()) {
            if (metric.instrument_descriptor.name_ != "zlink.stream.connections.closed")
                continue;
            for (const auto &point : metric.point_data_attr_) {
                if (opentelemetry::nostd::get<std::string> (point.attributes.at ("close_reason"))
                    != "client_close")
                    continue;
                const auto &sum =
                  opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
                total += opentelemetry::nostd::get<double> (sum.value_);
            }
        }
        return total;
    };
    auto evidence = std::make_shared<observations_t> ();
    evidence->disconnect_release = std::make_shared<fw::task_completion_source_t<void>> ();
    auto probe = std::make_shared<held_session_disconnect_probe_t> ();
    auto entered = probe->entered.get_future ();
    auto packet_returned = probe->packet_returned.get_future ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t owner;
    owner.app.use_metric_provider (metrics.api ());
    configure (owner, evidence, first_rid, store, relocations);
    owner.app.advanced ().services ().add_factory<held_bound_session_t> (
      [evidence, probe] (fw::service_provider_t &provider) {
          return std::make_unique<held_bound_session_t> (
            provider.get_required<fw::actor_manager_t> (), evidence, probe);
      },
      fw::service_lifetime_t::scoped);
    owner.app.add_zlink_framework ()
      .add_stream_node (stream_name)
      .bind (websocket ? "ws://127.0.0.1:0/disconnect" : "tcp://127.0.0.1:0")
      .register_session<held_bound_session_t> ();
    owner.start ();
    ASSERT_TRUE (ready (owner));
    struct release_t
    {
        std::shared_ptr<held_session_disconnect_probe_t> probe;
        std::shared_ptr<observations_t> evidence;
        ~release_t ()
        {
            probe->release.complete (fw::result_t<void>::success ());
            evidence->disconnect_release->complete (fw::result_t<void>::success ());
        }
    } release{probe, evidence};
    auto services = owner.app.advanced ().services ().build_provider ();
    zlink::stream_connector::connector_options_t options;
    options.endpoint = services.get_required<fw::framework_runtime_t> ()
                         .listener_status (fw::listener_kind_t::stream, stream_name)
                         .endpoint;
    // Listener status reports the WebSocket acceptor endpoint; add the configured handshake path.
    if (websocket)
        options.endpoint += "/disconnect";
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    options.reconnect.enabled = false;
    auto client = zlink::stream_connector::connector_factory_t::create (options);
    ASSERT_TRUE (client.connect ());
    client.send (view_request_t{}).submit ();
    ASSERT_EQ (entered.wait_for (wait_budget), std::future_status::ready);
    const auto before_close = closed_connections ();
    ASSERT_TRUE (client.close ());
    const auto physical_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (closed_connections () == before_close
           && std::chrono::steady_clock::now () < physical_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (closed_connections () - before_close, 1);
    std::cout << "physical_close_observed=1 session_final_while_held="
              << probe->final_callbacks.load () << std::endl;
    EXPECT_EQ (probe->final_callbacks.load (), 0);
    {
        std::unique_lock lock (evidence->mutex);
        const bool progressed = evidence->changed.wait_for (
          lock, wait_budget, [&] { return evidence->disconnect_callbacks == 1; });
        std::cout << "actor_disconnect_while_session_held=" << progressed << std::endl;
        EXPECT_TRUE (progressed);
    }
    EXPECT_EQ (probe->final_callbacks.load (), 0);
    probe->release.complete (fw::result_t<void>::success ());
    ASSERT_EQ (packet_returned.wait_for (wait_budget), std::future_status::ready);
    {
        std::unique_lock lock (evidence->mutex);
        ASSERT_TRUE (evidence->changed.wait_for (
          lock, wait_budget, [&] { return evidence->disconnect_callbacks == 1; }));
    }
    EXPECT_EQ (probe->final_callbacks.load (), 0);
    EXPECT_EQ (probe->destroyed.load (), 0);
    auto shutdown = owner.app.shutdown ();
    ASSERT_EQ (owner.app.runtime_state (), fw::framework_runtime_state_t::draining);
    EXPECT_FALSE (shutdown.await_ready ());
    EXPECT_EQ (probe->destroyed.load (), 0);
    std::cout << "cleanup_pending_session_scope_retained=1 shutdown_draining_observed=1"
                 " public_shutdown_pending=1"
              << std::endl;
    evidence->disconnect_release->complete (fw::result_t<void>::success ());
    auto &gateway = services.get_required<fw::detail::actor_gateway_runtime_t> ();
    const auto terminal_deadline = std::chrono::steady_clock::now () + wait_budget;
    while ((probe->final_callbacks.load () != 1 || gateway.actor_bound (actor_id))
           && std::chrono::steady_clock::now () < terminal_deadline)
        std::this_thread::yield ();
    EXPECT_EQ (probe->final_callbacks.load (), 1);
    EXPECT_FALSE (gateway.actor_bound (actor_id));
    const auto stopped = shutdown.result_for (wait_budget);
    ASSERT_TRUE (stopped && *stopped);
    EXPECT_EQ (stopped->value ().outcome, fw::termination_outcome_t::stopped);
    EXPECT_EQ (stopped->value ().reason, fw::termination_reason_t::none);
    owner.thread.join ();
    EXPECT_EQ (evidence->disconnect_callbacks, 1);
    EXPECT_EQ (probe->final_callbacks.load (), 1);
    EXPECT_EQ (probe->destroyed.load (), 1);
    std::cout << "released_positive_control=1 actor_disconnect_exactly_once="
              << evidence->disconnect_callbacks << std::endl;
}

TEST (BoundSessionDisconnect, TcpCleanupProgressesWhileSessionHandlerWaits)
{
    physical_disconnect_progresses_while_session_handler_waits (false);
}
TEST (BoundSessionDisconnect, WebSocketCleanupProgressesWhileSessionHandlerWaits)
{
    physical_disconnect_progresses_while_session_handler_waits (true);
}

void actor_handler_closes_physical_session (bool remote, bool join)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t owner, session;
    configure (owner, evidence_owner, first_rid, store, relocations);
    if (!remote) {
        owner.app.advanced ().services ().add_factory<relay_session_t> (
          [evidence_owner] (fw::service_provider_t &provider) {
              return std::make_unique<relay_session_t> (
                provider.get_required<fw::actor_manager_t> (), evidence_owner);
          },
          fw::service_lifetime_t::scoped);
        owner.app.add_zlink_framework ()
          .add_stream_node (stream_name)
          .bind ("tcp://127.0.0.1:0")
          .register_session<relay_session_t> ();
    }
    owner.start ();
    ASSERT_TRUE (ready (owner));
    auto services = owner.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    ASSERT_TRUE (actors.get_or_create (fw::actor_id_t (actor_id), actor_type).async ().result ());
    host_t *session_owner = &owner;
    if (remote) {
        configure_session (session, evidence_owner, store, relocations);
        session.start ();
        ASSERT_TRUE (ready (session));
        session_owner = &session;
    }
    auto session_services = session_owner->app.advanced ().services ().build_provider ();
    if (remote) {
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
        ASSERT_TRUE (peer_ready (session_services.get_required<fw::route_mesh_runtime_t> ()));
    }
    view_client_t client (session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint);
    ASSERT_TRUE (client.connect ());
    const auto initial_view = client.view ();
    ASSERT_TRUE (initial_view.has_value ());
    EXPECT_EQ (client.view (), initial_view);
    auto &sender = services.get_required<fw::actor_client_t> ();
    if (join) {
        ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                       .get_or_create (fw::spot_id_t (local_spot), room_type)
                       .async ()
                       .result ());
        ASSERT_TRUE (move_to (sender, evidence, local_spot, 1));
    }
    const auto closed =
      sender.request (fw::actor_id_t (actor_id), close_session_t{37}).async<probe_t> ().result ();
    ASSERT_TRUE (closed) << (closed.error () ? closed.error ()->what () : "close request failed");
    EXPECT_EQ (37, closed.value ().value);
    EXPECT_TRUE (client.wait_closed ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.disconnect_callbacks == 1; }));
        EXPECT_EQ (1, evidence.disconnect_callbacks);
    }
    auto &gateway = services.get_required<fw::detail::actor_gateway_runtime_t> ();
    const auto cleanup_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (gateway.actor_bound (actor_id) && std::chrono::steady_clock::now () < cleanup_deadline)
        std::this_thread::yield ();
    ASSERT_FALSE (gateway.actor_bound (actor_id));
    const auto absent =
      sender.request (fw::actor_id_t (actor_id), close_session_t{37}).async<probe_t> ().result ();
    EXPECT_FALSE (absent);
    EXPECT_EQ (fw::framework_error_kind_t::invalid_operation, absent.error_kind ());
}
TEST (BoundSessionClose, EntryLocal)
{
    actor_handler_closes_physical_session (false, false);
}
TEST (BoundSessionClose, UserLocal)
{
    actor_handler_closes_physical_session (false, true);
}
TEST (BoundSessionClose, EntryRemote)
{
    actor_handler_closes_physical_session (true, false);
}
TEST (BoundSessionClose, UserRemote)
{
    actor_handler_closes_physical_session (true, true);
}

TEST (ActorMaintenanceJoin, JoinedActorSendCanDeferNextJoin)
{
    joined_actor_chain (false);
}
TEST (ActorMaintenanceJoin, AssignmentAfterMoveClosesReachesSession)
{
    joined_actor_chain (true);
}
} // namespace zlink::framework::test::actor_maintenance_join
