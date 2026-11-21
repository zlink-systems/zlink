/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/host_lifecycle_degraded_object_spot.hpp"
#include "../support/host_lifecycle_blocking_stop.hpp"
#include "../support/host_lifecycle_ready_probe.hpp"
#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "../support/relocation_unit_fixture.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/host/relocation_target_eligibility.hpp"
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

#include "../support/host_lifecycle_location_recovery_fixture.hpp"

namespace zlink::framework::test::host_lifecycle
{
using zlink::framework::test::relocation_unit_spot_t;

static bool verify_relocation_blocker (
  std::string_view label,
  std::function<void (zlink::framework::zlink_framework_options_t &)> configure,
  zlink::framework::relocation_reason_t expected,
  std::chrono::milliseconds relocation_deadline = std::chrono::seconds (1),
  std::chrono::milliseconds minimum_wait = std::chrono::milliseconds::zero ())
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework (std::move (configure));
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    app.add_hosted_service (std::move (service));

    char program[] = "termination-topology-preflight";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
    service_view->wait_started ();
    const auto serving_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!app.is_ready () && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto relocation_started_at = std::chrono::steady_clock::now ();
    const auto result =
      app
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = relocation_deadline})
        .result ()
        .value ();
    const auto relocation_elapsed = std::chrono::steady_clock::now () - relocation_started_at;
    const bool matched = result.outcome == zlink::framework::relocation_outcome_t::blocked
                         && result.reason == expected && app.is_ready ()
                         && relocation_elapsed >= minimum_wait;
    if (!matched)
        std::cerr << label << " must block Relocate without changing Serving\n";

    auto shutdown = app.shutdown (std::chrono::seconds (2));
    service_view->wait_stop_entered ();
    service_view->release ();
    const auto stopped = shutdown.result ().value ();
    run_thread.join ();
    return matched && stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && exit_code == 0;
}

bool verify_degraded_host_blocks_and_resumes_relocation ()
{
    auto app = zlink::framework::app_t::create ();
    auto location_store = std::make_shared<recovering_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto &options = app.add_zlink_framework ();
    options.add_location_store (location_store);
    options.add_relocation_store (relocation_store);
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval = std::chrono::milliseconds (10);
    locations.owner_lease_renew_timeout = std::chrono::milliseconds (50);
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.owner_lease_fencing_margin = std::chrono::milliseconds (1);
    locations.polling_interval = std::chrono::milliseconds (5);
    auto degraded_mesh = options.add_route_mesh ("degraded-relocation-host");
    degraded_mesh.channel_name ("degraded-relocation-channel").server ();
    degraded_mesh.set_object_role (zlink::framework::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("degraded-relocation-node"))
      .listen ("inproc://degraded-relocation-node")
      .add_spot_factory<degraded_object_spot_t> (
        "degraded-object",
        [] (zlink::framework::spot_context_t context) {
            return std::make_shared<degraded_object_spot_t> (std::move (context));
        },
        [] (auto &factory) { factory.recreate_on_relocation (); });
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    app.add_hosted_service (std::move (service));

    auto ready_probe = std::make_unique<ready_host_probe_t> ();
    auto *ready_view = ready_probe.get ();
    app.add_hosted_service (std::move (ready_probe));

    char program[] = "degraded-relocation-host";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
    service_view->wait_started ();
    const bool preparing_without_lease =
      !app.is_ready ()
      && app.runtime_state () == zlink::framework::framework_runtime_state_t::preparing;
    const auto blocked =
      app
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::milliseconds (50)})
        .result ()
        .value ();
    const bool relocation_blocked =
      blocked.outcome == zlink::framework::relocation_outcome_t::blocked
      && blocked.reason == zlink::framework::relocation_reason_t::runtime_not_ready;

    location_store->recover ();
    auto services = app.advanced ().services ().build_provider ();
    auto &locations_repository = services.get_required<zlink::framework::location_repository_t> ();
    location_store->wait_for_publication ([&] {
        return !locations_repository.list_mesh_nodes ("degraded-relocation-host")
                  .result ()
                  .value ()
                  .items.empty ();
    });
    const bool recovery_completed = ready_view->wait_ready ();
    const auto resumed =
      app
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::milliseconds (25)})
        .result ()
        .value ();
    // Spec 05-host-relocation-flow §5.1: recovery admits relocation, and this
    // host owns no workload that would require a target.
    const bool relocation_resumed =
      resumed.outcome == zlink::framework::relocation_outcome_t::relocated
      && resumed.reason == zlink::framework::relocation_reason_t::none;
    services.close ();

    auto shutdown = app.shutdown (std::chrono::seconds (1));
    service_view->wait_stop_entered ();
    service_view->release ();
    const auto stopped = shutdown.result ().value ();
    run_thread.join ();
    const bool passed =
      preparing_without_lease && relocation_blocked && recovery_completed && relocation_resumed
      && stopped.outcome == zlink::framework::termination_outcome_t::stopped && exit_code == 0;
    if (!passed) {
        std::cerr << "degraded host relocation admission did not close and resume"
                  << " preparing-without-lease=" << preparing_without_lease
                  << " blocked-reason=" << static_cast<int> (blocked.reason)
                  << " blocked=" << relocation_blocked
                  << " recovery-completed=" << recovery_completed
                  << " resumed=" << relocation_resumed << '\n';
    }
    return passed;
}

void configure_retry_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> &location_store,
  const std::shared_ptr<zlink::framework::runtime::in_memory_relocation_store_t> &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework ([location_store, relocation_store,
                              routing_id = std::move (routing_id)] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (location_store);
        options.add_relocation_store (relocation_store);
        options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
        auto mesh = options.add_route_mesh ("host-relocation-retry-mesh");
        mesh.channel_name ("host-relocation-retry-channel").server ();
        mesh.set_object_role (zlink::framework::object_role_t::server)
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from (routing_id))
          .add_spot_factory<relocation_unit_spot_t> (
            "relocation-unit",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<relocation_unit_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.recreate_on_relocation (); });
    });
}

struct lone_host_relocation_t
{
    bool ready = false;
    bool unit_created = false;
    zlink::framework::relocation_result_t result;
    zlink::framework::framework_runtime_state_t state_after =
      zlink::framework::framework_runtime_state_t::preparing;
    bool stopped = false;
};

/* A host alone in its Mesh relocates with no other node to target. With
 * owns_unit it first creates one User Spot. */
lone_host_relocation_t
relocate_lone_host (std::string routing_id, bool owns_unit, std::chrono::milliseconds deadline)
{
    const std::string mesh_name = "lone-host-relocation-mesh";
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([owns_unit, mesh_name, routing_id = std::move (routing_id)] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (
          std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
        options.add_relocation_store (
          std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ());
        options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.channel_name ("lone-host-relocation-channel").server ();
        mesh.set_object_role (zlink::framework::object_role_t::server)
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from (routing_id));
        if (owns_unit) {
            mesh.add_spot_factory<relocation_unit_spot_t> (
              "relocation-unit",
              [] (zlink::framework::spot_context_t context) {
                  return std::make_shared<relocation_unit_spot_t> (std::move (context));
              },
              [] (auto &factory) { factory.recreate_on_relocation (); });
        }
    });

    auto ready_probe = std::make_unique<ready_host_probe_t> ();
    auto *ready_view = ready_probe.get ();
    app.add_hosted_service (std::move (ready_probe));

    char program[] = "lone-host-relocation";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = app.run (1, arguments); });

    lone_host_relocation_t observed;
    observed.ready = ready_view->wait_ready ();
    if (observed.ready && owns_unit) {
        auto services = app.advanced ().services ().build_provider ();
        observed.unit_created = static_cast<bool> (
          services.get_required<zlink::framework::spot_manager_t> ()
            .get_or_create (zlink::framework::spot_id_t ("relocation-unit"), "relocation-unit")
            .in_mesh (mesh_name)
            .timeout (std::chrono::seconds (1))
            .async ()
            .result ());
        services.close ();
    }
    if (observed.ready && (!owns_unit || observed.unit_created)) {
        observed.result =
          app
            .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = deadline})
            .result ()
            .value ();
        observed.state_after = app.runtime_state ();
    }
    const auto stopped = app.shutdown (std::chrono::seconds (2)).result ().value ();
    run_thread.join ();
    observed.stopped =
      stopped.outcome == zlink::framework::termination_outcome_t::stopped && exit_code == 0;
    return observed;
}

/* Spec 05-host-relocation-flow §5.1 (#1947): a host with no Actor, User Spot or
 * Instance Spot needs no target and ends Relocated/None; a host that owns a
 * unit still needs a target and stays Serving when none exists. */
bool verify_host_relocation_needs_target_only_for_units ()
{
    // A workload-free host completes without waiting for a target.
    const auto empty =
      relocate_lone_host ("lone-empty-host", false, std::chrono::milliseconds (500));
    const bool empty_relocated =
      empty.ready && empty.result.outcome == zlink::framework::relocation_outcome_t::relocated
      && empty.result.reason == zlink::framework::relocation_reason_t::none
      && empty.state_after == zlink::framework::framework_runtime_state_t::relocated
      && empty.stopped;
    if (!empty_relocated) {
        std::cerr << "host with only mesh listeners must relocate without a target: ready="
                  << empty.ready << " outcome=" << static_cast<int> (empty.result.outcome)
                  << " reason=" << static_cast<int> (empty.result.reason)
                  << " state=" << static_cast<int> (empty.state_after)
                  << " stopped=" << empty.stopped << '\n';
    }

    const auto owner = relocate_lone_host ("lone-unit-host", true, std::chrono::milliseconds (1));
    const bool owner_blocked =
      owner.ready && owner.unit_created
      && owner.result.outcome == zlink::framework::relocation_outcome_t::blocked
      && owner.result.reason == zlink::framework::relocation_reason_t::target_unavailable
      && owner.state_after == zlink::framework::framework_runtime_state_t::serving && owner.stopped;
    if (!owner_blocked) {
        std::cerr << "host with a User Spot and no target must stay Blocked: ready=" << owner.ready
                  << " unit=" << owner.unit_created
                  << " outcome=" << static_cast<int> (owner.result.outcome)
                  << " reason=" << static_cast<int> (owner.result.reason)
                  << " state=" << static_cast<int> (owner.state_after)
                  << " stopped=" << owner.stopped << '\n';
    }
    std::cout << "empty-host: relocated=" << empty_relocated
              << " outcome=" << static_cast<int> (empty.result.outcome)
              << " reason=" << static_cast<int> (empty.result.reason)
              << " state=" << static_cast<int> (empty.state_after) << " stopped=" << empty.stopped
              << '\n'
              << "unit-host: blocked=" << owner_blocked << " unit=" << owner.unit_created
              << " outcome=" << static_cast<int> (owner.result.outcome)
              << " reason=" << static_cast<int> (owner.result.reason)
              << " state=" << static_cast<int> (owner.state_after) << " stopped=" << owner.stopped
              << '\n';
    return empty_relocated && owner_blocked;
}

bool verify_host_without_mesh_validates_target_version ()
{
    bool passed = true;
    for (const auto target_version : {-1, 0, 1}) {
        auto app = zlink::framework::app_t::create ();
        // No Store is needed when there are no relocation units (§5.1).
        app.add_zlink_framework ();
        auto probe = std::make_unique<ready_host_probe_t> ();
        auto *ready = probe.get ();
        app.add_hosted_service (std::move (probe));
        char program[] = "no-mesh-relocation-version";
        char *arguments[] = {program, nullptr};
        int exit_code = -1;
        std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
        const bool serving = ready->wait_ready ();
        bool rejected = false;
        bool relocated = false;
        try {
            const auto result =
              app
                .relocate ({.mode = zlink::framework::relocation_mode_t::rolling_update,
                            .target_application_version = target_version,
                            .deadline = std::chrono::seconds (1)})
                .result ()
                .value ();
            relocated = result.outcome == zlink::framework::relocation_outcome_t::relocated
                        && result.reason == zlink::framework::relocation_reason_t::none
                        && result.effective_target_application_version == target_version;
            std::cout << "no-store result: target=" << target_version
                      << " outcome=" << static_cast<int> (result.outcome)
                      << " reason=" << static_cast<int> (result.reason) << '\n';
        }
        catch (const std::invalid_argument &) {
            rejected = true;
        }
        const bool correct_state =
          app.runtime_state ()
          == (target_version <= 0 ? zlink::framework::framework_runtime_state_t::serving
                                  : zlink::framework::framework_runtime_state_t::relocated);
        const auto stopped = app.shutdown (std::chrono::seconds (2)).result ().value ();
        run_thread.join ();
        const bool correct =
          serving && correct_state && (target_version <= 0 ? rejected : relocated)
          && stopped.outcome == zlink::framework::termination_outcome_t::stopped && exit_code == 0;
        std::cout << "no-mesh target=" << target_version << " rejected=" << rejected
                  << " relocated=" << relocated << " correct-state=" << correct_state
                  << " passed=" << correct << '\n';
        passed = correct && passed;
    }
    return passed;
}

bool verify_relocation_retry_after_target_unavailable ()
{
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();

    auto source = zlink::framework::app_t::create ();
    configure_retry_relocation_app (source, location_store, relocation_store,
                                    "host-relocation-retry-source");
    auto source_probe = std::make_unique<ready_host_probe_t> ();
    auto *source_ready = source_probe.get ();
    source.add_hosted_service (std::move (source_probe));
    char source_program[] = "host-relocation-retry-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!source_ready->wait_ready ()) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "relocation retry source must reach Serving\n";
        return false;
    }

    auto services = source.advanced ().services ().build_provider ();
    const auto created =
      services.get_required<zlink::framework::spot_manager_t> ()
        .get_or_create (zlink::framework::spot_id_t ("retry-unit"), "relocation-unit")
        .in_mesh ("host-relocation-retry-mesh")
        .timeout (std::chrono::seconds (1))
        .async ()
        .result ();
    services.close ();
    if (!created) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "relocation retry source must own a User Spot: "
                  << (created.error () ? created.error ()->what () : "creation failed") << '\n';
        return false;
    }

    const auto first =
      source
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::milliseconds (100)})
        .result ()
        .value ();
    if (first.outcome != zlink::framework::relocation_outcome_t::blocked
        || first.reason != zlink::framework::relocation_reason_t::target_unavailable
        || !source.is_ready ()) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "first relocation must return Blocked/TargetUnavailable "
                     "and preserve Serving\n";
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_retry_relocation_app (target, location_store, relocation_store,
                                    "host-relocation-retry-target");
    auto target_probe = std::make_unique<ready_host_probe_t> ();
    auto *target_ready = target_probe.get ();
    target.add_hosted_service (std::move (target_probe));
    char target_program[] = "host-relocation-retry-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!target_ready->wait_ready ()) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        std::cerr << "relocation retry target must reach Serving\n";
        return false;
    }

    const auto second =
      source
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::seconds (3)})
        .result ()
        .value ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    if (second.outcome != zlink::framework::relocation_outcome_t::relocated
        || second.reason != zlink::framework::relocation_reason_t::none) {
        std::cerr << "second relocation must restart preflight and relocate: outcome="
                  << static_cast<int> (second.outcome)
                  << " reason=" << static_cast<int> (second.reason) << '\n';
        return false;
    }
    return target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && target_exit_code == 0 && source_exit_code == 0;
}

bool verify_relocation_target_eligibility_applies_full_narrowing ()
{
    using zlink::framework::framework_runtime_state_t;
    using zlink::framework::maintenance_policy_kind_t;
    using zlink::framework::mesh_node_descriptor_t;
    using zlink::framework::object_capability_t;
    using zlink::framework::object_role_t;
    using zlink::framework::placement_object_kind_t;
    using zlink::framework::relocation_unit_target_eligible;

    mesh_node_descriptor_t source;
    source.application_version = 7;
    source.maintenance_wave = "wave-a";
    source.object_capabilities.push_back (object_capability_t{
      placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::disabled, false, 0});
    source.object_capabilities.push_back (object_capability_t{
      placement_object_kind_t::actor, "player", maintenance_policy_kind_t::disabled, false, 0});

    const auto make_eligible = [] () {
        mesh_node_descriptor_t candidate;
        candidate.state = framework_runtime_state_t::serving;
        candidate.object_role = object_role_t::server;
        candidate.placement_weight = 100;
        candidate.application_version = 7;
        candidate.object_capabilities.push_back (
          object_capability_t{placement_object_kind_t::user_spot, "room",
                              maintenance_policy_kind_t::disabled, false, 0});
        candidate.object_capabilities.push_back (object_capability_t{
          placement_object_kind_t::actor, "player", maintenance_policy_kind_t::disabled, false, 0});
        return candidate;
    };

    if (!relocation_unit_target_eligible (source, make_eligible (), 7, "room", {"player"})) {
        std::cerr << "eligible relocation target was wrongly rejected\n";
        return false;
    }

    // The pre-fix selector predicate checked only serving state, version, and
    // object-type support, so it accepted every candidate below. The frozen
    // 6-step narrowing (cpp interface 02-configuration-host) must reject them.
    struct rejection_case_t
    {
        const char *name;
        std::function<void (mesh_node_descriptor_t &)> mutate;
    };
    const rejection_case_t rejection_cases[] = {
      {"zero placement weight", [] (mesh_node_descriptor_t &c) { c.placement_weight = 0; }},
      {"not an Object Server",
       [] (mesh_node_descriptor_t &c) { c.object_role = object_role_t::none; }},
      {"same maintenance wave as source",
       [] (mesh_node_descriptor_t &c) { c.maintenance_wave = "wave-a"; }},
      {"exhausted actor capacity",
       [] (mesh_node_descriptor_t &c) {
           c.capacity.actors.limit = 1;
           c.capacity.actors.active = 1;
       }},
      {"wrong effective version", [] (mesh_node_descriptor_t &c) { c.application_version = 8; }},
    };
    for (const auto &item : rejection_cases) {
        auto candidate = make_eligible ();
        item.mutate (candidate);
        if (relocation_unit_target_eligible (source, candidate, 7, "room", {"player"})) {
            std::cerr << "ineligible relocation target was accepted: " << item.name << "\n";
            return false;
        }
    }

    // Snapshot relocation policy requires a snapshot adapter on the candidate.
    {
        mesh_node_descriptor_t snapshot_source = source;
        snapshot_source.object_capabilities[0].policy = maintenance_policy_kind_t::snapshot;
        auto candidate = make_eligible ();
        candidate.object_capabilities[0].policy = maintenance_policy_kind_t::snapshot;
        candidate.object_capabilities[0].has_snapshot_adapter = false;
        if (relocation_unit_target_eligible (snapshot_source, candidate, 7, "room", {"player"})) {
            std::cerr << "snapshot candidate without adapter was accepted\n";
            return false;
        }
        candidate.object_capabilities[0].has_snapshot_adapter = true;
        if (!relocation_unit_target_eligible (snapshot_source, candidate, 7, "room", {"player"})) {
            std::cerr << "snapshot candidate with adapter was rejected\n";
            return false;
        }
    }

    // An actor-only unit (empty Spot type) imposes no user Spot requirement but
    // still applies the per-candidate gates.
    {
        auto candidate = make_eligible ();
        if (!relocation_unit_target_eligible (source, candidate, 7, "", {"player"})) {
            std::cerr << "eligible actor-only target was rejected\n";
            return false;
        }
        candidate.placement_weight = 0;
        if (relocation_unit_target_eligible (source, candidate, 7, "", {"player"})) {
            std::cerr << "ineligible actor-only target (zero weight) was accepted\n";
            return false;
        }
    }

    // Spec step 6: deterministic weighted draw over the eligible set. A higher
    // weight is preferred, the result is order-independent, and a zero weight is
    // excluded.
    {
        using zlink::framework::select_weighted_relocation_target;
        using weighted_set_t = std::vector<std::pair<std::string, std::uint32_t>>;

        const weighted_set_t high_low = {{"high", 3}, {"low", 1}};
        if (select_weighted_relocation_target (high_low) != std::optional<std::string> ("high")) {
            std::cerr << "weighted draw did not prefer the higher weight\n";
            return false;
        }
        const weighted_set_t low_high = {{"low", 1}, {"high", 3}};
        if (select_weighted_relocation_target (low_high) != std::optional<std::string> ("high")) {
            std::cerr << "weighted draw was not order-independent\n";
            return false;
        }
        const weighted_set_t single = {{"only", 1}};
        if (select_weighted_relocation_target (single) != std::optional<std::string> ("only")) {
            std::cerr << "weighted draw did not return the sole candidate\n";
            return false;
        }
        const weighted_set_t empty_set = {};
        if (select_weighted_relocation_target (empty_set).has_value ()) {
            std::cerr << "weighted draw returned a target for an empty set\n";
            return false;
        }
        const weighted_set_t zero_and_positive = {{"zero", 0}, {"positive", 2}};
        if (select_weighted_relocation_target (zero_and_positive)
            != std::optional<std::string> ("positive")) {
            std::cerr << "weighted draw did not exclude the zero-weight candidate\n";
            return false;
        }
    }

    return true;
}

int verify_manual_topology_relocation_blockers ()
{
    if (!verify_relocation_blocker (
          "manual ClientServer topology",
          [] (zlink::framework::zlink_framework_options_t &options) {
              options.add_client_server_channel ("manual-orders")
                .client ()
                .connect ("tcp://127.0.0.1:29999");
          },
          zlink::framework::relocation_reason_t::manual_topology_unsupported)) {
        return EXIT_FAILURE;
    }
    if (!verify_relocation_blocker (
          "manual RouteMesh topology",
          [] (zlink::framework::zlink_framework_options_t &options) {
              auto node = options.add_route_mesh ("retire-manual-mesh");
              node.set_object_role (zlink::framework::object_role_t::none);
              node.channel_name ("retire-manual-channel").client ();
              node.set_routing_id (zlink::routing_id_t::from ("retire-manual-node"))
                .listen ("inproc://cpp-retire-manual-node");
              node.peer_connections ().connect ("tcp://127.0.0.1:29998");
          },
          zlink::framework::relocation_reason_t::manual_topology_unsupported)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
}
