/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/execution_wait.hpp"
#include "../support/execution_signal.hpp"
#include "../support/execution_serial_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/streams/session_serial_executor.hpp"
#include "../support/scope_exit.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{
bool verify_serial_queue_lanes_are_unbounded ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_options_t options;
    options.lifecycle_burst_limit = 2;
    options.owner_time_budget = std::chrono::milliseconds::zero ();
    serial_execution_queue_t queue (executor, options);

    std::mutex gate;
    std::condition_variable changed;
    bool first_entered = false;
    bool release_first = false;
    std::vector<std::string> order;
    // Releases the first item and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::lock_guard lock (gate);
            release_first = true;
        }
        changed.notify_all ();
        queue.drain ();
    });

    if (!queue.try_post (
          "application-first",
          [&] {
              {
                  std::lock_guard lock (gate);
                  first_entered = true;
                  changed.notify_all ();
              }
              std::unique_lock lock (gate);
              changed.wait (lock, [&] { return release_first; });
              order.push_back ("application-first");
          },
          serial_work_options_t{serial_work_lane_t::application, 256})) {
        return false;
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return first_entered; })) {
            release_first = true;
            changed.notify_all ();
            return false;
        }
    }

    const auto application = serial_work_options_t{serial_work_lane_t::application, 256};
    const auto lifecycle = serial_work_options_t{serial_work_lane_t::lifecycle, 256};
    if (!queue.try_post (
          "application-second", [&] { order.push_back ("application-second"); }, application)
        || !queue.try_post (
          "lifecycle-first", [&] { order.push_back ("lifecycle-first"); }, lifecycle)
        || !queue.try_post (
          "lifecycle-second", [&] { order.push_back ("lifecycle-second"); }, lifecycle)
        || !queue.try_post (
          "application-over-former-byte-limit",
          [&] { order.push_back ("application-over-former-byte-limit"); }, application)
        || !queue.try_post (
          "lifecycle-over-former-message-limit",
          [&] { order.push_back ("lifecycle-over-former-message-limit"); }, lifecycle)) {
        return false;
    }
    if (queue.pending_count (serial_work_lane_t::application) != 3
        || queue.pending_count (serial_work_lane_t::lifecycle) != 3
        || queue.pending_bytes () != 1536) {
        return false;
    }

    {
        std::lock_guard lock (gate);
        release_first = true;
        changed.notify_all ();
    }
    queue.drain ();
    return order.size () == 6 && order.front () == "application-first"
           && std::find (order.begin (), order.end (), "application-over-former-byte-limit")
                != order.end ()
           && std::find (order.begin (), order.end (), "lifecycle-over-former-message-limit")
                != order.end ()
           && queue.pending_count () == 0 && queue.pending_bytes () == 0;
}

bool verify_transferred_owner_reservation_is_continuous_until_terminal ()
{
    using namespace zlink::framework::runtime;
    using namespace zlink::framework::runtime::mesh;

    offload_executor_t executor (1);
    serial_execution_queue_options_t options;
    serial_execution_queue_t queue (executor, options);

    std::mutex gate;
    std::condition_variable changed;
    std::optional<serial_execution_queue_t::async_completion_t> complete_active;
    std::optional<serial_execution_queue_t::async_completion_t> complete_transferred;
    bool active_entered = false;
    bool transferred_entered = false;
    if (!queue.try_post_async ("owner-cap-active", [&] (auto complete) {
            std::lock_guard lock (gate);
            complete_active.emplace (std::move (complete));
            active_entered = true;
            changed.notify_all ();
        })) {
        return false;
    }
    // Completes the active turn and drains before the locals above are destroyed.
    scope_exit_t drain_active ([&] {
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return active_entered; });
        }
        (*complete_active) ([] {});
        queue.drain ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return active_entered; })) {
            return false;
        }
    }

    service_mailbox_t mailbox;
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::vector<std::uint8_t> (512, std::uint8_t{0x2a})));
    auto record = service_mailbox_record_t::from_parts (
      "transferred-owner", service_mailbox_domain_t::application, std::move (parts));
    if (!mailbox.try_enqueue (std::move (record))) {
        (*complete_active) ([] {});
        queue.drain ();
        return false;
    }
    auto claim =
      mailbox.try_claim_owner (service_mailbox_domain_t::application, "transferred-owner", 1, 4096);
    if (!claim || claim->claimed_messages != 1
        || claim->claimed_bytes <= serial_execution_queue_t::fixed_work_byte_cost) {
        (*complete_active) ([] {});
        queue.drain ();
        return false;
    }
    std::atomic_int transfer_count{0};
    std::atomic_bool transfer_released{false};
    const serial_work_options_t transferred{
      serial_work_lane_t::application, claim->claimed_bytes,
      [&mailbox, claim, &transfer_count, &transfer_released] {
          transfer_count.fetch_add (1, std::memory_order_relaxed);
          transfer_released.store (mailbox.release (*claim), std::memory_order_release);
      }};
    const auto transferred_posted = queue.try_post_async (
      "transferred-owner-reservation",
      [&] (auto complete) {
          std::lock_guard lock (gate);
          complete_transferred.emplace (std::move (complete));
          transferred_entered = true;
          changed.notify_all ();
      },
      transferred);
    if (!transferred_posted) {
        (*complete_active) ([] {});
        queue.drain ();
        return false;
    }
    // Completes both turns and drains before the mailbox and counters above are destroyed.
    scope_exit_t drain_transferred ([&] {
        (*complete_active) ([] {});
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return transferred_entered; });
        }
        (*complete_transferred) ([] {});
        queue.drain ();
    });
    if (transfer_count.load (std::memory_order_relaxed) != 1
        || !transfer_released.load (std::memory_order_acquire) || mailbox.release (*claim)
        || queue.pending_count (serial_work_lane_t::application) != 2
        || queue.pending_bytes ()
             != serial_execution_queue_t::fixed_work_byte_cost + claim->claimed_bytes) {
        (*complete_active) ([] {});
        {
            std::unique_lock lock (gate);
            (void) changed.wait_for (lock, std::chrono::seconds (1),
                                     [&] { return transferred_entered; });
        }
        if (complete_transferred)
            (*complete_transferred) ([] {});
        queue.drain ();
        return false;
    }
    (*complete_active) ([] {});
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                               [&] { return transferred_entered; })) {
            return false;
        }
    }
    if (queue.pending_count (serial_work_lane_t::application) != 1
        || queue.pending_bytes () != claim->claimed_bytes) {
        (*complete_transferred) ([] {});
        queue.drain ();
        return false;
    }
    (*complete_transferred) ([] {});
    queue.drain ();
    return transfer_count.load (std::memory_order_relaxed) == 1 && queue.pending_count () == 0
           && queue.pending_bytes () == 0;
}

bool verify_transferred_owner_reservation_shares_unbounded_lifecycle_lane ()
{
    using namespace zlink::framework::runtime;

    offload_executor_t executor (1);
    serial_execution_queue_options_t options;
    serial_execution_queue_t queue (executor, options);

    std::mutex gate;
    std::condition_variable changed;
    std::optional<serial_execution_queue_t::async_completion_t> complete_active;
    bool active_entered = false;
    const auto lifecycle = serial_work_options_t{serial_work_lane_t::lifecycle,
                                                 serial_execution_queue_t::fixed_work_byte_cost};
    if (!queue.try_post_async (
          "lifecycle-owner-cap-active",
          [&] (auto complete) {
              std::lock_guard lock (gate);
              complete_active.emplace (std::move (complete));
              active_entered = true;
              changed.notify_all ();
          },
          lifecycle)) {
        return false;
    }
    // Completes the active turn and drains before the locals above are destroyed.
    scope_exit_t drain_active ([&] {
        {
            std::unique_lock lock (gate);
            changed.wait (lock, [&] { return active_entered; });
        }
        if (complete_active)
            (*complete_active) ([] {});
        queue.drain ();
    });
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return active_entered; }))
            return false;
    }

    const auto release_active = [&] {
        if (complete_active) {
            (*complete_active) ([] {});
            complete_active.reset ();
        }
        queue.drain ();
    };
    std::atomic_int ordinary_runs{0};
    if (!queue.try_post (
          "ordinary-lifecycle-queued",
          [&] { ordinary_runs.fetch_add (1, std::memory_order_release); }, lifecycle)) {
        release_active ();
        return false;
    }

    std::atomic_int transfer_count{0};
    std::atomic_int transferred_runs{0};
    constexpr auto transferred_byte_cost = serial_execution_queue_t::fixed_work_byte_cost * 2;
    const serial_work_options_t transferred{
      serial_work_lane_t::lifecycle, transferred_byte_cost,
      [&] { transfer_count.fetch_add (1, std::memory_order_release); }};
    if (!queue.try_post (
          "transferred-lifecycle-owner-reservation",
          [&] { transferred_runs.fetch_add (1, std::memory_order_release); }, transferred)) {
        release_active ();
        return false;
    }
    if (transfer_count.load (std::memory_order_acquire) != 1
        || queue.pending_count (serial_work_lane_t::lifecycle) != 3
        || queue.pending_bytes ()
             != 2 * serial_execution_queue_t::fixed_work_byte_cost + transferred_byte_cost) {
        release_active ();
        return false;
    }

    release_active ();
    return transfer_count.load (std::memory_order_acquire) == 1
           && ordinary_runs.load (std::memory_order_acquire) == 1
           && transferred_runs.load (std::memory_order_acquire) == 1 && queue.pending_count () == 0
           && queue.pending_bytes () == 0;
}

bool verify_serial_queue_owner_time_budget ()
{
    using namespace zlink::framework::runtime;

    // Declared before the executor and queues so queued work never outlives them.
    std::vector<int> batched_order;
    std::mutex gate;
    std::condition_variable changed;
    bool first_entered = false;
    bool release_first = false;
    std::vector<std::string> order;
    offload_executor_t executor (1);
    serial_execution_queue_options_t options;
    options.owner_time_budget = std::chrono::milliseconds (100);
    serial_execution_queue_t batched_queue (executor, options);

    for (int value = 1; value <= 4; ++value) {
        if (!batched_queue.try_post (
              "budget-batch", [&batched_order, value] { batched_order.push_back (value); })) {
            return false;
        }
    }
    batched_queue.drain ();
    if (batched_order != std::vector<int>{1, 2, 3, 4} || batched_queue.pending_count () != 0) {
        return false;
    }

    options.owner_time_budget = std::chrono::milliseconds (1);
    serial_execution_queue_t overloaded_owner (executor, options);
    serial_execution_queue_t other_owner (executor, options);

    if (!overloaded_owner.try_post ("overloaded-0", [&] {
            std::unique_lock lock (gate);
            first_entered = true;
            changed.notify_all ();
            changed.wait (lock, [&] { return release_first; });
            order.push_back ("overloaded-0");
        })) {
        return false;
    }
    {
        std::unique_lock lock (gate);
        if (!changed.wait_for (lock, std::chrono::seconds (1), [&] { return first_entered; })) {
            release_first = true;
            changed.notify_all ();
            return false;
        }
    }
    for (int index = 1; index < 8; ++index) {
        if (!overloaded_owner.try_post ("overloaded-rest", [&, index] {
                std::lock_guard lock (gate);
                order.push_back ("overloaded-" + std::to_string (index));
            })) {
            std::lock_guard lock (gate);
            release_first = true;
            changed.notify_all ();
            return false;
        }
    }
    if (!other_owner.try_post ("other-owner", [&] {
            std::lock_guard lock (gate);
            order.push_back ("other");
        })) {
        std::lock_guard lock (gate);
        release_first = true;
        changed.notify_all ();
        return false;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (3));
    {
        std::lock_guard lock (gate);
        release_first = true;
    }
    changed.notify_all ();
    overloaded_owner.drain ();
    other_owner.drain ();

    std::lock_guard lock (gate);
    const auto other = std::find (order.begin (), order.end (), "other");
    const auto last = std::find (order.begin (), order.end (), "overloaded-7");
    return other != order.end () && last != order.end () && other < last
           && overloaded_owner.pending_count () == 0 && other_owner.pending_count () == 0;
}
bool verify_serial_executor_submission_paths_select_queues ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::per_actor_spot ());
    session_serial_executor_t session (fixture.worker);
    std::atomic_uint observed{0};
    const auto mark = [&observed] (unsigned bit) {
        observed.fetch_or (bit, std::memory_order_release);
    };

    const auto spot = fixture.serial.execute_spot ("submission-spot", [&] { mark (1U); });
    const auto actor =
      fixture.serial.execute_actor ("actor-a", "submission-actor", [&] (auto complete) {
          mark (2U);
          complete ([] {});
      });
    const auto timer =
      fixture.serial.execute_timer ("tick", "submission-timer", [&] (auto complete) {
          mark (4U);
          complete ([] {});
      });
    const auto lifecycle =
      fixture.serial.execute_lifecycle ("submission-lifecycle", [&] { mark (8U); });
    const auto application = session.execute_application ([&] (auto complete) {
        mark (16U);
        complete ([] {});
    });

    const auto ran = wait_until ([&] { return observed.load (std::memory_order_acquire) == 31U; });
    fixture.spot_queue->drain ();
    fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
    fixture.serial.timer_queue ("tick")->drain ();
    session.drain ();
    return spot && actor && timer && lifecycle && application && ran;
}

bool verify_per_actor_two_actors_overlap ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::per_actor_spot ());
    serial_test_blocker_t actor_a;
    serial_test_blocker_t actor_b;
    const auto accepted_a = fixture.serial.execute_actor ("actor-a", "overlap-a", actor_a.work ());
    const auto accepted_b = fixture.serial.execute_actor ("actor-b", "overlap-b", actor_b.work ());
    const auto overlapped = actor_a.wait_for_entry () && actor_b.wait_for_entry ();
    actor_a.release ();
    actor_b.release ();
    fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
    fixture.serial.actor_executor ("actor-b")->queue ()->drain ();
    return accepted_a && accepted_b && overlapped;
}

bool verify_spot_wide_two_actors_are_serial ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    serial_test_blocker_t actor_a;
    serial_test_signal_t actor_b_started;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        actor_a.release ();
        fixture.drain ();
    });
    const auto accepted_a = fixture.serial.execute_actor ("actor-a", "wide-a", actor_a.work ());
    if (!accepted_a || !actor_a.wait_for_entry ())
        return false;

    const auto accepted_b = fixture.serial.execute_actor ("actor-b", "wide-b", [&] (auto complete) {
        actor_b_started.set ();
        complete ([] {});
    });
    const auto ran_too_early = actor_b_started.wait_for (std::chrono::milliseconds (50));
    actor_a.release ();
    const auto ran_after_release = actor_b_started.wait_for ();
    fixture.spot_queue->drain ();
    fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
    fixture.serial.actor_executor ("actor-b")->queue ()->drain ();
    return accepted_b && !ran_too_early && ran_after_release;
}
bool verify_same_actor_fifo_in_both_modes ()
{
    using namespace zlink::framework::runtime;

    for (const auto execution :
         {spot_lane_execution_t::per_actor, spot_lane_execution_t::spot_wide}) {
        const auto policy = execution == spot_lane_execution_t::per_actor
                              ? serial_lane_policy_t::per_actor_spot ()
                              : serial_lane_policy_t::spot_wide ();
        serial_executor_test_fixture_t fixture (policy);
        serial_test_signal_t second_started;
        std::mutex order_gate;
        std::vector<std::string> order;
        const auto record = [&] (std::string event) {
            std::lock_guard lock (order_gate);
            order.push_back (std::move (event));
        };
        serial_test_blocker_t first;
        // Releases the blocker and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            first.release ();
            fixture.drain ();
        });

        const auto accepted_first = fixture.serial.execute_actor (
          "actor-a", "fifo-first", first.work ([&] { record ("first:start"); }));
        if (!accepted_first || !first.wait_for_entry ())
            return false;
        const auto accepted_second =
          fixture.serial.execute_actor ("actor-a", "fifo-second", [&] (auto complete) {
              record ("second");
              second_started.set ();
              complete ([] {});
          });
        const auto ran_too_early = second_started.wait_for (std::chrono::milliseconds (50));
        first.release ([&] { record ("first:end"); });
        const auto ran_after_release = second_started.wait_for ();
        fixture.spot_queue->drain ();
        fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
        std::lock_guard lock (order_gate);
        if (!accepted_second || ran_too_early || !ran_after_release
            || order != std::vector<std::string>{"first:start", "first:end", "second"}) {
            return false;
        }
    }
    return true;
}

bool verify_per_actor_timer_names_overlap_and_keep_fifo ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::per_actor_spot ());
    serial_test_signal_t second_tick_started;
    serial_test_signal_t beat_started;
    std::mutex order_gate;
    std::vector<std::string> order;
    const auto record = [&] (std::string event) {
        std::lock_guard lock (order_gate);
        order.push_back (std::move (event));
    };
    serial_test_blocker_t first_tick;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        first_tick.release ();
        fixture.drain ();
    });

    const auto accepted_first = fixture.serial.execute_timer (
      "tick", "tick-first", first_tick.work ([&] { record ("tick:first:start"); }));
    if (!accepted_first || !first_tick.wait_for_entry ())
        return false;
    const auto accepted_second =
      fixture.serial.execute_timer ("tick", "tick-second", [&] (auto complete) {
          record ("tick:second");
          second_tick_started.set ();
          complete ([] {});
      });
    const auto accepted_beat = fixture.serial.execute_timer ("beat", "beat", [&] (auto complete) {
        record ("beat");
        beat_started.set ();
        complete ([] {});
    });
    const auto beat_overlapped = beat_started.wait_for ();
    const auto same_name_ran_early = second_tick_started.is_set ();
    first_tick.release ([&] { record ("tick:first:end"); });
    const auto second_ran = second_tick_started.wait_for ();
    fixture.serial.timer_queue ("tick")->drain ();
    fixture.serial.timer_queue ("beat")->drain ();

    std::lock_guard lock (order_gate);
    return accepted_second && accepted_beat && beat_overlapped && !same_name_ran_early && second_ran
           && order
                == std::vector<std::string>{"tick:first:start", "beat", "tick:first:end",
                                            "tick:second"};
}

bool verify_actor_mailbox_backlog_is_accepted_in_both_modes ()
{
    using namespace zlink::framework::runtime;

    for (const auto execution :
         {spot_lane_execution_t::per_actor, spot_lane_execution_t::spot_wide}) {
        const auto policy = execution == spot_lane_execution_t::per_actor
                              ? serial_lane_policy_t::per_actor_spot ()
                              : serial_lane_policy_t::spot_wide ();
        serial_executor_test_fixture_t fixture (policy);
        serial_execution_queue_options_t actor_options;
        auto full_actor_queue = std::make_shared<serial_execution_queue_t> (
          *fixture.worker, actor_options, serial_execution_queue_t::error_handler_t{},
          serial_lane_policy_t::actor_delivery ());
        fixture.serial.replace_actor_queue ("actor-full", full_actor_queue);

        serial_test_signal_t same_ran;
        serial_test_signal_t other_ran;
        serial_test_blocker_t first;
        // Releases the blocker and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            first.release ();
            fixture.drain ();
        });
        const auto accepted_first =
          fixture.serial.execute_actor ("actor-full", "capacity-first", first.work ());
        if (!accepted_first || !first.wait_for_entry ())
            return false;
        const auto same_actor_accepted =
          fixture.serial.execute_actor ("actor-full", "queued-same-actor", [&] (auto complete) {
              same_ran.set ();
              complete ([] {});
          });
        const auto other_actor_accepted =
          fixture.serial.execute_actor ("actor-open", "capacity-other", [&] (auto complete) {
              other_ran.set ();
              complete ([] {});
          });
        first.release ();
        const auto same_completed = same_ran.wait_for ();
        const auto other_completed = other_ran.wait_for ();
        full_actor_queue->drain ();
        fixture.spot_queue->drain ();
        fixture.serial.actor_executor ("actor-open")->queue ()->drain ();
        if (!same_actor_accepted || !same_completed || !other_actor_accepted || !other_completed)
            return false;
    }
    return true;
}

bool verify_spot_wide_large_and_small_payloads_are_queued ()
{
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    serial_execution_queue_options_t actor_options;
    auto actor_queue = std::make_shared<serial_execution_queue_t> (
      *fixture.worker, actor_options, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::actor_delivery ());
    fixture.serial.replace_actor_queue ("actor-a", actor_queue);

    serial_test_signal_t second_large_ran;
    serial_test_signal_t small_ran;
    serial_test_blocker_t first;
    // Releases the blocker and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        first.release ();
        fixture.drain ();
    });
    const serial_work_options_t large{serial_work_lane_t::application, 60};
    const serial_work_options_t small{serial_work_lane_t::application, 10};
    const auto accepted_first =
      fixture.serial.execute_actor ("actor-a", "large-first", first.work (), large);
    if (!accepted_first || !first.wait_for_entry ())
        return false;
    const auto second_large_accepted = fixture.serial.execute_actor (
      "actor-a", "large-second",
      [&] (auto complete) {
          second_large_ran.set ();
          complete ([] {});
      },
      large);
    const auto small_accepted = fixture.serial.execute_actor (
      "actor-a", "small",
      [&] (auto complete) {
          small_ran.set ();
          complete ([] {});
      },
      small);
    first.release ();
    const auto second_large_completed = second_large_ran.wait_for ();
    const auto small_completed = small_ran.wait_for ();
    actor_queue->drain ();
    fixture.spot_queue->drain ();
    return second_large_accepted && second_large_completed && small_accepted && small_completed;
}

bool verify_spot_wide_upper_queue_accepts_former_boundaries ()
{
    using namespace zlink::framework::runtime;

    const auto second_submission_runs = [] (std::size_t payload_bytes) {
        serial_execution_queue_options_t spot_options;
        serial_executor_test_fixture_t fixture (serial_lane_policy_t::spot_wide (), spot_options);

        serial_execution_queue_options_t actor_options;
        for (const auto *actor_id : {"actor-a", "actor-b"}) {
            fixture.serial.replace_actor_queue (actor_id,
                                                std::make_shared<serial_execution_queue_t> (
                                                  *fixture.worker, actor_options,
                                                  serial_execution_queue_t::error_handler_t{},
                                                  serial_lane_policy_t::actor_delivery ()));
        }

        serial_test_signal_t second_ran;
        std::atomic_bool rejected{false};
        serial_test_blocker_t first;
        // Releases the blocker and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            first.release ();
            fixture.drain ();
        });
        const auto accepted_first =
          fixture.serial.execute_actor ("actor-a", "upper-first", first.work (),
                                        serial_work_options_t{serial_work_lane_t::application, 1});
        if (!accepted_first || !first.wait_for_entry ())
            return false;
        const auto admitted_to_actor_queue = fixture.serial.execute_actor (
          "actor-b", "upper-second",
          [&] (auto complete) {
              second_ran.set ();
              complete ([] {});
          },
          serial_work_options_t{serial_work_lane_t::application, payload_bytes}, false,
          [&] { rejected.store (true, std::memory_order_release); });
        first.release ();
        const auto completed = second_ran.wait_for ();
        fixture.spot_queue->drain ();
        fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
        fixture.serial.actor_executor ("actor-b")->queue ()->drain ();
        return admitted_to_actor_queue && completed && !rejected.load (std::memory_order_acquire);
    };

    return second_submission_runs (1) && second_submission_runs (10'000);
}
bool verify_serial_lane_policies ()
{
    using namespace zlink::framework::runtime;
    const auto entry = serial_lane_policy_t::entry_spot ();
    const auto spot_wide = serial_lane_policy_t::spot_wide ();
    const auto per_actor = serial_lane_policy_t::per_actor_spot ();
    const auto session = serial_lane_policy_t::session ();
    const auto actor_delivery = serial_lane_policy_t::actor_delivery ();

    const auto *entry_spot = std::get_if<spot_lane_policy_t> (&entry.value ());
    const auto *wide_spot = std::get_if<spot_lane_policy_t> (&spot_wide.value ());
    const auto *actor_spot = std::get_if<spot_lane_policy_t> (&per_actor.value ());
    return entry_spot && entry_spot->execution == spot_lane_execution_t::entry
           && entry_spot->lifecycle == spot_lane_lifecycle_t::active && wide_spot
           && wide_spot->execution == spot_lane_execution_t::spot_wide
           && wide_spot->lifecycle == spot_lane_lifecycle_t::active && actor_spot
           && actor_spot->execution == spot_lane_execution_t::per_actor
           && actor_spot->lifecycle == spot_lane_lifecycle_t::active
           && std::holds_alternative<session_lane_policy_t> (session.value ())
           && std::holds_alternative<actor_delivery_lane_policy_t> (actor_delivery.value ())
           && !entry.allows_turn_yield () && spot_wide.allows_turn_yield ()
           && !per_actor.allows_turn_yield () && !session.allows_turn_yield ()
           && !actor_delivery.allows_turn_yield ()
           && std::is_constructible_v<spot_lane_policy_t, spot_lane_execution_t,
                                      spot_lane_lifecycle_t>
           && std::is_constructible_v<session_lane_policy_t, session_lane_lifecycle_t>
           && !std::is_constructible_v<session_lane_policy_t, spot_lane_lifecycle_t>
           && !std::is_constructible_v<actor_delivery_lane_policy_t, spot_lane_lifecycle_t>
           && !std::is_constructible_v<actor_delivery_lane_policy_t, session_lane_lifecycle_t>;
}
}
