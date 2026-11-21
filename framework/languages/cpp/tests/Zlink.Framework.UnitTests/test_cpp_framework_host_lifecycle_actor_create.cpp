/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_remote_create_spot.hpp"
#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/spots/spot_runtime.hpp"

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

namespace zlink::framework::test::host_lifecycle
{
std::vector<std::byte> bytes_from_hex (std::string_view value)
{
    const auto digit = [] (char character) -> unsigned char {
        return character <= '9' ? static_cast<unsigned char> (character - '0')
                                : static_cast<unsigned char> (character - 'a' + 10);
    };
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (
          static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

void configure_remote_actor_create_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<observing_actor_creation_store_t> &location_store,
  std::string routing_id,
  bool actor_target)
{
    app.add_zlink_framework ([location_store, routing_id = std::move (routing_id),
                              actor_target] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (location_store);
        options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
        auto mesh = options.add_route_mesh ("host-remote-actor-create-mesh");
        mesh.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (routing_id))
          .listen ("tcp://127.0.0.1:0");
        if (actor_target) {
            mesh
              .add_entry_spot<remote_create_entry_spot_t> (
                [] (zlink::framework::entry_spot_context_t context) {
                    return std::make_shared<remote_create_entry_spot_t> (std::move (context));
                })
              .add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                "remote-create-actor", std::make_shared<configuration_actor_factory_t> (),
                [] (auto &factory) { factory.disable_relocation (); });
        }
    });
}

bool verify_remote_actor_create_target_owns_completion ()
{
    configuration_actor_factory_t::create_count.store (0, std::memory_order_relaxed);
    remote_create_entry_spot_t::created_count.store (0, std::memory_order_release);
    remote_create_entry_spot_t::joined_count.store (0, std::memory_order_release);
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto target_location_store =
      std::make_shared<observing_actor_creation_store_t> (location_store);
    auto source_location_store =
      std::make_shared<observing_actor_creation_store_t> (location_store);

    target_location_store->republish_descriptor.store (true, std::memory_order_release);

    auto target = zlink::framework::app_t::create ();
    configure_remote_actor_create_app (target, target_location_store, "host-remote-create-target",
                                       true);
    char target_program[] = "host-remote-create-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (3))) {
        target.request_stop ();
        target_thread.join ();
        std::cerr << "remote Actor create target must reach Serving\n";
        return false;
    }

    auto source = zlink::framework::app_t::create ();
    configure_remote_actor_create_app (source, source_location_store, "host-remote-create-source",
                                       false);
    char source_program[] = "host-remote-create-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until ([&] { return source.is_ready (); }, std::chrono::seconds (3))) {
        source.request_stop ();
        target.request_stop ();
        source_thread.join ();
        target_thread.join ();
        std::cerr << "remote Actor create source must reach Serving\n";
        return false;
    }

    auto source_services = source.advanced ().services ().build_provider ();
    auto &routes = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    const auto route_ready = wait_until (
      [&] { return routes.snapshot ("host-remote-actor-create-mesh").ready_peer_count == 1; },
      std::chrono::seconds (5));

    auto &actors = source_services.get_required<zlink::framework::actor_manager_t> ();
    const auto created =
      actors.get_or_create (zlink::framework::actor_id_t ("actor-canonical"), "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    const bool descriptor_rebuilt_once =
      target_location_store->descriptor_conflicts.load (std::memory_order_relaxed) == 1
      && configuration_actor_factory_t::create_count.load (std::memory_order_relaxed) == 1
      && remote_create_entry_spot_t::created_count.load (std::memory_order_acquire) == 1;
    auto &location_repository =
      source_services.get_required<zlink::framework::location_repository_t> ();
    const auto authority =
      location_repository
        .read_authority (zlink::framework::runtime::actor_authority_key ("actor-canonical"))
        .result ()
        .value ();
    const auto *authority_snapshot =
      std::get_if<zlink::framework::authority_snapshot_t> (&authority);
    const bool authority_active =
      authority_snapshot
      && authority_snapshot->allocation.state
           == zlink::framework::placement_allocation_state_t::active
      && authority_snapshot->allocation.target.node_rid.value () == "host-remote-create-target"
      && !authority_snapshot->pending_creation;
    // The requester probes once before reserving; another terminal read would
    // mean it tried to complete the target-owned reservation after the reply.
    const auto source_terminal_reads =
      source_location_store->terminal_reads.load (std::memory_order_acquire);
    const auto node_terminal =
      bytes_from_hex ("01000000250000000000000000010200180f6163746f722d63616e6f6e6963616c"
                      "000000000000000100");
    bool cpp_schema_encoded = false;
    {
        std::lock_guard lock (target_location_store->terminal_mutex);
        cpp_schema_encoded = target_location_store->published_terminal == node_terminal;
    }
    source_location_store->terminal_override = node_terminal;
    const auto node_terminal_replay =
      actors.get_or_create (zlink::framework::actor_id_t ("actor-canonical"), "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    const auto *node_created =
      node_terminal_replay
        ? std::get_if<zlink::framework::actor_create_created_t> (&node_terminal_replay.value ())
        : nullptr;
    const bool node_schema_decoded =
      node_created && node_created->actor.actor_id ().value () == "actor-canonical"
      && node_created->actor.object_generation () == 1;

    source_location_store->terminal_override.reset ();
    target_location_store->hide_terminal_reads.store (true, std::memory_order_release);
    target_location_store->force_terminal_write_conflict.store (true, std::memory_order_release);
    const auto exceptional_terminal_reads_before =
      source_location_store->terminal_reads.load (std::memory_order_acquire);
    const auto exceptional_reply_replay =
      actors
        .get_or_create (zlink::framework::actor_id_t ("actor-exception-replay"),
                        "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    const auto *exception_created =
      exceptional_reply_replay
        ? std::get_if<zlink::framework::actor_create_created_t> (&exceptional_reply_replay.value ())
        : nullptr;
    const bool exceptional_reply_replayed =
      exception_created && exception_created->actor.actor_id ().value () == "actor-exception-replay"
      && exception_created->actor.object_generation () == 2
      && source_location_store->terminal_reads.load (std::memory_order_acquire)
           == exceptional_terminal_reads_before + 2;

    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    source_thread.join ();
    target_thread.join ();

    const bool passed =
      descriptor_rebuilt_once && route_ready && created && authority_active && cpp_schema_encoded
      && node_schema_decoded && exceptional_reply_replayed && source_terminal_reads == 1
      && std::holds_alternative<zlink::framework::actor_create_created_t> (created.value ())
      && remote_create_entry_spot_t::created_count.load (std::memory_order_acquire) == 2
      && remote_create_entry_spot_t::joined_count.load (std::memory_order_acquire) == 0
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_exit_code == 0 && target_exit_code == 0;
    if (!passed) {
        std::cerr << "remote Actor create target must publish the terminal before replying: "
                     "route-ready="
                  << route_ready
                  << " created-callback=" << remote_create_entry_spot_t::created_count.load ()
                  << " joined-callback=" << remote_create_entry_spot_t::joined_count.load ()
                  << " created=" << static_cast<bool> (created)
                  << " authority-active=" << authority_active
                  << " cpp-schema-encoded=" << cpp_schema_encoded
                  << " node-schema-decoded=" << node_schema_decoded
                  << " exceptional-reply-replayed=" << exceptional_reply_replayed
                  << " exceptional-result=" << static_cast<bool> (exceptional_reply_replay)
                  << " exceptional-actor="
                  << (exception_created ? exception_created->actor.actor_id ().value () : "-")
                  << " exceptional-generation="
                  << (exception_created ? exception_created->actor.object_generation () : 0)
                  << " exceptional-error="
                  << (exceptional_reply_replay.error () ? exceptional_reply_replay.error ()->what ()
                                                        : "-")
                  << " source-terminal-reads=" << source_terminal_reads
                  << " error=" << (created.error () ? created.error ()->what () : "-") << '\n';
    }
    return passed;
}

/* Actor model §5 and Spot-Actor membership §2: a Created result publishes the
 * Ready authority and message admission together, so the first request to the
 * created Actor is accepted. The caller starts before the Actor node, waits for
 * the public ready peer as the perf ActorCaller does, creates, and requests at
 * once (#1385). */
/* Actor model §5 and Spot-Actor membership §2: a Created result publishes the
 * Ready authority and message admission together, so the first request to the
 * created Actor is accepted. The caller starts before the Actor node, waits for
 * the public ready peer as the perf ActorCaller does, creates, and requests at
 * once (#1385). */
bool verify_first_request_after_remote_actor_create (int iterations)
{
    for (int iteration = 0; iteration < iterations; ++iteration) {
        auto location_store =
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
        auto target_location_store =
          std::make_shared<observing_actor_creation_store_t> (location_store);
        auto source_location_store =
          std::make_shared<observing_actor_creation_store_t> (location_store);

        auto source = zlink::framework::app_t::create ();
        configure_remote_actor_create_app (source, source_location_store,
                                           "host-first-request-source", false);
        char source_program[] = "host-first-request-source";
        char *source_arguments[] = {source_program, nullptr};
        int source_exit_code = -1;
        std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
        const bool source_ready =
          wait_until ([&] { return source.is_ready (); }, std::chrono::seconds (3));

        auto target = zlink::framework::app_t::create ();
        configure_remote_actor_create_app (target, target_location_store,
                                           "host-first-request-target", true);
        char target_program[] = "host-first-request-target";
        char *target_arguments[] = {target_program, nullptr};
        int target_exit_code = -1;
        std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });

        bool route_ready = false;
        std::string created_error = "-";
        std::string request_error = "-";
        bool created = false;
        bool replied = false;
        if (source_ready) {
            auto source_services = source.advanced ().services ().build_provider ();
            auto &routes = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
            route_ready = wait_until (
              [&] {
                  return routes.snapshot ("host-remote-actor-create-mesh").ready_peer_count == 1;
              },
              std::chrono::seconds (5));
            const auto actor_id = "actor-first-request-" + std::to_string (iteration);
            if (route_ready) {
                auto &actors = source_services.get_required<zlink::framework::actor_manager_t> ();
                const auto result =
                  actors
                    .get_or_create (zlink::framework::actor_id_t (actor_id), "remote-create-actor")
                    .timeout (std::chrono::seconds (5))
                    .async ()
                    .result ();
                created = result
                          && std::holds_alternative<zlink::framework::actor_create_created_t> (
                            result.value ());
                if (!result && result.error ())
                    created_error = result.error ()->what ();
            }
            if (created) {
                auto &client = source_services.get_required<zlink::framework::actor_client_t> ();
                const auto reply = client
                                     .request (zlink::framework::actor_id_t (actor_id),
                                               degraded_object_message_t{iteration})
                                     .timeout (std::chrono::seconds (5))
                                     .async<degraded_object_message_t> ()
                                     .result ();
                replied = reply && reply.value ().value == iteration + 1;
                if (!reply && reply.error ())
                    request_error = reply.error ()->what ();
            }
        }

        source.request_stop ();
        target.request_stop ();
        source_thread.join ();
        target_thread.join ();
        if (!source_ready || !route_ready || !created || !replied) {
            std::cerr << "first request after remote Actor create must be accepted: iteration="
                      << iteration << " source-ready=" << source_ready
                      << " route-ready=" << route_ready << " created=" << created
                      << " create-error=" << created_error << " replied=" << replied
                      << " request-error=" << request_error << '\n';
            return false;
        }
    }
    return true;
}
}
