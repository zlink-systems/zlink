/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_remote_create_spot.hpp"
#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../support/host_lifecycle_actor_fixture.hpp"

#include "../support/host_lifecycle_wait.hpp"

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static void erase_actor_reference (public_host_runtime_t &host, const std::string &id)
    {
        host._spot_actor_index_lane.run ([&] { host._actors.erase (id); }).get ();
    }
};
}

namespace zlink::framework::test::host_lifecycle
{
class relocated_member_spot_t final : public zlink::framework::spot_t<configuration_actor_t>
{
  public:
    explicit relocated_member_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&relocated_member_spot_t::on_ready> ();
        _context.handlers ().add_handler<&relocated_member_spot_t::on_spot_echo> ();
        _context.handlers ().add_actor_request<&relocated_member_spot_t::on_echo> ();
    }
    void on_ready (const relocation_ready_message_t &) { _context.relocation_ready ().defer (); }
    degraded_object_message_t on_spot_echo (const degraded_object_message_t &request)
    {
        if (_context.node_rid ().value () == "relocated-source")
            ++source_requests;
        else
            ++target_requests;
        return {request.value + 1};
    }
    static inline std::atomic_int source_requests{0};
    static inline std::atomic_int target_requests{0};
    degraded_object_message_t on_echo (configuration_actor_t &,
                                       zlink::framework::message_context_t &,
                                       const degraded_object_message_t &request)
    {
        return {request.value + 1};
    }
    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }
    zlink::framework::task_t<void> on_initialize () override { co_return; }
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void> on_actor_joined (configuration_actor_t &) override { co_return; }
    zlink::framework::task_t<void> on_leave_actor (configuration_actor_t &) override { co_return; }

  private:
    zlink::framework::spot_context_t _context;
};

bool verify_relocated_actor_follow_and_shutdown (bool aggregate, bool missing_source)
{
    using namespace zlink::framework;
    auto locations = std::make_shared<observing_actor_creation_store_t> (
      std::make_shared<runtime::in_memory_location_store_t> ());
    auto relocations = std::make_shared<runtime::in_memory_relocation_store_t> ();
    auto configure = [&] (app_t &app, std::string rid) {
        app.add_zlink_framework ([&, rid, host = &app] (zlink_framework_options_t &options) {
            options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
            detail::dispatch_options_access_t::set_logger (
              options.configure_dispatch (),
              host->logging ().factory ().create ("relocation.flow"));
            options.configure_locations ().owner_lease_renew_interval =
              std::chrono::milliseconds (10);
            options.configure_locations ().owner_lease_fencing_margin =
              std::chrono::milliseconds (1);
            options.add_location_store (locations);
            options.add_relocation_store (relocations);
            options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
            auto mesh = options.add_route_mesh ("relocated-actor-shutdown");
            mesh.set_object_role (object_role_t::server)
              .set_routing_id (zlink::routing_id_t::from (rid))
              .listen ("tcp://127.0.0.1:0")
              .add_entry_spot<remote_create_entry_spot_t> ([] (entry_spot_context_t context) {
                  return std::make_shared<remote_create_entry_spot_t> (std::move (context));
              })
              .add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                "remote-create-actor", std::make_shared<configuration_actor_factory_t> (),
                [] (auto &factory) { factory.recreate_on_relocation (); });
            mesh.add_spot_factory<relocated_member_spot_t> (
              "relocated-room",
              [] (spot_context_t context) {
                  return std::make_shared<relocated_member_spot_t> (std::move (context));
              },
              [] (auto &factory) {
                  factory.set_execution_mode (user_spot_execution_mode_t::spot_wide);
                  factory.set_relocation_coordination_mode (
                    spot_relocation_coordination_mode_t::application_signaled);
                  factory.recreate_on_relocation ();
              });
        });
    };
    auto source = app_t::create ();
    auto target = app_t::create ();
    std::atomic_bool removed{false};
    locations->on_read = [&] (const store_key_t &key) {
        if (!missing_source || source.runtime_state () != framework_runtime_state_t::relocating
            || key.value.find (std::string ("authority") + '\0' + "actor" + '\0'
                               + "relocated-actor")
                 == std::string::npos
            || removed.exchange (true))
            return;
        auto node_runtime = detail::spot_node_runtime_t::from (source.advanced ().zlink (),
                                                               "relocated-actor-shutdown");
        runtime::host::public_host_runtime_test_access_t::erase_actor_reference (
          *node_runtime->native_node (), "relocated-actor");
    };
    configure (source, "relocated-source");
    configure (target, "relocated-target");
    source.set_message_flow_mode (message_flow_log_mode_t::normal);
    target.set_message_flow_mode (message_flow_log_mode_t::normal);
    source.logging ()
      .use_file ((std::filesystem::temp_directory_path () / "relocated-source.flow").string ())
      .set_min_level (log_level_t::debug);
    target.logging ()
      .use_file ((std::filesystem::temp_directory_path () / "relocated-target.flow").string ())
      .set_min_level (log_level_t::debug);
    char program[] = "relocated-actor-shutdown";
    char *arguments[] = {program, nullptr};
    std::thread source_thread ([&] { source.run (1, arguments); });
    const bool source_ready =
      wait_until ([&] { return source.is_ready (); }, std::chrono::seconds (5));
    auto services = source.advanced ().services ().build_provider ();
    auto &actors = services.get_required<actor_manager_t> ();
    const auto created =
      actors.get_or_create (actor_id_t ("relocated-actor"), "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    auto &client = services.get_required<actor_client_t> ();
    bool joined = !aggregate;
    auto routes =
      source.advanced ().zlink ().route_client (services.get_required<serializer_registry_t> ());
    if (aggregate) {
        const auto second =
          actors.get_or_create (actor_id_t ("relocated-standalone"), "remote-create-actor")
            .timeout (std::chrono::seconds (5))
            .async ()
            .result ();
        const auto room = services.get_required<spot_manager_t> ()
                            .get_or_create (spot_id_t ("relocated-room"), "relocated-room")
                            .timeout (std::chrono::seconds (5))
                            .async ()
                            .result ();
        const auto submitted =
          client.send (actor_id_t ("relocated-actor"), relocation_ready_message_t{1})
            .async ()
            .result ();
        joined = second && room && submitted
                 && wait_until (
                   [&] {
                       const auto membership =
                         actors.find_spot (actor_id_t ("relocated-actor")).result ();
                       return membership && membership.value ()
                              && membership.value ()->spot_id () == spot_id_t ("relocated-room");
                   },
                   std::chrono::seconds (5));
    }
    const auto before =
      client.request (actor_id_t ("relocated-actor"), degraded_object_message_t{1})
        .timeout (std::chrono::seconds (5))
        .async<degraded_object_message_t> ()
        .result ();
    bool spot_before = !aggregate;
    if (aggregate) {
        const auto reply =
          routes.request_to_spot (spot_id_t ("relocated-room"), degraded_object_message_t{1})
            .timeout (std::chrono::seconds (5))
            .async<degraded_object_message_t> ()
            .result ();
        spot_before = reply && reply.value ().value == 2;
    }
    std::thread target_thread ([&] { target.run (1, arguments); });
    const bool target_ready =
      wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (5));
    auto relocating = source.relocate ({.mode = relocation_mode_t::planned_maintenance});
    if (aggregate && joined
        && wait_until (
          [&] { return source.runtime_state () == framework_runtime_state_t::relocating; },
          std::chrono::seconds (5))) {
        (void) routes.send_to_spot (spot_id_t ("relocated-room"), relocation_ready_message_t{1})
          .async ()
          .result ();
    }
    const auto moved = relocating.result ().value ();
    const auto live_after_move = configuration_actor_t::live_count.load ();
    bool spot_after = !aggregate;
    if (aggregate) {
        const auto reply =
          routes.request_to_spot (spot_id_t ("relocated-room"), degraded_object_message_t{2})
            .timeout (std::chrono::seconds (5))
            .async<degraded_object_message_t> ()
            .result ();
        spot_after = reply && reply.value ().value == 3
                     && relocated_member_spot_t::source_requests.load () == 1
                     && relocated_member_spot_t::target_requests.load () == 1;
        if (!reply && reply.error ())
            std::cerr << "Spot Message Follow request failed: " << reply.error ()->what () << '\n';
    }
    if (missing_source) {
        source.shutdown (std::chrono::seconds (5)).result ().value ();
        target.shutdown (std::chrono::seconds (5)).result ().value ();
        source_thread.join ();
        target_thread.join ();
        std::cout << "missing source: removed=" << removed.load ()
                  << " reason=" << static_cast<int> (moved.reason) << '\n';
        return removed && moved.outcome == relocation_outcome_t::blocked
               && moved.reason == relocation_reason_t::state_incompatible;
    }
    const auto after = client.request (actor_id_t ("relocated-actor"), degraded_object_message_t{2})
                         .timeout (std::chrono::seconds (5))
                         .async<degraded_object_message_t> ()
                         .result ();
    const auto source_stop = source.shutdown (std::chrono::seconds (5)).result ().value ();
    const auto target_stop = target.shutdown (std::chrono::seconds (5)).result ().value ();
    source_thread.join ();
    target_thread.join ();
    std::cout << "relocated Actor: source-ready=" << source_ready
              << " target-ready=" << target_ready << " created=" << static_cast<bool> (created)
              << " before=" << static_cast<bool> (before)
              << " relocation=" << static_cast<int> (moved.outcome)
              << " live-after-move=" << live_after_move << " follow=" << static_cast<bool> (after)
              << " joined=" << joined << " spot-before=" << spot_before
              << " spot-after=" << spot_after
              << " source-stop=" << static_cast<int> (source_stop.outcome)
              << " target-stop=" << static_cast<int> (target_stop.outcome) << '\n';
    return source_ready && target_ready && created && joined && spot_before && spot_after && before
           && before.value ().value == 2 && moved.outcome == relocation_outcome_t::relocated
           && live_after_move == (aggregate ? 2 : 1) && after && after.value ().value == 3
           && source_stop.outcome == termination_outcome_t::stopped
           && target_stop.outcome == termination_outcome_t::stopped;
}
}
