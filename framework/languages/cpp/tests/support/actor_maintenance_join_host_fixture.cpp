/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_host_fixture.hpp"
#include "actor_maintenance_join_actors.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>

namespace zlink::framework::test::actor_maintenance_join
{
class other_actor_t final : public fw::actor_t
{
  public:
    explicit other_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};
class other_factory_t final : public fw::actor_factory_t<other_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<other_actor_t>> create (fw::actor_context_t context,
                                                       std::stop_token) override
    {
        co_return std::make_shared<other_actor_t> (std::move (context));
    }
};
class other_entry_t final : public fw::entry_spot_t<other_actor_t>
{
  public:
    explicit other_entry_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    fw::task_t<void> on_actor_joined (other_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (other_actor_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};
class room_state_t final : public fw::spot_relocation_adapter_t<room_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (room_t &, std::stop_token) override
    {
        EXPECT_TRUE (fw::detail::capture_current_serial_turn ());
        co_return std::vector<std::byte>{std::byte{1}};
    }
    fw::task_t<void> restore (room_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};

release_callbacks_t::~release_callbacks_t ()
{
    {
        std::lock_guard lock (e.mutex);
        e.release_replay_post = true;
        e.release_replay_prefix = true;
    }
    e.changed.notify_all ();
    e.joined_send_captured.complete (fw::result_t<void>::success ());
    e.release_leave.complete (fw::result_t<void>::success ());
    e.release_join_admission.complete (fw::result_t<void>::success ());
    e.release_completion.complete (fw::result_t<void>::success ());
    if (e.native_handoff_hold)
        e.native_handoff_hold->release.complete (fw::result_t<void>::success ());
}

host_t::~host_t ()
{
    app.request_stop ();
    if (thread.joinable ())
        thread.join ();
    for (const auto &registration :
         fw::detail::mesh_node_runtime_t::registrations (app.advanced ().zlink ()))
        registration->spot_state->handler_invocation_for_tests = {};
}

void host_t::start ()
{
    thread = std::thread ([this] {
        char name[] = "maintenance-join";
        char *argv[]{name, nullptr};
        app.run (1, argv);
    });
}

void observe_joined_batch_admission (observations_t &evidence,
                                     const fw::message_flow_event_t &event)
{
    if (event.actor_id != actor_id
        || (event.packet_name != move_t::packet_name && event.packet_name != probe_t::packet_name))
        return;
    const bool native_admitted =
      event.outcome == fw::message_flow_outcome_t::admitted
      && event.detail_stage == "canonical_application.commit_accepted_ingress";
    const bool retained = event.detail_stage == "relay_actor_packet.try_append_backlog"
                          && event.detail_result == "result=appended";
    std::function<void ()> complete;
    {
        std::lock_guard lock (evidence.mutex);
        if (retained)
            ++evidence.backlog_appends;
        if (native_admitted && evidence.joined_admission_completion
            && --evidence.joined_admission_completion->remaining == 0) {
            complete = std::move (evidence.joined_admission_completion->complete);
            evidence.joined_admission_completion.reset ();
        }
    }
    if (complete)
        complete ();
}

void configure (host_t &host,
                const std::shared_ptr<observations_t> &observations,
                const std::string &rid,
                std::shared_ptr<fw::location_store_t> store,
                std::shared_ptr<fw::relocation_store_t> relocations,
                std::chrono::milliseconds follow_duration,
                int spot_limit,
                bool application_signaled,
                entry_kind_t entry,
                std::optional<int> placement_weight)
{
    auto &options = host.app.add_zlink_framework ();
    if (follow_duration != default_follow_duration) {
        options.configure_locations ().route_cache_max_age = 0ms;
        options.set_message_follow_duration (follow_duration);
    }
    host.app.logging ().use_file (rid + ".flow").set_min_level (fw::log_level_t::debug);
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      options.configure_dispatch (), [obs = observations] (const fw::message_flow_event_t &event) {
          observe_joined_batch_admission (*obs, event);
          const auto hold = obs->native_handoff_hold;
          if (hold && event.actor_id == actor_id && event.spot_id == suffix_spot
              && event.outcome == fw::message_flow_outcome_t::sent
              && event.message_kind == fw::dispatch_message_kind_t::actor_request
              && !event.packet_name) {
              hold->next_join_started.complete (fw::result_t<void>::success ());
              obs->changed.notify_all ();
          }
          if (hold && event.actor_id == actor_id && event.packet_name == probe_t::packet_name) {
              if (event.spot_id == local_spot
                  && event.detail_stage == "invoke_erased.application_handler"
                  && event.detail_result == "result=finished success=true") {
                  hold->preceding_h_finished.complete (fw::result_t<void>::success ());
                  obs->changed.notify_all ();
              }
          }
          {
              std::lock_guard lock (obs->mutex);
              if (event.packet_name == probe_t::packet_name && event.actor_id == actor_id
                  && event.message_kind == fw::dispatch_message_kind_t::actor_request
                  && event.outcome == fw::message_flow_outcome_t::admitted
                  && event.detail_stage == "canonical_application.commit_accepted_ingress")
                  ++obs->ordinary_posts;
              if (event.packet_name == "handoff_replay_failed")
                  obs->replay_failures.push_back (event);
          }
          obs->changed.notify_all ();
      });
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (store);
    options.add_relocation_store (relocations);
    auto mesh = options.add_route_mesh (mesh_name);
    mesh.set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from (rid))
      .listen ("tcp://127.0.0.1:0");
    if (placement_weight)
        mesh.set_placement_weight (*placement_weight);
    if (entry == entry_kind_t::member)
        mesh.add_entry_spot<entry_t> ([evidence = observations] (fw::entry_spot_context_t context) {
            return std::make_shared<entry_t> (std::move (context), evidence);
        });
    if (entry == entry_kind_t::other)
        mesh
          .add_entry_spot<other_entry_t> ([] (fw::entry_spot_context_t context) {
              return std::make_shared<other_entry_t> (std::move (context));
          })
          .add_actor_factory<other_actor_t, other_factory_t> (
            other_actor_type, std::make_shared<other_factory_t> (),
            [] (auto &factory) { factory.recreate_on_relocation (); });
    mesh
      .add_spot_factory<room_t> (
        room_type,
        [evidence = observations] (fw::spot_context_t context) {
            {
                std::lock_guard lock (evidence->mutex);
                ++evidence->room_factories;
            }
            return std::make_shared<room_t> (std::move (context), evidence);
        },
        [spot_limit, application_signaled] (auto &factory) {
            if (application_signaled) {
                factory.set_relocation_coordination_mode (
                  fw::spot_relocation_coordination_mode_t::application_signaled);
                factory.template preserve_state_with<room_state_t> ();
            } else
                factory.disable_relocation ();
            if (spot_limit > 0)
                factory.set_stable_type_limit (spot_limit);
        })
      .add_actor_factory<actor_t, factory_t> (
        actor_type, std::make_shared<factory_t> (observations),
        [] (auto &factory) { factory.recreate_on_relocation (); });
    for (const auto &registration :
         fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()))
        registration->spot_state->handler_invocation_for_tests =
          [obs = observations] (fw::detail::handler_invocation_phase_t phase,
                                std::string_view packet, std::string_view spot,
                                std::string_view actor) {
              if (actor != actor_id)
                  return;
              if (phase == fw::detail::handler_invocation_phase_t::before_admission) {
                  const auto hold = obs->native_handoff_hold;
                  if (hold && packet == probe_t::packet_name) {
                      bool entered;
                      {
                          std::lock_guard lock (obs->mutex);
                          entered = ++hold->entries == 2;
                      }
                      if (entered) {
                          hold->entered.complete (fw::result_t<void>::success ());
                          obs->changed.notify_all ();
                          hold->release.task ().result ().value ();
                      }
                  }
                  if (obs->hold_replay_prefix && spot == local_spot
                      && packet == probe_t::packet_name) {
                      std::unique_lock lock (obs->mutex);
                      if (!obs->replay_prefix_entered) {
                          obs->replay_prefix_entered = true;
                          obs->changed.notify_all ();
                          obs->changed.wait (lock, [obs] { return obs->release_replay_prefix; });
                      }
                  }
              } else if (obs->joined_sender && !obs->send_after_close
                         && packet == move_t::packet_name && spot == local_spot) {
                  std::unique_lock lock (obs->mutex);
                  obs->changed.wait (lock, [obs] { return obs->release_replay_post; });
              }
          };
}

bool ready (host_t &host)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!host.app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return host.app.is_ready ();
}

bool move_to (fw::actor_client_t &sender,
              observations_t &evidence,
              std::string target,
              std::size_t count)
{
    auto sent =
      sender.send (fw::actor_id_t (actor_id), move_t{std::move (target)}).async ().result ();
    if (!sent) {
        std::fprintf (stderr, "maintenance move send failed: kind=%d error=%s\n",
                      static_cast<int> (sent.error_kind ()),
                      sent.error () ? sent.error ()->what () : "no error");
        return false;
    }
    std::unique_lock lock (evidence.mutex);
    return evidence.changed.wait_for (lock, wait_budget,
                                      [&] { return evidence.completions.size () >= count; });
}

bool peer_ready (fw::route_mesh_runtime_t &routes, std::string_view peer_rid, std::size_t count)
{
    const auto ready = [&] {
        const auto snapshot = routes.snapshot (mesh_name);
        return snapshot.ready_peer_count >= count
               && std::any_of (
                 snapshot.peers.begin (), snapshot.peers.end (), [&] (const auto &peer) {
                     return peer.state == fw::peer_state_t::ready
                            && (peer_rid.empty () || peer.node_rid.to_string () == peer_rid);
                 });
    };
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return ready ();
}
} // namespace zlink::framework::test::actor_maintenance_join
