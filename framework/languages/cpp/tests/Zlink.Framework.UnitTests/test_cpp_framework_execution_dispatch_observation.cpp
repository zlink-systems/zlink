/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include <array>
#include <limits>
#include "../support/execution_cases.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/diagnostics/runtime_observation.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/fanout/fanout_location_runtime.hpp"
#include "../support/scope_exit.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;

#ifndef ZLINK_SERIAL_EXECUTION_CONFORMANCE_PATH
#error "serial execution conformance fixture path is required"
#endif

#ifndef ZLINK_RUNTIME_OBSERVATION_CONFORMANCE_PATH
#error "runtime observation conformance fixture path is required"
#endif

namespace zlink::framework::test::execution
{

const nlohmann::json &serial_execution_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_SERIAL_EXECUTION_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error ("serial execution conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

const nlohmann::json &runtime_observation_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_RUNTIME_OBSERVATION_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "runtime observation conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

bool verify_common_dispatch_limits ()
{
    using namespace zlink::framework::runtime;
    const auto &fixture = serial_execution_fixture ();
    const auto &limits = fixture.at ("limits");
    const serial_execution_queue_options_t queue_options;
    const receive_batch_budget_t receive_options;
    return fixture.at ("fixture") == "zlink.framework.serial-execution"
           && fixture.at ("version") == 1
           && queue_options.owner_time_budget
                == std::chrono::milliseconds (
                  limits.at ("ownerTimeBudgetMilliseconds").get<std::int64_t> ())
           && queue_options.owner_time_budget == dispatch_limits::owner_time_budget
           && queue_options.lifecycle_burst_limit == limits.at ("lifecycleBurstLimit")
           && queue_options.lifecycle_burst_limit == dispatch_limits::lifecycle_burst_limit
           && serial_execution_queue_t::fixed_work_byte_cost == limits.at ("fixedWorkByteCost")
           && serial_execution_queue_t::fixed_work_byte_cost
                == dispatch_limits::fixed_work_byte_cost
           && receive_options.max_messages == dispatch_limits::receive_batch_messages
           && receive_options.max_bytes == dispatch_limits::receive_batch_bytes
           && receive_options.max_elapsed == dispatch_limits::receive_batch_time;
}

bool verify_fixture_accounting_boundaries ()
{
    using namespace zlink::framework::runtime;
    try {
        const auto &fixture = serial_execution_fixture ();
        for (const auto &scenario : fixture.at ("accountingScenarios")) {
            const auto lane_name = scenario.at ("lane").get<std::string> ();
            const auto lane = lane_name == "application" ? serial_work_lane_t::application
                              : lane_name == "lifecycle"
                                ? serial_work_lane_t::lifecycle
                                : throw std::runtime_error ("unknown serial work lane");
            const auto payload_bytes =
              scenario.at ("retainedPayloadBytesPerWork").get<std::size_t> ();
            const auto accepted = scenario.at ("acceptedWorkCount").get<std::size_t> ();
            const auto byte_cost = serial_execution_queue_t::fixed_work_byte_cost + payload_bytes;
            if (accepted == 0 || !scenario.at ("runningWorkConsumesReservation").get<bool> ()) {
                return false;
            }

            offload_executor_t executor (2);
            serial_execution_queue_t queue (executor, serial_execution_queue_options_t{});
            std::mutex gate;
            std::condition_variable changed;
            bool active = false;
            std::optional<serial_execution_queue_t::async_completion_t> finish_active;
            const auto options = serial_work_options_t{lane, byte_cost};
            if (!queue.try_post_async (
                  scenario.at ("name").get<std::string> (),
                  [&] (auto complete) {
                      std::lock_guard lock (gate);
                      finish_active.emplace (std::move (complete));
                      active = true;
                      changed.notify_all ();
                  },
                  options)) {
                return false;
            }
            // Completes the active turn and drains before the locals above are destroyed.
            scope_exit_t drain_on_exit ([&] {
                serial_execution_queue_t::async_completion_t release;
                {
                    std::unique_lock lock (gate);
                    changed.wait (lock, [&] { return active; });
                    if (finish_active)
                        release = std::move (*finish_active);
                }
                if (release)
                    release ([] {});
                queue.drain ();
            });
            {
                std::unique_lock lock (gate);
                if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return active; })) {
                    return false;
                }
            }
            for (std::size_t index = 1; index < accepted; ++index) {
                if (!queue.try_post ("fixture-boundary", [] {}, options))
                    return false;
            }
            if (!queue.try_post (
                  "fixture-former-boundary", [] {}, options)
                || queue.pending_count (lane) != accepted + 1
                || queue.pending_bytes () != (accepted + 1) * byte_cost) {
                return false;
            }

            serial_execution_queue_t::async_completion_t finish;
            {
                std::lock_guard lock (gate);
                finish = std::move (*finish_active);
            }
            finish ([] {});
            queue.drain ();
            if (queue.pending_count () != 0 || queue.pending_bytes () != 0
                || !queue.try_post ("fixture-after-terminal", [] {}, options)) {
                return false;
            }
            queue.drain ();
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool verify_fixture_arbitration_and_owner_isolation ()
{
    using namespace zlink::framework::runtime;
    try {
        const auto &fixture = serial_execution_fixture ();
        const auto &invariants = fixture.at ("dispatchInvariants");
        if (!invariants.at ("applicationAndLifecycleUseDistinctFifos").get<bool> ()
            || !invariants.at ("applicationAndLifecycleHaveIndependentAdmission").get<bool> ()
            || !invariants.at ("emptyToNonEmptySchedulesImmediately").get<bool> ()
            || !invariants.at ("pollingIsNotAProgressMechanism").get<bool> ()
            || invariants.at ("implicitInlineExecution").get<bool> ()
            || !invariants.at ("resumeAfterYieldUsesNewTurn").get<bool> ()) {
            return false;
        }
        const auto same_owner = [&] (std::string_view target) -> const nlohmann::json & {
            const auto &rules = fixture.at ("sameOwnerCalls");
            const auto found = std::find_if (rules.begin (), rules.end (), [&] (const auto &rule) {
                return rule.at ("target").template get_ref<const std::string &> () == target;
            });
            if (found == rules.end ())
                throw std::runtime_error ("same-owner fixture rule is missing");
            return *found;
        };
        const auto &self_actor = same_owner ("selfActor");
        const auto &same_spot = same_owner ("sameSpot");
        const auto &member_actor = same_owner ("differentMemberActorOnSameSpot");
        const auto &different_owner = same_owner ("differentOwner");
        if (self_actor.at ("async") != "invalidOperation"
            || self_actor.at ("yield") != "invalidOperation"
            || self_actor.at ("actorClaimAfterYield") != "retained"
            || same_spot.at ("async") != "invalidOperation"
            || same_spot.at ("yield") != "resumeOnNewTurn"
            || member_actor.at ("async") != "invalidOperation"
            || member_actor.at ("yield") != "resumeOnNewTurn"
            || member_actor.at ("actorClaimAfterYield") != "retained"
            || different_owner.at ("async") != "awaitWithoutGateRelease"
            || different_owner.at ("yield") != "resumeOnNewTurn") {
            return false;
        }

        const auto &scenario = fixture.at ("arbitrationScenarios").at (0);
        const auto applications = scenario.at ("applicationInput").get<std::vector<std::string>> ();
        const auto lifecycle = scenario.at ("lifecycleInput").get<std::vector<std::string>> ();
        const auto expected = scenario.at ("expectedSelection").get<std::vector<std::string>> ();
        if (lifecycle.empty ())
            return false;

        offload_executor_t arbitration_executor (2);
        serial_execution_queue_t arbitration_queue (arbitration_executor,
                                                    serial_execution_queue_options_t{});
        std::mutex order_gate;
        std::condition_variable order_changed;
        std::vector<std::string> order;
        // L1 keeps its turn while the other inputs are accepted. A pending
        // asynchronous lifecycle item returns its turn (gate §7), so L1 blocks
        // inside the turn instead.
        bool first_started = false;
        bool release_first = false;
        // Releases L1 and drains before the locals above are destroyed.
        scope_exit_t drain_arbitration ([&] {
            {
                std::lock_guard lock (order_gate);
                release_first = true;
            }
            order_changed.notify_all ();
            arbitration_queue.drain ();
        });
        if (!arbitration_queue.try_post (
              lifecycle.front (),
              [&] {
                  std::unique_lock lock (order_gate);
                  order.push_back (lifecycle.front ());
                  first_started = true;
                  order_changed.notify_all ();
                  order_changed.wait (lock, [&] { return release_first; });
              },
              {serial_work_lane_t::lifecycle, serial_execution_queue_t::fixed_work_byte_cost})) {
            return false;
        }
        {
            std::unique_lock lock (order_gate);
            if (!order_changed.wait_for (lock, std::chrono::seconds (1),
                                         [&] { return first_started; })) {
                return false;
            }
        }
        for (const auto &name : applications) {
            if (!arbitration_queue.try_post (name,
                                             [&, name] {
                                                 std::lock_guard lock (order_gate);
                                                 order.push_back (name);
                                             },
                                             {serial_work_lane_t::application,
                                              serial_execution_queue_t::fixed_work_byte_cost})) {
                return false;
            }
        }
        for (auto item = std::next (lifecycle.begin ()); item != lifecycle.end (); ++item) {
            if (!arbitration_queue.try_post (*item,
                                             [&, name = *item] {
                                                 std::lock_guard lock (order_gate);
                                                 order.push_back (name);
                                             },
                                             {serial_work_lane_t::lifecycle,
                                              serial_execution_queue_t::fixed_work_byte_cost})) {
                return false;
            }
        }
        {
            std::lock_guard lock (order_gate);
            release_first = true;
        }
        order_changed.notify_all ();
        arbitration_queue.drain ();
        {
            std::lock_guard lock (order_gate);
            if (order != expected)
                return false;
        }

        offload_executor_t shared_executor (2);
        serial_execution_queue_t owner_a (shared_executor, serial_execution_queue_options_t{});
        serial_execution_queue_t owner_b (shared_executor, serial_execution_queue_options_t{});
        std::mutex progress_gate;
        std::condition_variable progress_changed;
        std::optional<serial_execution_queue_t::async_completion_t> release_owner_a;
        bool owner_a_lifecycle_ran = false;
        std::size_t owner_b_runs = 0;
        bool inline_execution = false;
        const auto caller_thread = std::this_thread::get_id ();
        if (!owner_a.try_post_async ("owner-a-active", [&] (auto complete) {
                std::lock_guard lock (progress_gate);
                release_owner_a.emplace (std::move (complete));
                progress_changed.notify_all ();
            })) {
            return false;
        }
        // Completes the active owner A turn and drains before the locals above are destroyed.
        scope_exit_t drain_owners ([&] {
            serial_execution_queue_t::async_completion_t release;
            {
                std::unique_lock lock (progress_gate);
                progress_changed.wait (lock, [&] { return release_owner_a.has_value (); });
                release = std::move (*release_owner_a);
            }
            if (release)
                release ([] {});
            owner_a.drain ();
            owner_b.drain ();
        });
        {
            std::unique_lock lock (progress_gate);
            if (!progress_changed.wait_for (lock, std::chrono::seconds (1),
                                            [&] { return release_owner_a.has_value (); })) {
                return false;
            }
        }
        const auto application_capacity =
          fixture.at ("limits").at ("application").at ("messageCapacity").get<std::size_t> ();
        for (std::size_t index = 1; index < application_capacity; ++index) {
            if (!owner_a.try_post ("owner-a-saturated", [] {}))
                return false;
        }
        if (!owner_a.try_post ("owner-a-former-capacity", [] {})
            || !owner_a.try_post (
              "owner-a-lifecycle",
              [&] {
                  std::lock_guard lock (progress_gate);
                  owner_a_lifecycle_ran = true;
              },
              {serial_work_lane_t::lifecycle, serial_execution_queue_t::fixed_work_byte_cost})) {
            return false;
        }
        const auto record_owner_b_progress = [&] {
            std::lock_guard lock (progress_gate);
            inline_execution = inline_execution || std::this_thread::get_id () == caller_thread;
            ++owner_b_runs;
            progress_changed.notify_all ();
        };
        if (!owner_b.try_post (
              "owner-b-lifecycle", record_owner_b_progress,
              {serial_work_lane_t::lifecycle, serial_execution_queue_t::fixed_work_byte_cost})
            || !owner_b.try_post ("owner-b-application", record_owner_b_progress)) {
            return false;
        }
        {
            std::unique_lock lock (progress_gate);
            if (!progress_changed.wait_for (lock, std::chrono::seconds (1),
                                            [&] { return owner_b_runs == 2; })
                || owner_a_lifecycle_ran || inline_execution) {
                return false;
            }
        }
        serial_execution_queue_t::async_completion_t finish_owner_a;
        {
            std::lock_guard lock (progress_gate);
            finish_owner_a = std::move (*release_owner_a);
        }
        finish_owner_a ([] {});
        owner_a.drain ();
        owner_b.drain ();
        std::lock_guard lock (progress_gate);
        return owner_a_lifecycle_ran;
    }
    catch (...) {
        return false;
    }
}
bool verify_runtime_observation_loss_and_terminal_retention ()
{
    struct probe_status_t
    {
        int sequence = 0;
        std::string source;
        std::string value;
    };
    using observer_t =
      zlink::framework::observation_detail::runtime_observer_state_t<probe_status_t>;

    std::mutex gate;
    std::condition_variable changed;
    bool first_entered = false;
    bool release_first = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> received;
    auto observer = std::make_shared<observer_t> (1, [&] (const auto &observed) {
        std::unique_lock lock (gate);
        received.push_back (observed);
        if (observed.status.sequence == 1) {
            first_entered = true;
            changed.notify_all ();
            changed.wait (lock, [&] { return release_first; });
        }
        changed.notify_all ();
    });
    observer->start ();
    observer->enqueue ("A", probe_status_t{1});
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return first_entered; })) {
            release_first = true;
            changed.notify_all ();
            lock.unlock ();
            observer->close ();
            return false;
        }
    }
    observer->enqueue ("A", probe_status_t{2});
    observer->enqueue ("A", probe_status_t{3});
    observer->enqueue ("A", probe_status_t{4}, true);
    {
        std::lock_guard lock (gate);
        release_first = true;
        changed.notify_all ();
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                               [&] { return received.size () == 2; })) {
            return false;
        }
        if (received[0].status.sequence != 1 || received[1].status.sequence != 4
            || received[1].loss.coalesced_count != 2
            || received[1].loss.discarded_terminal_count != 0) {
            return false;
        }
    }
    observer->close ();

    std::mutex terminal_gate;
    std::condition_variable terminal_changed;
    bool terminal_first_entered = false;
    bool terminal_release_first = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> terminals;
    auto terminal_observer = std::make_shared<observer_t> (1, [&] (const auto &observed) {
        std::unique_lock lock (terminal_gate);
        terminals.push_back (observed);
        if (observed.status.sequence == 10) {
            terminal_first_entered = true;
            terminal_changed.notify_all ();
            terminal_changed.wait (lock, [&] { return terminal_release_first; });
        }
        terminal_changed.notify_all ();
    });
    terminal_observer->start ();
    terminal_observer->enqueue ("A", probe_status_t{10});
    {
        std::unique_lock lock (terminal_gate);
        if (!terminal_changed.wait_for (lock, std::chrono::seconds (1),
                                        [&] { return terminal_first_entered; })) {
            terminal_release_first = true;
            terminal_changed.notify_all ();
            lock.unlock ();
            terminal_observer->close ();
            return false;
        }
    }
    terminal_observer->enqueue ("A", probe_status_t{11}, true);
    terminal_observer->enqueue ("A", probe_status_t{12}, true);
    {
        std::lock_guard lock (terminal_gate);
        terminal_release_first = true;
        terminal_changed.notify_all ();
    }
    {
        std::unique_lock lock (terminal_gate);
        if (!terminal_changed.wait_for (lock, std::chrono::seconds (1),
                                        [&] { return terminals.size () == 2; })) {
            return false;
        }
        if (terminals[1].status.sequence != 12 || terminals[1].loss.discarded_terminal_count != 1) {
            return false;
        }
    }
    terminal_observer->close ();

    const auto &fixture = runtime_observation_fixture ();
    if (fixture.at ("fixture") != "zlink.framework.runtime-observation"
        || fixture.at ("version") != 1)
        return false;
    const auto scenario = std::find_if (
      fixture.at ("scenarios").begin (), fixture.at ("scenarios").end (),
      [] (const auto &candidate) {
          return candidate.at ("name") == "multi-source-retention-and-terminal-overflow";
      });
    if (scenario == fixture.at ("scenarios").end ())
        return false;

    std::mutex fixture_gate;
    std::condition_variable fixture_changed;
    bool fixture_blocked = false;
    bool release_fixture = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> fixture_received;
    auto fixture_observer = std::make_shared<observer_t> (
      scenario->at ("terminalCapacity").get<std::size_t> (), [&] (const auto &observed) {
          std::unique_lock lock (fixture_gate);
          fixture_received.push_back (observed);
          if (observed.status.sequence == -1) {
              fixture_blocked = true;
              fixture_changed.notify_all ();
              fixture_changed.wait (lock, [&] { return release_fixture; });
          }
          fixture_changed.notify_all ();
      });
    fixture_observer->start ();
    fixture_observer->enqueue ("blocker", probe_status_t{-1, "blocker", "blocker"});
    {
        std::unique_lock lock (fixture_gate);
        if (!fixture_changed.wait_for (lock, std::chrono::seconds (1),
                                       [&] { return fixture_blocked; })) {
            release_fixture = true;
            fixture_changed.notify_all ();
            lock.unlock ();
            fixture_observer->close ();
            return false;
        }
    }
    for (const auto &operation : scenario->at ("operations")) {
        const auto source = operation.at ("source").get<std::string> ();
        fixture_observer->enqueue (source,
                                   probe_status_t{operation.at ("sequence").get<int> (), source,
                                                  operation.at ("value").get<std::string> ()},
                                   operation.at ("kind") == "terminal");
    }
    {
        std::lock_guard lock (fixture_gate);
        release_fixture = true;
        fixture_changed.notify_all ();
    }

    const auto expected_count = 1 + scenario->at ("expectedTerminalFifo").size ()
                                + scenario->at ("expectedRetainedIntermediateBySource").size ();
    {
        std::unique_lock lock (fixture_gate);
        if (!fixture_changed.wait_for (lock, std::chrono::seconds (2),
                                       [&] { return fixture_received.size () == expected_count; }))
            return false;

        std::size_t received_index = 1;
        for (const auto &expected : scenario->at ("expectedTerminalFifo")) {
            const auto &actual = fixture_received.at (received_index++).status;
            if (actual.source != expected.at ("source").get<std::string> ()
                || actual.sequence != expected.at ("sequence").get<int> ()
                || actual.value != expected.at ("value").get<std::string> ())
                return false;
        }
        for (const auto &[source, expected] :
             scenario->at ("expectedRetainedIntermediateBySource").items ()) {
            const auto &actual = fixture_received.at (received_index++).status;
            if (actual.source != source || actual.sequence != expected.at ("sequence").get<int> ()
                || actual.value != expected.at ("value").get<std::string> ())
                return false;
        }
        const auto &loss = fixture_received.back ().loss;
        if (loss.coalesced_count
              != std::stoull (
                scenario->at ("expectedLoss").at ("coalescedIntermediateCount").get<std::string> ())
            || loss.discarded_terminal_count
                 != std::stoull (
                   scenario->at ("expectedLoss").at ("discardedTerminalCount").get<std::string> ()))
            return false;
    }
    fixture_observer->close ();

    const auto saturation_scenario =
      std::find_if (fixture.at ("scenarios").begin (), fixture.at ("scenarios").end (),
                    [] (const auto &candidate) {
                        return candidate.at ("name") == "loss-counters-saturate-independently";
                    });
    if (saturation_scenario == fixture.at ("scenarios").end ())
        return false;
    auto coalesced_loss =
      static_cast<std::uint64_t> (std::stoull (saturation_scenario->at ("initialLoss")
                                                 .at ("coalescedIntermediateCount")
                                                 .get<std::string> ()));
    auto discarded_loss = static_cast<std::uint64_t> (std::stoull (
      saturation_scenario->at ("initialLoss").at ("discardedTerminalCount").get<std::string> ()));
    for (int index = 0;
         index
         < saturation_scenario->at ("increments").at ("coalescedIntermediateCount").get<int> ();
         ++index) {
        zlink::framework::observation_detail::increment_runtime_observation_loss (coalesced_loss);
    }
    for (int index = 0;
         index < saturation_scenario->at ("increments").at ("discardedTerminalCount").get<int> ();
         ++index) {
        zlink::framework::observation_detail::increment_runtime_observation_loss (discarded_loss);
    }
    if (coalesced_loss
          != std::stoull (saturation_scenario->at ("expectedLoss")
                            .at ("coalescedIntermediateCount")
                            .get<std::string> ())
        || discarded_loss
             != std::stoull (saturation_scenario->at ("expectedLoss")
                               .at ("discardedTerminalCount")
                               .get<std::string> ()))
        return false;

    std::mutex lifetime_gate;
    std::condition_variable lifetime_changed;
    bool lifetime_blocked = false;
    bool release_lifetime = false;
    std::vector<zlink::framework::observed_status_t<probe_status_t>> lifetime_received;
    auto lifetime_observer = std::make_shared<observer_t> (1, [&] (const auto &observed) {
        std::unique_lock lock (lifetime_gate);
        lifetime_received.push_back (observed);
        if (observed.status.sequence == -1) {
            lifetime_blocked = true;
            lifetime_changed.notify_all ();
            lifetime_changed.wait (lock, [&] { return release_lifetime; });
        }
        lifetime_changed.notify_all ();
    });
    lifetime_observer->start ();
    lifetime_observer->enqueue ("blocker", probe_status_t{-1, "blocker", "blocker"});
    {
        std::unique_lock lock (lifetime_gate);
        if (!lifetime_changed.wait_for (lock, std::chrono::seconds (1),
                                        [&] { return lifetime_blocked; })) {
            release_lifetime = true;
            lifetime_changed.notify_all ();
            lock.unlock ();
            lifetime_observer->close ();
            return false;
        }
    }
    lifetime_observer->enqueue ("A", probe_status_t{1, "A", "terminal-A"}, true);
    lifetime_observer->enqueue ("A", probe_status_t{2, "A", "suppressed-A"});
    lifetime_observer->enqueue ("B", probe_status_t{3, "B", "terminal-B"}, true);
    lifetime_observer->enqueue ("A", probe_status_t{4, "A", "restarted-A"});
    {
        std::lock_guard lock (lifetime_gate);
        release_lifetime = true;
        lifetime_changed.notify_all ();
    }
    {
        std::unique_lock lock (lifetime_gate);
        if (!lifetime_changed.wait_for (lock, std::chrono::seconds (2),
                                        [&] { return lifetime_received.size () == 3; }))
            return false;
        if (lifetime_received[1].status.value != "terminal-B"
            || lifetime_received[2].status.value != "restarted-A"
            || lifetime_received[2].loss.coalesced_count != 1
            || lifetime_received[2].loss.discarded_terminal_count != 1)
            return false;
    }
    lifetime_observer->close ();

    std::mutex shared_gate;
    std::condition_variable shared_changed;
    bool slow_entered = false;
    bool release_slow = false;
    bool fast_delivered = false;
    auto slow_observer = std::make_shared<observer_t> (1, [&] (const auto &) {
        std::unique_lock lock (shared_gate);
        slow_entered = true;
        shared_changed.notify_all ();
        shared_changed.wait (lock, [&] { return release_slow; });
    });
    auto fast_observer = std::make_shared<observer_t> (1, [&] (const auto &) {
        std::lock_guard lock (shared_gate);
        fast_delivered = true;
        shared_changed.notify_all ();
    });
    slow_observer->start ();
    fast_observer->start ();
    slow_observer->enqueue ("slow", probe_status_t{1});
    {
        std::unique_lock lock (shared_gate);
        if (!shared_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return slow_entered; })) {
            release_slow = true;
            shared_changed.notify_all ();
            lock.unlock ();
            slow_observer->close ();
            fast_observer->close ();
            return false;
        }
    }
    fast_observer->enqueue ("fast", probe_status_t{1});
    {
        std::unique_lock lock (shared_gate);
        if (!shared_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return fast_delivered; })) {
            release_slow = true;
            shared_changed.notify_all ();
            lock.unlock ();
            slow_observer->close ();
            fast_observer->close ();
            return false;
        }
        release_slow = true;
        shared_changed.notify_all ();
    }
    slow_observer->close ();
    fast_observer->close ();
    return true;
}

int verify_application_queue_limits_and_metrics ()
{
    {
        using queue_t = zlink::framework::runtime::application_job_queue_t;
        queue_t queue (
          {zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 4, 1});

        auto first = queue.try_reserve_supply ();
        if (!first) {
            return 100;
        }
        first->mark_queued ();

        std::mutex grants_mutex;
        std::vector<int> grants;
        std::optional<queue_t::permit_t> second;
        std::optional<queue_t::permit_t> third;
        auto cancelled = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              if (permit) {
                  std::lock_guard lock (grants_mutex);
                  grants.push_back (1);
              }
          },
          zlink::framework::runtime::application_job_queue_t::origin_t::remote);
        auto second_waiter = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              std::lock_guard lock (grants_mutex);
              if (permit) {
                  grants.push_back (2);
                  second.emplace (std::move (*permit));
              }
          },
          zlink::framework::runtime::application_job_queue_t::origin_t::remote);
        auto third_waiter = queue.wait_for_supply (
          [&] (std::optional<queue_t::permit_t> permit) {
              std::lock_guard lock (grants_mutex);
              if (permit) {
                  grants.push_back (3);
                  third.emplace (std::move (*permit));
              }
          },
          zlink::framework::runtime::application_job_queue_t::origin_t::remote);
        if (!cancelled.cancel ()) {
            return 101;
        }

        first->release_for_handler_entry ();
        {
            std::lock_guard lock (grants_mutex);
            if (grants != std::vector<int>{2} || !second) {
                return 102;
            }
        }
        second->mark_queued ();
        second->release_for_handler_entry ();
        second.reset ();
        {
            std::lock_guard lock (grants_mutex);
            if (grants != std::vector<int> ({2, 3}) || !third) {
                return 103;
            }
        }
        third->release_without_handler ();
        third.reset ();

        const auto before_reset = queue.snapshot ();
        if (before_reset.reserved_supply_permits != 0 || before_reset.queued_application_jobs != 0
            || before_reset.permits_in_use != 0 || before_reset.peak_permits_in_use != 1
            || before_reset.capacity_waiters != 0 || before_reset.capacity_wait_count != 3) {
            return 104;
        }
        queue.reset_metrics ();
        const auto after_reset = queue.snapshot ();
        if (after_reset.peak_permits_in_use != after_reset.permits_in_use
            || after_reset.capacity_wait_count != 0
            || after_reset.capacity_wait_duration != std::chrono::nanoseconds::zero ()) {
            return 105;
        }
        (void) second_waiter;
        (void) third_waiter;
    }

    {
        using namespace zlink::framework;
        using namespace zlink::framework::runtime;

        const application_job_queue_processor_limits_t constrained{
          std::uint32_t{16}, std::uint32_t{8}, std::uint32_t{4}, std::uint32_t{6},
          std::uint32_t{12}};
        if (effective_application_job_processors (constrained) != 4) {
            return 106;
        }

        const std::array profiles{
          application_job_queue_profile_t::compact, application_job_queue_profile_t::low_latency,
          application_job_queue_profile_t::balanced, application_job_queue_profile_t::throughput};
        const std::array<std::uint32_t, 4> multipliers{32, 64, 128, 256};
        for (const auto processors : {4u, 8u, 16u}) {
            for (std::size_t index = 0; index < profiles.size (); ++index) {
                const auto resolved = resolve_application_job_queue_configuration (
                  profiles[index], std::nullopt,
                  {processors, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
                if (resolved.effective_processor_count != processors
                    || resolved.effective_max_queued_application_jobs
                         != multipliers[index] * processors) {
                    return 107;
                }
            }
        }

        for (const auto manual : {std::uint32_t{1}, static_cast<std::uint32_t> (
                                                      std::numeric_limits<std::int32_t>::max ())}) {
            const auto resolved = resolve_application_job_queue_configuration (
              application_job_queue_profile_t::balanced, manual, constrained);
            if (resolved.configured_manual_max != manual
                || resolved.effective_max_queued_application_jobs != manual) {
                return 108;
            }
        }

        bool zero_rejected = false;
        try {
            (void) resolve_application_job_queue_configuration (
              application_job_queue_profile_t::balanced, std::uint32_t{0}, constrained);
        }
        catch (const framework_exception_t &) {
            zero_rejected = true;
        }
        bool overflow_rejected = false;
        try {
            (void) calculate_application_job_queue_limit (
              application_job_queue_profile_t::throughput,
              std::numeric_limits<std::uint32_t>::max ());
        }
        catch (const framework_exception_t &) {
            overflow_rejected = true;
        }
        if (!zero_rejected || !overflow_rejected) {
            return 109;
        }

        const std::array<std::string_view, 14> expected_metric_names{
          "zlink.host.core_hwm.effective_budget",
          "zlink.host.core_hwm.applied",
          "zlink.host.core_hwm.accounted",
          "zlink.host.core_hwm.completion_accounted",
          "zlink.host.core_hwm.blocked_ratio",
          "zlink.host.application_job_queue.limit",
          "zlink.host.application_job_queue.jobs",
          "zlink.host.application_job_queue.capacity_waiters",
          "zlink.host.application_job_queue.capacity_waits",
          "zlink.host.application_job_queue.capacity_wait_duration",
          "zlink.host.application_job_queue.pressure_state",
          "zlink.host.application_job_queue.pressure_transitions",
          "zlink.host.application_job_queue.pause_duration",
          "zlink.host.application_job_queue.flow_state_config_failures"};
        for (std::size_t index = 0; index < expected_metric_names.size (); ++index) {
            if (host_capacity_metric_catalog[index].name != expected_metric_names[index]) {
                return 110;
            }
        }
        if (host_capacity_metric_catalog[2].unit != "By"
            || !host_capacity_metric_catalog[2].state_label
            || !host_capacity_metric_catalog[3].state_label
            || host_capacity_metric_catalog[4].unit != "{ppm}"
            || host_capacity_metric_catalog[6].unit != "{job}"
            || !host_capacity_metric_catalog[6].state_label
            || host_capacity_metric_catalog[8].kind != detail::metric_instrument_kind_t::counter
            || host_capacity_metric_catalog[8].unit != "{wait}"
            || host_capacity_metric_catalog[9].kind != detail::metric_instrument_kind_t::counter
            || host_capacity_metric_catalog[9].unit != "s"
            || !host_capacity_metric_catalog[10].state_label
            || host_capacity_metric_catalog[10].unit != "{state}"
            || host_capacity_metric_catalog[11].kind != detail::metric_instrument_kind_t::counter
            || !host_capacity_metric_catalog[11].state_label
            || host_capacity_metric_catalog[12].kind != detail::metric_instrument_kind_t::observable
            || !host_capacity_metric_catalog[12].state_label
            || host_capacity_metric_catalog[12].unit != "s"
            || host_capacity_metric_catalog[13].kind != detail::metric_instrument_kind_t::counter
            || host_capacity_metric_catalog[13].state_label) {
            return 111;
        }
    }
    return 0;
}
}
