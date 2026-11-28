/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/host/relocation_target_eligibility.hpp"
#include "runtime/locations/authority_aggregate_relocation.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/utils/poll_interval_wait.hpp"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <set>
#include <stdexcept>
#include <typeindex>
#include <variant>

namespace zlink::framework::detail
{

void app_state_t::wait_relocation_cleanup ()
{
    {
        std::unique_lock lock (termination_teardown_mutex);
        termination_teardown_changed.wait (lock, [&] { return active_relocation_preflights == 0; });
    }
    std::shared_ptr<relocation_waiter_t> waiter;
    auto terminal = [&] {
        std::lock_guard lock (relocation_operation.mutex);
        return relocation_operation.register_terminal_waiter_locked (waiter);
    }();
    // Timers are already stopped. Observe the accepted source owner's
    // cutoff cleanup before hosted services release its native resources.
    (void) detail::task_result_access_t::read (terminal);
}

void app_state_t::complete_termination (termination_result_t terminal)
{
    std::vector<std::shared_ptr<termination_waiter_t>> waiters;
    {
        std::lock_guard lock (termination_operation.mutex);
        if (!termination_operation.terminal) {
            termination_operation.terminal = true;
            termination_operation.result = terminal;
            waiters = std::move (termination_operation.waiters);
            termination_operation.waiters.clear ();
        }
        auto expected = framework_runtime_state_t::draining;
        (void) runtime_state.compare_exchange_strong (expected, framework_runtime_state_t::stopped,
                                                      std::memory_order_acq_rel);
    }
    for (auto &waiter : waiters) {
        waiter->complete (terminal);
    }
}

} // namespace zlink::framework::detail

namespace zlink::framework
{

namespace
{

enum class shutdown_force_reason_t
{
    deadline_exceeded,
    teardown_failed
};

struct shutdown_completed_t
{
};

struct shutdown_forced_t
{
    shutdown_force_reason_t reason;
};

using shutdown_progress_t = std::variant<shutdown_completed_t, shutdown_forced_t>;

bool capacity_available_for_relocation (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved < static_cast<std::uint64_t> (usage.limit);
}

bool supports_relocation_source (const mesh_node_descriptor_t &source,
                                 const mesh_node_descriptor_t &candidate,
                                 std::int64_t target_application_version)
{
    if (candidate.state != framework_runtime_state_t::serving
        || candidate.object_role != object_role_t::server || candidate.placement_weight <= 0
        || candidate.application_version != target_application_version
        || (source.maintenance_wave && candidate.maintenance_wave == source.maintenance_wave)) {
        return false;
    }
    for (const auto &required : source.object_capabilities) {
        const auto supported = std::find_if (
          candidate.object_capabilities.begin (), candidate.object_capabilities.end (),
          [&] (const object_capability_t &value) {
              return value.object_kind == required.object_kind
                     && value.stable_type == required.stable_type
                     && (required.policy != maintenance_policy_kind_t::snapshot
                         || (value.policy == maintenance_policy_kind_t::snapshot
                             && value.has_snapshot_adapter));
          });
        if (supported == candidate.object_capabilities.end ())
            return false;
        if (required.object_kind == placement_object_kind_t::actor) {
            if (!capacity_available_for_relocation (candidate.capacity.actors))
                return false;
        } else {
            if (!capacity_available_for_relocation (candidate.capacity.spots))
                return false;
            const auto typed = std::find_if (
              candidate.capacity.spot_types.begin (), candidate.capacity.spot_types.end (),
              [&] (const spot_type_capacity_t &value) {
                  return value.object_kind == required.object_kind
                         && value.stable_type == required.stable_type;
              });
            if (typed != candidate.capacity.spot_types.end ()
                && !capacity_available_for_relocation (typed->usage)) {
                return false;
            }
        }
    }
    return candidate.activation_concurrency.limit <= 0
           || candidate.activation_concurrency.active
                < static_cast<std::uint32_t> (candidate.activation_concurrency.limit);
}

struct relocation_preflight_t
{
    std::optional<relocation_reason_t> blocker;
    std::int64_t effective_target_application_version = 0;
    std::int64_t source_application_version = 0;
};

std::int64_t relocation_target_application_version (const relocation_options_t &options,
                                                    std::int64_t source_version)
{
    if (options.mode == relocation_mode_t::planned_maintenance)
        return source_version;
    if (*options.target_application_version <= source_version) {
        throw std::invalid_argument (
          "rolling update target application version must be greater than the source version");
    }
    return *options.target_application_version;
}

task_t<relocation_preflight_t>
relocation_topology_preflight_once (detail::app_state_t &state, const relocation_options_t &options)
{
    if (state.has_manual_service_topology && state.has_manual_service_topology ())
        co_return relocation_preflight_t{relocation_reason_t::manual_topology_unsupported, 0};
    try {
        // A process with several Meshes must find a target on every Mesh once it
        // owns a unit anywhere (§5.1), so the inventory is read for the whole host.
        bool requires_target = false;
        for (const auto &node : state.route_mesh_nodes) {
            if (!node)
                continue;
            const auto runtime =
              detail::spot_node_runtime_t::from (state.zlink, node->mesh_name ());
            if (!runtime)
                continue;
            const auto units = co_await runtime->read_relocation_units ();
            if (!units.empty ()) {
                requires_target = true;
                break;
            }
        }
        if (!requires_target) {
            const auto source_version = mesh_node_descriptor_t{}.application_version;
            co_return relocation_preflight_t{
              std::nullopt, relocation_target_application_version (options, source_version),
              source_version};
        }
        if (!state.services.contains (std::type_index (typeid (relocation_repository_t)))
            || !state.services.contains (std::type_index (typeid (location_repository_t)))) {
            co_return relocation_preflight_t{relocation_reason_t::store_unavailable, 0};
        }

        auto provider = state.services.build_provider ();
        auto &store = provider.get_required<location_repository_t> ();
        auto &live = provider.get_required<runtime::live_location_reader_t> ();

        std::set<std::string> local_rids;
        std::optional<std::int64_t> source_application_version;
        for (const auto &node : state.route_mesh_nodes) {
            if (node) {
                const auto rid = co_await node->routing_id_async ();
                if (rid)
                    local_rids.insert (rid->to_hex ());
            }
        }

        for (const auto &node : state.route_mesh_nodes) {
            if (!node)
                continue;
            const auto [node_mesh, local_rid] = co_await node->identity_async ();
            const auto status = node->status ();
            if (!local_rid || status.lifecycle_generation () == 0)
                co_return relocation_preflight_t{relocation_reason_t::runtime_not_ready, 0};

            std::vector<mesh_node_descriptor_t> descriptors;
            location_page_request_t page;
            do {
                auto listed = co_await store.list_mesh_nodes (node_mesh, page);
                descriptors.insert (descriptors.end (),
                                    std::make_move_iterator (listed.items.begin ()),
                                    std::make_move_iterator (listed.items.end ()));
                page.continuation_token = std::move (listed.continuation_token);
            } while (page.continuation_token);

            const auto source = std::find_if (
              descriptors.begin (), descriptors.end (),
              [&] (const mesh_node_descriptor_t &descriptor) {
                  return descriptor.rid.to_hex () == local_rid->to_hex ()
                         && descriptor.lifecycle_generation == status.lifecycle_generation ();
              });
            std::optional<std::chrono::steady_clock::duration> source_lifetime;
            if (source != descriptors.end ())
                source_lifetime = co_await live.owner_admission_lifetime (source->owner_id);
            if (!source_lifetime) {
                co_return relocation_preflight_t{relocation_reason_t::store_unavailable, 0};
            }
            if (source_application_version
                && *source_application_version != source->application_version) {
                co_return relocation_preflight_t{relocation_reason_t::state_incompatible, 0};
            }
            source_application_version = source->application_version;
            const auto target_application_version =
              relocation_target_application_version (options, source->application_version);

            if (co_await node->native_node ().has_finishing_relocation_target ())
                co_return relocation_preflight_t{relocation_reason_t::operation_in_progress,
                                                 target_application_version,
                                                 source->application_version};
            if (auto spots = detail::spot_node_runtime_t::from (state.zlink, node_mesh)) {
                const auto units = co_await spots->read_relocation_units ();
                std::vector<authority_key_t> keys;
                for (const auto &actor : units.standalone_actors)
                    keys.push_back (runtime::actor_authority_key (actor.actor_id ().value ()));
                for (const auto &unit : units.spots) {
                    keys.push_back (runtime::spot_authority_key (unit.spot_id));
                    for (const auto &actor : unit.actors)
                        keys.push_back (runtime::actor_authority_key (actor.actor_id ().value ()));
                }
                for (const auto &key : keys) {
                    const auto read = co_await store.read_authority (key);
                    const auto *authority = std::get_if<authority_snapshot_t> (&read);
                    if (authority && runtime::finishing_relocation (*authority))
                        co_return relocation_preflight_t{relocation_reason_t::operation_in_progress,
                                                         target_application_version,
                                                         source->application_version};
                }
            }

            bool replacement = false;
            for (const auto &candidate : descriptors) {
                if (local_rids.contains (candidate.rid.to_hex ())
                    || candidate.lifecycle_generation == 0)
                    continue;
                const auto candidate_lifetime =
                  co_await live.owner_admission_lifetime (candidate.owner_id);
                if (candidate_lifetime
                    && supports_relocation_source (*source, candidate, target_application_version)
                    && node->has_admitted_peer (candidate.rid, candidate.lifecycle_generation)) {
                    replacement = true;
                    break;
                }
            }
            if (!replacement) {
                co_return relocation_preflight_t{relocation_reason_t::target_unavailable,
                                                 target_application_version};
            }
        }
        const auto source_version = *source_application_version;
        const auto effective = relocation_target_application_version (options, source_version);
        co_return relocation_preflight_t{std::nullopt, effective, source_version};
    }
    catch (const std::invalid_argument &) {
        throw;
    }
    catch (...) {
        co_return relocation_preflight_t{relocation_reason_t::store_unavailable, 0};
    }
}

std::chrono::milliseconds relocation_topology_poll_interval (detail::app_state_t &state) noexcept
{
    try {
        auto provider = state.services.build_provider ();
        if (auto runtime = provider.get<runtime::location_runtime_t> ()) {
            return std::max (runtime->get ().options ().polling_interval,
                             std::chrono::milliseconds (1));
        }
    }
    catch (...) {
    }
    return std::chrono::milliseconds (25);
}

task_t<relocation_preflight_t>
relocation_topology_preflight_until (detail::app_state_t &state,
                                     const relocation_options_t &options,
                                     std::chrono::steady_clock::time_point deadline_at)
{
    const auto polling_interval = relocation_topology_poll_interval (state);
    for (;;) {
        auto result = co_await relocation_topology_preflight_once (state, options);
        if (result.blocker != relocation_reason_t::target_unavailable
            || std::chrono::steady_clock::now () >= deadline_at) {
            co_return result;
        }
        co_await detail::delay (
          state.delays,
          std::chrono::ceil<std::chrono::milliseconds> (
            std::min (deadline_at, std::chrono::steady_clock::now () + polling_interval)
            - std::chrono::steady_clock::now ()));
    }
}

template <typename Predicate>
task_t<std::optional<mesh_node_descriptor_t>>
wait_for_relocation_target (runtime::store_location_resolvers_t &peers,
                            detail::delay_owner_t &delays,
                            std::string mesh_name,
                            std::chrono::steady_clock::time_point deadline_at,
                            std::chrono::milliseconds polling_interval,
                            const std::function<bool ()> &shutdown_requested,
                            Predicate &&eligible,
                            relocation_reason_t &failure_reason)
{
    for (;;) {
        auto live = co_await peers.list_live_mesh_nodes (mesh_name);
        // Spec step 6: apply node-wide placement weight to the final eligible
        // candidate set with a deterministic weighted draw instead of taking the
        // first eligible node in store order. The candidate key pairs RID with
        // lifecycle generation so a manual fixed-RID node appearing at two
        // generations is not collapsed and the winner maps back to the exact
        // descriptor.
        const auto candidate_key = [] (const mesh_node_descriptor_t &candidate) {
            return candidate.rid.to_hex () + '\0' + std::to_string (candidate.lifecycle_generation);
        };
        std::vector<std::pair<std::string, std::uint32_t>> eligible_by_weight;
        for (const auto &candidate : live) {
            if (eligible (candidate) && candidate.placement_weight > 0) {
                eligible_by_weight.emplace_back (
                  candidate_key (candidate),
                  static_cast<std::uint32_t> (candidate.placement_weight));
            }
        }
        if (const auto chosen = select_weighted_relocation_target (eligible_by_weight)) {
            const auto target =
              std::find_if (live.begin (), live.end (), [&] (const auto &candidate) {
                  return candidate_key (candidate) == *chosen;
              });
            if (target != live.end ())
                co_return *target;
        }
        if (shutdown_requested ()) {
            failure_reason = relocation_reason_t::shutdown_requested;
            co_return std::nullopt;
        }
        if (std::chrono::steady_clock::now () >= deadline_at) {
            failure_reason = relocation_reason_t::target_unavailable;
            co_return std::nullopt;
        }
        try {
            co_await detail::delay (
              delays,
              std::max (std::chrono::milliseconds::zero (),
                        std::chrono::ceil<std::chrono::milliseconds> (
                          std::min<std::chrono::steady_clock::duration> (
                            deadline_at - std::chrono::steady_clock::now (), polling_interval))));
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::shutting_down)
                throw;
            failure_reason = relocation_reason_t::shutdown_requested;
            co_return std::nullopt;
        }
    }
}

bool publish_mesh_descriptor_state (detail::app_state_t &state,
                                    framework_runtime_state_t desired) noexcept
{
    bool published = true;
    for (const auto &service : state.hosted_services) {
        auto *lifecycle = detail::lifecycle_of (service.get ());
        if (lifecycle && !lifecycle->publish_descriptor_state (desired)) {
            published = false;
        }
    }
    return published;
}

std::vector<std::string> begin_application_relocation_readiness (detail::app_state_t &state)
{
    std::vector<std::string> meshes;
    for (const auto &service : state.hosted_services) {
        auto *lifecycle = detail::lifecycle_of (service.get ());
        if (!lifecycle)
            continue;
        lifecycle->visit_relocation_nodes ([&] (const auto &node) {
            if (!node
                || std::find (meshes.begin (), meshes.end (), node->mesh_name ()) != meshes.end ())
                return;
            auto runtime = detail::spot_node_runtime_t::from (state.zlink, node->mesh_name ());
            if (!runtime)
                return;
            runtime->begin_relocation_readiness ();
            meshes.push_back (node->mesh_name ());
        });
    }
    return meshes;
}

void cancel_application_relocation_readiness (detail::app_state_t &state,
                                              const std::vector<std::string> &meshes) noexcept
{
    for (const auto &mesh : meshes) {
        auto runtime = detail::spot_node_runtime_t::from (state.zlink, mesh);
        if (runtime)
            runtime->end_relocation_readiness ({});
    }
}

void record_host_relocation (detail::app_state_t &state,
                             const relocation_result_t &result,
                             std::chrono::steady_clock::time_point started)
{
    runtime::runtime_metrics_t metrics (state.monitoring);
    if (!metrics.enabled ())
        return;
    const char *mode = result.mode == relocation_mode_t::planned_maintenance ? "planned_maintenance"
                                                                             : "rolling_update";
    metrics.histogram (
      "zlink.host.relocation.duration", "s",
      std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count (),
      {{"mode", mode},
       {"outcome", result.outcome == relocation_outcome_t::relocated ? "relocated" : "blocked"}});
    if (result.outcome == relocation_outcome_t::blocked) {
        constexpr const char *reasons[] = {"none",
                                           "target_unavailable",
                                           "store_unavailable",
                                           "relocation_disabled",
                                           "state_incompatible",
                                           "deadline_exceeded",
                                           "relocation_failed",
                                           "runtime_not_ready",
                                           "manual_topology_unsupported",
                                           "shutdown_requested",
                                           "operation_in_progress"};
        metrics.counter (
          "zlink.host.relocation.blocked", "{operation}", 1,
          {{"mode", mode}, {"reason", reasons[static_cast<unsigned> (result.reason)]}});
    }
}

} // namespace

task_t<relocation_result_t> app_t::relocate (relocation_options_t options,
                                             std::stop_token wait_cancellation)
{
    if (options.mode == relocation_mode_t::planned_maintenance
        && options.target_application_version) {
        throw std::invalid_argument (
          "planned maintenance does not accept a target application version");
    }
    if (options.mode == relocation_mode_t::rolling_update && !options.target_application_version) {
        throw std::invalid_argument ("rolling update requires a target application version");
    }
    const auto deadline = options.deadline.value_or (std::chrono::seconds (30));
    if (deadline <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("relocation deadline must be greater than zero");
    options.deadline = deadline;
    return relocate_async (std::move (options), wait_cancellation);
}

task_t<relocation_result_t> app_t::relocate_async (relocation_options_t options,
                                                   std::stop_token wait_cancellation)
{
    const auto metric_started = std::chrono::steady_clock::now ();
    const auto rejected = [&] (relocation_result_t result) {
        record_host_relocation (*_state, result, metric_started);
        return result_t<relocation_result_t>::success (result);
    };
    detail::app_state_t::relocation_preflight_guard_t preflight_guard (*_state);
    const auto preflight_deadline_at = std::chrono::steady_clock::now () + *options.deadline;

    auto &operation = _state->relocation_operation;
    std::thread completed_worker;
    {
        std::lock_guard lock (operation.mutex);
        if (!operation.started && operation.worker.joinable ()) {
            completed_worker = std::move (operation.worker);
        }
    }
    if (completed_worker.joinable ()) {
        completed_worker.join ();
    }

    relocation_preflight_t preflight;
    if (runtime_state () != framework_runtime_state_t::serving) {
        preflight.blocker = relocation_reason_t::runtime_not_ready;
    } else {
        try {
            preflight = co_await relocation_topology_preflight_until (*_state, options,
                                                                      preflight_deadline_at);
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::shutting_down)
                throw;
            preflight.blocker = relocation_reason_t::shutdown_requested;
        }
    }

    std::shared_ptr<detail::app_state_t::relocation_waiter_t> waiter;
    task_t<relocation_result_t> task (result_t<relocation_result_t>::success ({}));
    {
        std::lock_guard lock (operation.mutex);
        if (operation.started && !operation.terminal) {
            // A concurrent call joins the running operation when it targets the
            // same mode and effective target application version. The deadline
            // is not part of the join key: the first call's deadline fixes the
            // shared operation, and a later joining call neither extends nor
            // shortens it.
            const bool joins_running_operation =
              operation.options.mode == options.mode
              && operation.options.target_application_version == options.target_application_version;
            if (!joins_running_operation) {
                // The rejected call's result reflects the valid option it
                // requested, not the running operation's target: rolling_update
                // requests its explicit target version, planned_maintenance
                // requests the source application version.
                const auto rejected_effective_target = relocation_target_application_version (
                  options, operation.source_application_version);
                co_return rejected ({options.mode, rejected_effective_target,
                                     relocation_outcome_t::blocked,
                                     relocation_reason_t::operation_in_progress});
            }
        } else if (operation.terminal) {
            co_return result_t<relocation_result_t>::success (operation.result);
        } else {
            if (preflight.blocker) {
                co_return rejected ({options.mode, preflight.effective_target_application_version,
                                     relocation_outcome_t::blocked, *preflight.blocker});
            }
            const auto readiness_meshes = begin_application_relocation_readiness (*_state);
            if (!publish_mesh_descriptor_state (*_state, framework_runtime_state_t::relocating)) {
                (void) publish_mesh_descriptor_state (*_state, framework_runtime_state_t::serving);
                cancel_application_relocation_readiness (*_state, readiness_meshes);
                co_return rejected ({options.mode, preflight.effective_target_application_version,
                                     relocation_outcome_t::blocked,
                                     relocation_reason_t::store_unavailable});
            }
            operation.deadline = std::chrono::duration_cast<std::chrono::milliseconds> (
              preflight_deadline_at - std::chrono::steady_clock::now ());
            if (operation.deadline <= std::chrono::milliseconds::zero ()) {
                (void) publish_mesh_descriptor_state (*_state, framework_runtime_state_t::serving);
                cancel_application_relocation_readiness (*_state, readiness_meshes);
                co_return rejected ({options.mode, preflight.effective_target_application_version,
                                     relocation_outcome_t::blocked,
                                     relocation_reason_t::target_unavailable});
            }
            operation.metric_started = metric_started;
            operation.started = true;
            operation.options = options;
            operation.source_application_version = preflight.source_application_version;
            operation.deadline_at = std::chrono::system_clock::now () + operation.deadline;
            operation.result.mode = options.mode;
            operation.result.effective_target_application_version =
              preflight.effective_target_application_version;
            _state->runtime_state.store (framework_runtime_state_t::relocating,
                                         std::memory_order_release);
            auto *state = _state.get ();
            operation.worker = std::thread ([state] {
                auto running = std::make_shared<task_t<void>> (run_shared_relocation (*state));
                detail::observe_task_completion (*running, [running] (const result_t<void> &) {});
            });
        }
        task = operation.register_terminal_waiter_locked (waiter);
    }
    waiter->arm (wait_cancellation);
    preflight_guard.release ();
    co_return co_await runtime::await_result (std::move (task));
}

task_t<void> app_t::run_shared_relocation (detail::app_state_t &state)
{
    auto &operation = state.relocation_operation;
    const auto deadline_at = std::chrono::steady_clock::now () + operation.deadline;
    relocation_result_t terminal{
      operation.options.mode, operation.result.effective_target_application_version,
      relocation_outcome_t::blocked, relocation_reason_t::relocation_failed};
    std::vector<std::string> readiness_meshes;
    std::map<std::string, std::vector<spot_id_t>> relocated_ready_spots;
    std::size_t committed_units = 0;

    auto shutdown_requested = [&] {
        std::lock_guard lock (operation.mutex);
        return operation.shutdown_requested;
    };
    const auto source_authority_lost =
      [] (std::optional<runtime::stateful::relocation_reason_t> reason) {
          return reason == runtime::stateful::relocation_reason_t::owner_lease_expired
                 || reason == runtime::stateful::relocation_reason_t::authority_missing;
      };
    auto complete = [&] (relocation_result_t result,
                         std::optional<runtime::stateful::relocation_reason_t> unit_failure = {}) {
        for (const auto &mesh_name : readiness_meshes) {
            auto runtime = detail::spot_node_runtime_t::from (state.zlink, mesh_name);
            if (runtime) {
                const auto found = relocated_ready_spots.find (mesh_name);
                runtime->end_relocation_readiness (
                  found != relocated_ready_spots.end () ? found->second : std::vector<spot_id_t>{});
            }
        }
        std::unique_lock lock (operation.mutex);
        const bool interrupted = operation.shutdown_requested;
        if (interrupted) {
            result.outcome = relocation_outcome_t::blocked;
            result.reason = relocation_reason_t::shutdown_requested;
        }
        if (!interrupted) {
            if (result.outcome == relocation_outcome_t::relocated) {
                if (!publish_mesh_descriptor_state (state, framework_runtime_state_t::relocated)) {
                    result.outcome = relocation_outcome_t::blocked;
                    result.reason = relocation_reason_t::store_unavailable;
                }
            }
            if (result.outcome == relocation_outcome_t::relocated) {
                state.runtime_state.store (framework_runtime_state_t::relocated,
                                           std::memory_order_release);
            } else if (committed_units != 0 || source_authority_lost (unit_failure)) {
                state.runtime_state.store (framework_runtime_state_t::error,
                                           std::memory_order_release);
            } else {
                (void) publish_mesh_descriptor_state (state, framework_runtime_state_t::serving);
                state.runtime_state.store (framework_runtime_state_t::serving,
                                           std::memory_order_release);
            }
        }

        std::vector<std::shared_ptr<detail::app_state_t::relocation_waiter_t>> waiters;
        operation.terminal = result.outcome == relocation_outcome_t::relocated;
        operation.started = operation.terminal;
        if (!operation.terminal) {
            operation.shutdown_requested = false;
            operation.deadline_at.reset ();
        }
        record_host_relocation (state, result, operation.metric_started);
        operation.result = result;
        waiters = std::move (operation.waiters);
        operation.waiters.clear ();
        lock.unlock ();
        for (auto &waiter : waiters)
            waiter->complete (result);
    };

    try {
        std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> relocation_nodes;
        for (const auto &service : state.hosted_services) {
            if (auto *lifecycle = detail::lifecycle_of (service.get ()))
                lifecycle->visit_relocation_nodes ([&] (const auto &node) {
                    if (!node)
                        return;
                    relocation_nodes.push_back (node);
                    const auto mesh = node->mesh_name ();
                    if (std::find (readiness_meshes.begin (), readiness_meshes.end (), mesh)
                        == readiness_meshes.end ())
                        readiness_meshes.push_back (mesh);
                });
        }

        struct mesh_relocation_plan_t
        {
            std::shared_ptr<detail::mesh_node_runtime_t> node;
            std::string node_mesh;
            std::optional<zlink::routing_id_t> local_rid;
            detail::spot_node_runtime_t spot_runtime;
            mesh_node_descriptor_t source_descriptor;
            detail::spot_node_runtime_t::relocation_units_t units;
        };

        std::vector<mesh_relocation_plan_t> plans;
        auto provider = state.services.build_provider ();
        auto peers = provider.get<runtime::store_location_resolvers_t> ();
        auto location_store = provider.get<location_repository_t> ();
        const auto topology_poll_interval = relocation_topology_poll_interval (state);
        for (const auto &node : relocation_nodes) {
            const auto [node_mesh, local_rid] = co_await node->identity_async ();
            auto spot_runtime = detail::spot_node_runtime_t::from (state.zlink, node_mesh);
            if (!spot_runtime)
                continue;
            if ((co_await spot_runtime->read_relocation_units ()).empty ())
                continue;

            if (!peers || !location_store) {
                terminal.reason = relocation_reason_t::store_unavailable;
                complete (terminal);
                co_return;
            }

            // Source's own descriptor supplies the maintenance wave and the
            // declared relocation policy per capability for the candidate
            // narrowing below. Match RID *and* lifecycle generation exactly as
            // the preflight does, so a stale generation of a fixed-RID source is
            // never used to drive eligibility; and fail closed if the source
            // can't be read, matching the preflight's store_unavailable — never
            // proceed with a default descriptor, which would silently disable
            // the maintenance-wave and snapshot-policy gates.
            mesh_node_descriptor_t source_descriptor;
            {
                const auto source_generation = node->status ().lifecycle_generation ();
                auto live_source = co_await peers->get ().list_live_mesh_nodes (node_mesh);
                const auto found =
                  std::find_if (live_source.begin (), live_source.end (),
                                [&] (const mesh_node_descriptor_t &descriptor) {
                                    return local_rid
                                           && descriptor.rid.to_hex () == local_rid->to_hex ()
                                           && descriptor.lifecycle_generation == source_generation;
                                });
                if (found == live_source.end ()) {
                    terminal.reason = relocation_reason_t::store_unavailable;
                    complete (terminal);
                    co_return;
                }
                source_descriptor = *found;
            }

            // Resolve the source lifecycle fence before acquiring current units.
            auto units = co_await spot_runtime->relocation_units ();
            if (units.empty ())
                continue;
            plans.push_back ({node, node_mesh, local_rid, std::move (*spot_runtime),
                              std::move (source_descriptor), std::move (units)});
        }

        // Host §7: all Actor units settle before any Spot aggregate starts.
        for (auto &[node, node_mesh, local_rid, spot_runtime, source_descriptor, units] : plans) {
            for (const auto &actor : units.standalone_actors) {
                if (shutdown_requested ()) {
                    terminal.reason = relocation_reason_t::shutdown_requested;
                    complete (terminal);
                    co_return;
                }
                const auto now = std::chrono::steady_clock::now ();
                if (now >= deadline_at) {
                    terminal.reason = relocation_reason_t::deadline_exceeded;
                    complete (terminal);
                    co_return;
                }
                const auto target = co_await wait_for_relocation_target (
                  peers->get (), state.delays, node_mesh, deadline_at, topology_poll_interval,
                  shutdown_requested,
                  [&] (const mesh_node_descriptor_t &peer) {
                      if (local_rid && peer.rid.to_hex () == local_rid->to_hex ())
                          return false;
                      // Single-Actor relocation target: same frozen narrowing as
                      // the shared path, with no user Spot requirement.
                      return relocation_unit_target_eligible (
                               source_descriptor, peer,
                               terminal.effective_target_application_version, {},
                               {std::string (
                                 ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                   actor))})
                             && node->has_admitted_peer (peer.rid, peer.lifecycle_generation);
                  },
                  terminal.reason);
                if (!target) {
                    complete (terminal);
                    co_return;
                }

                const auto authority_key =
                  runtime::actor_authority_key (actor.actor_id ().value ());
                const auto authority_read =
                  co_await location_store->get ().read_authority (authority_key);
                const auto *authority = std::get_if<authority_snapshot_t> (&authority_read);
                if (!authority
                    || authority->allocation.state != placement_allocation_state_t::active
                    || authority->allocation.object_kind != placement_object_kind_t::actor
                    || authority->allocation.stable_type
                         != ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
                    || authority->object_generation != actor.object_generation ()
                    || authority->allocation.target.node_rid.value () != local_rid->to_string ()) {
                    terminal.reason = relocation_reason_t::state_incompatible;
                    complete (terminal);
                    co_return;
                }

                const auto source = co_await node->native_node ().resolve_actor_async (actor);
                if (!source) {
                    terminal.reason = relocation_reason_t::state_incompatible;
                    complete (terminal);
                    co_return;
                }
                const auto moved = co_await node->relocate_application_actor (
                  actor, *target, *authority, deadline_at);
                if (moved.terminal != runtime::stateful::relocation_terminal_t::completed) {
                    terminal.reason = relocation_reason_t::relocation_failed;
                    complete (terminal, moved.reason);
                    co_return;
                }
                const std::vector<runtime::stateful::object_ref_t> source_unit{source.value ()};
                co_await spot_runtime.retire_relocation_source (source_unit);
                ++committed_units;
            }
        }

        for (auto &[node, node_mesh, local_rid, spot_runtime, source_descriptor, units] : plans) {
            if (units.spots.empty ())
                continue;
            while (!co_await spot_runtime.relocation_ready_async ()) {
                if (shutdown_requested () || std::chrono::steady_clock::now () >= deadline_at) {
                    terminal.reason = shutdown_requested ()
                                        ? relocation_reason_t::shutdown_requested
                                        : relocation_reason_t::deadline_exceeded;
                    complete (terminal);
                    co_return;
                }
                co_await detail::delay (state.delays, std::chrono::milliseconds (1));
            }
            for (const auto &unit : units.spots) {
                const auto spot_key = runtime::spot_authority_key (unit.spot_id);
                const auto close_committed = [&] (const authority_read_result_t &read) {
                    if (unit.kind != placement_object_kind_t::instance_spot)
                        return false;
                    const auto *authority = std::get_if<authority_snapshot_t> (&read);
                    if (!authority)
                        return true;
                    const auto closing =
                      runtime::decode_instance_closing_state (authority->payload);
                    return closing && closing->stable_type == unit.spot_type
                           && closing->spot_id == unit.spot_id
                           && closing->object_generation == unit.object_generation
                           && closing->authority_owner_generation
                                == authority->authority_owner_generation
                           && authority->allocation.target.node_rid.value ()
                                == local_rid->to_string ()
                           && authority->allocation.target.node_lifecycle_generation
                                == source_descriptor.lifecycle_generation;
                };
                std::vector<std::string> actor_types;
                actor_types.reserve (unit.actors.size ());
                for (const auto &actor : unit.actors)
                    actor_types.emplace_back (
                      ::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
                const auto target = co_await wait_for_relocation_target (
                  peers->get (), state.delays, node_mesh, deadline_at, topology_poll_interval,
                  shutdown_requested,
                  [&] (const mesh_node_descriptor_t &peer) {
                      if (local_rid && peer.rid.to_hex () == local_rid->to_hex ())
                          return false;
                      // The actual target selection applies the same frozen
                      // candidate narrowing the preflight applies
                      // (supports_relocation_source): a serving Object Server on
                      // the effective version, excluded from source's non-empty
                      // maintenance wave, with relocation-policy/adapter
                      // compatibility, positive placement weight, and
                      // population/reservation capacity; and, like the preflight,
                      // the candidate's RID/generation must be an admitted Core
                      // peer.
                      return relocation_unit_target_eligible (
                               source_descriptor, peer,
                               terminal.effective_target_application_version,
                               relocation_spot_requirement_t{unit.kind, unit.spot_type},
                               actor_types)
                             && node->has_admitted_peer (peer.rid, peer.lifecycle_generation);
                  },
                  terminal.reason);
                if (!target) {
                    if (terminal.reason == relocation_reason_t::target_unavailable
                        && unit.kind == placement_object_kind_t::instance_spot
                        && close_committed (
                          co_await location_store->get ().read_authority (spot_key)))
                        continue;
                    complete (terminal);
                    co_return;
                }

                auto spot_authority = co_await location_store->get ().read_authority (spot_key);
                if (close_committed (spot_authority))
                    continue;

                std::vector<runtime::stateful::object_ref_t> sources;
                std::vector<std::string> stable_types;
                const auto spot_source =
                  co_await node->native_node ().resolve_spot_async (std::string (unit.spot_id));
                if (!spot_source) {
                    if (unit.kind == placement_object_kind_t::instance_spot
                        && close_committed (
                          co_await location_store->get ().read_authority (spot_key)))
                        continue;
                    terminal.reason = relocation_reason_t::state_incompatible;
                    complete (terminal);
                    co_return;
                }
                sources.push_back (*spot_source);
                stable_types.push_back (unit.spot_type);
                for (const auto &actor : unit.actors) {
                    const auto source = co_await node->native_node ().resolve_actor_async (actor);
                    if (!source) {
                        terminal.reason = relocation_reason_t::state_incompatible;
                        complete (terminal);
                        co_return;
                    }
                    sources.push_back (*source);
                    stable_types.emplace_back (
                      ::zlink::framework::detail::actor_ref_access_t::actor_type (actor));
                }

                std::vector<authority_snapshot_t> authorities;
                for (std::size_t index = 0; index != sources.size (); ++index) {
                    const auto &source = sources[index];
                    authority_read_result_t authority_read;
                    if (index == 0)
                        authority_read = std::move (spot_authority);
                    else
                        authority_read = co_await location_store->get ().read_authority (
                          runtime::actor_authority_key (source.key));
                    const auto *authority = std::get_if<authority_snapshot_t> (&authority_read);
                    if (!authority
                        || authority->allocation.state != placement_allocation_state_t::active
                        || authority->allocation.object_kind
                             != (index == 0 ? unit.kind : placement_object_kind_t::actor)
                        || authority->allocation.stable_type != stable_types[index]
                        || authority->object_generation != source.object_generation
                        || authority->authority_owner_generation
                             != source.authority_owner_generation) {
                        terminal.reason = relocation_reason_t::state_incompatible;
                        complete (terminal);
                        co_return;
                    }
                    authorities.push_back (*authority);
                }

                const auto moved = co_await node->relocate_application_unit (
                  sources, std::move (stable_types), *target, authorities, deadline_at);
                if (moved.terminal != runtime::stateful::relocation_terminal_t::completed) {
                    if (unit.kind == placement_object_kind_t::instance_spot
                        && !source_authority_lost (moved.reason)
                        && close_committed (
                          co_await location_store->get ().read_authority (spot_key)))
                        continue;
                    terminal.reason = relocation_reason_t::relocation_failed;
                    complete (terminal, moved.reason);
                    co_return;
                }
                co_await spot_runtime.retire_relocation_source (sources);
                ++committed_units;
                relocated_ready_spots[node_mesh].push_back (unit.spot_id);
            }
        }
        terminal.outcome = relocation_outcome_t::relocated;
        terminal.reason = relocation_reason_t::none;
    }
    catch (const framework_exception_t &error) {
        terminal.reason = error.kind () == framework_error_kind_t::shutting_down
                            ? relocation_reason_t::shutdown_requested
                            : relocation_reason_t::store_unavailable;
    }
    catch (...) {
        terminal.reason = relocation_reason_t::store_unavailable;
    }
    complete (terminal);
    co_return;
}

task_t<termination_result_t> app_t::shutdown (std::chrono::milliseconds deadline,
                                              std::stop_token wait_cancellation)
{
    if (deadline <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("shutdown deadline must be greater than zero");
    {
        std::lock_guard relocation_lock (_state->relocation_operation.mutex);
        if (_state->relocation_operation.started && !_state->relocation_operation.terminal) {
            _state->relocation_operation.shutdown_requested = true;
        }
    }
    auto &operation = _state->termination_operation;
    std::shared_ptr<detail::app_state_t::termination_waiter_t> waiter;
    task_t<termination_result_t> task (result_t<termination_result_t>::success ({}));
    {
        std::lock_guard lock (operation.mutex);
        if (operation.terminal) {
            return task_t<termination_result_t> (
              result_t<termination_result_t>::success (operation.result));
        }
        if (!operation.started) {
            operation.metric_started = std::chrono::steady_clock::now ();
            operation.started = true;
            operation.deadline = deadline;
            operation.deadline_at = std::chrono::system_clock::now () + deadline;
            _state->delays.request_stop (std::chrono::steady_clock::now () + deadline);
            _state->runtime_state.store (framework_runtime_state_t::draining,
                                         std::memory_order_release);
            _state->draining->store (true, std::memory_order_release);
            auto *state = _state.get ();
            operation.worker = std::thread ([state] { run_shared_shutdown (*state); });
        }
        waiter = std::make_shared<detail::app_state_t::termination_waiter_t> ();
        task = waiter->task ();
        operation.waiters.push_back (waiter);
    }
    waiter->arm (wait_cancellation);
    return task;
}

void app_t::run_shared_shutdown (detail::app_state_t &state) noexcept
{
    const auto started_at = std::chrono::steady_clock::now ();
    const auto deadline_at = state.delays.stop_deadline ().value ();
    auto monitoring = detail::monitoring_runtime_t (state.monitoring);
    auto emit_state = [&] (detail::drain_state_t drain_state) {
        try {
            monitoring.publish_drain (detail::drain_event_t{drain_state});
        }
        catch (...) {
        }
    };

    emit_state (detail::drain_state_t::draining);

    shutdown_progress_t result = shutdown_completed_t{};
    bool force_state_emitted = false;
    auto force = [&] (shutdown_force_reason_t reason) {
        if (!std::holds_alternative<shutdown_completed_t> (result))
            return;
        result = shutdown_forced_t{reason};
        if (reason == shutdown_force_reason_t::deadline_exceeded)
            state.termination_operation.cleanup_cancellation.request_stop ();
        if (!force_state_emitted) {
            force_state_emitted = true;
            emit_state (detail::drain_state_t::force_stopping);
        }
    };

    while (std::chrono::steady_clock::now () < deadline_at) {
        bool relocation_active = false;
        {
            std::lock_guard lock (state.relocation_operation.mutex);
            relocation_active =
              state.relocation_operation.started && !state.relocation_operation.terminal;
        }
        if (!relocation_active)
            break;
        zlink::framework::runtime::wait_poll_interval (std::chrono::milliseconds (1));
    }
    {
        std::lock_guard lock (state.relocation_operation.mutex);
        if (state.relocation_operation.started && !state.relocation_operation.terminal) {
            force (shutdown_force_reason_t::deadline_exceeded);
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result))
        state.runtime_state.store (framework_runtime_state_t::draining, std::memory_order_release);

    for (const auto &service : state.hosted_services) {
        if (auto *lifecycle = detail::lifecycle_of (service.get ()))
            lifecycle->seal_application_dispatch ();
    }

    auto publish_draining_markers = [&] {
        bool marker_published = false;
        try {
            auto provider = state.services.build_provider ();
            if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                location_runtime->get ().set_draining (true);
                marker_published = location_runtime->get ().republish_peer_rows_draining ();
            } else {
                marker_published = true;
            }
        }
        catch (...) {
            marker_published = false;
        }
        while (std::chrono::steady_clock::now () < deadline_at && !marker_published) {
            std::this_thread::sleep_until (std::min (
              deadline_at, std::chrono::steady_clock::now () + std::chrono::milliseconds (100)));
            try {
                auto provider = state.services.build_provider ();
                if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                    marker_published = location_runtime->get ().republish_peer_rows_draining ();
                }
            }
            catch (...) {
            }
        }
        if (!marker_published) {
            force (shutdown_force_reason_t::teardown_failed);
            return;
        }
    };

    /* Publish the draining state before waiting for accepted callbacks. A
     * caller must stop selecting this host while an already accepted handler
     * is still completing. */
    if (std::holds_alternative<shutdown_completed_t> (result)) {
        publish_draining_markers ();
        if (std::holds_alternative<shutdown_completed_t> (result)
            && !publish_mesh_descriptor_state (state, framework_runtime_state_t::draining)) {
            force (shutdown_force_reason_t::teardown_failed);
        }
    }

    /* Admission is sealed before this barrier. Each callback accepted before
     * the seal owns a pending/active count until its terminal reply or send
     * completion, so a normal request completion cannot close its Spot. */
    if (std::holds_alternative<shutdown_completed_t> (result)) {
        auto outbound_pending = [&state] () -> bool {
            try {
                return detail::channel_runtime_t::from (state.zlink.message_bus ()).pending_count ()
                       > 0;
            }
            catch (...) {
            }
            return false;
        };
        while (outbound_pending () && std::chrono::steady_clock::now () < deadline_at)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        if (outbound_pending ()) {
            force (shutdown_force_reason_t::deadline_exceeded);
        } else {
            for (const auto &service : state.hosted_services) {
                auto *lifecycle = detail::lifecycle_of (service.get ());
                if (lifecycle && !lifecycle->wait_for_accepted_callbacks_until (deadline_at)) {
                    force (shutdown_force_reason_t::deadline_exceeded);
                    break;
                }
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        for (const auto &service : state.hosted_services) {
            auto *lifecycle = detail::lifecycle_of (service.get ());
            if (lifecycle && !lifecycle->wait_for_accepted_callbacks_until (deadline_at)) {
                force (shutdown_force_reason_t::deadline_exceeded);
                break;
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        for (const auto &service : state.hosted_services) {
            auto *lifecycle = detail::lifecycle_of (service.get ());
            if (lifecycle && !lifecycle->drain_sessions_until (deadline_at)) {
                force (std::chrono::steady_clock::now () >= deadline_at
                         ? shutdown_force_reason_t::deadline_exceeded
                         : shutdown_force_reason_t::teardown_failed);
                break;
            }
        }
    }

    if (std::holds_alternative<shutdown_completed_t> (result)) {
        bool spots_closed = true;
        try {
            for (const auto &snapshot : detail::spot_node_runtime_t::snapshots (state.zlink)) {
                auto runtime = detail::spot_node_runtime_t::from (state.zlink, snapshot.name);
                if (runtime
                    && !runtime->close_all_spots (
                      deadline_at, state.termination_operation.deadline_at.value (),
                      state.termination_operation.cleanup_cancellation)) {
                    spots_closed = false;
                    state.monitoring->diagnostics_logger.error (
                      "Host shutdown Spot teardown did not complete");
                    break;
                }
            }
        }
        catch (const std::exception &error) {
            spots_closed = false;
            state.monitoring->diagnostics_logger.error (
              std::string ("Host shutdown Spot teardown failed: ") + error.what ());
        }
        if (!spots_closed)
            force (std::chrono::steady_clock::now () >= deadline_at
                     ? shutdown_force_reason_t::deadline_exceeded
                     : shutdown_force_reason_t::teardown_failed);
    }

    {
        try {
            auto provider = state.services.build_provider ();
            if (auto location_runtime = provider.get<runtime::location_runtime_t> ()) {
                if (!location_runtime->get ().cleanup_owner (deadline_at)) {
                    force (std::chrono::steady_clock::now () >= deadline_at
                             ? shutdown_force_reason_t::deadline_exceeded
                             : shutdown_force_reason_t::teardown_failed);
                }
            }
        }
        catch (...) {
            force (shutdown_force_reason_t::teardown_failed);
        }
    }

    const bool force_stopped = std::holds_alternative<shutdown_forced_t> (result);
    if (force_stopped) {
        // User cleanup returns cooperatively: framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md#2-execution-gate--owner-처리-순서
        detail::cancel_handler_waits (state.handlers);
        for (const auto &service : state.hosted_services) {
            if (auto *lifecycle = detail::lifecycle_of (service.get ())) {
                lifecycle->cancel_execution_waits ();
                lifecycle->visit_relocation_nodes ([] (const auto &node) {
                    if (node)
                        node->cancel_pending_dispatch_waits ();
                });
                if (!lifecycle->wait_for_accepted_callbacks_until (deadline_at))
                    force (shutdown_force_reason_t::teardown_failed);
            }
        }
        /* graceful-drain-handoff §7: active sessions receive the reason code
         * before forced teardown; the notification is bounded and never
         * blocks the terminal result indefinitely. */
        try {
            for (const auto &service : state.hosted_services) {
                if (auto *lifecycle = detail::lifecycle_of (service.get ())) {
                    lifecycle->force_close_sessions ();
                }
            }
        }
        catch (...) {
        }
    }
    if (!force_stopped)
        emit_state (detail::drain_state_t::stopped);
    termination_reason_t terminal_reason = termination_reason_t::none;
    if (const auto *forced = std::get_if<shutdown_forced_t> (&result)) {
        switch (forced->reason) {
            case shutdown_force_reason_t::deadline_exceeded:
                terminal_reason = termination_reason_t::deadline_exceeded;
                break;
            case shutdown_force_reason_t::teardown_failed:
                terminal_reason = termination_reason_t::teardown_failed;
                break;
        }
    }
    termination_result_t terminal{force_stopped ? termination_outcome_t::force_stopped
                                                : termination_outcome_t::stopped,
                                  terminal_reason};
    bool teardown_completed = true;
    {
        std::unique_lock lock (state.termination_teardown_mutex);
        state.stop_requested.store (true, std::memory_order_release);
        state.termination_teardown_changed.notify_all ();
        if (state.run_active) {
            teardown_completed = state.termination_teardown_changed.wait_until (
              lock, deadline_at, [&] { return state.teardown_complete; });
        }
    }
    if (!teardown_completed) {
        state.termination_operation.cleanup_cancellation.request_stop ();
        std::unique_lock lock (state.termination_teardown_mutex);
        state.termination_teardown_changed.wait (lock, [&] { return state.teardown_complete; });
    }
    if (!teardown_completed && terminal.outcome == termination_outcome_t::stopped) {
        terminal.outcome = termination_outcome_t::force_stopped;
        terminal.reason = termination_reason_t::deadline_exceeded;
    }
    runtime::runtime_metrics_t metrics (state.monitoring);
    if (metrics.enabled ()) {
        metrics.histogram (
          "zlink.host.shutdown.duration", "s",
          std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                         - state.termination_operation.metric_started)
            .count (),
          {{"outcome",
            terminal.outcome == termination_outcome_t::stopped ? "stopped" : "force_stopped"}});
        if (terminal.outcome == termination_outcome_t::force_stopped)
            metrics.counter ("zlink.host.shutdown.forced", "{operation}", 1,
                             {{"reason", terminal.reason == termination_reason_t::deadline_exceeded
                                           ? "deadline_exceeded"
                                           : "teardown_failed"}});
    }
    state.complete_termination (terminal);
}

} // namespace zlink::framework
