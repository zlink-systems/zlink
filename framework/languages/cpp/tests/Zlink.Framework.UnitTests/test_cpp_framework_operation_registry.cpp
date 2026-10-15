/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/operations/exactly_once_table.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <atomic>
#include <barrier>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace foundation = zlink::framework::runtime::foundation;
namespace runtime = zlink::framework::runtime;

#ifndef ZLINK_COMPLETION_TERMINAL_CONFORMANCE_PATH
#error "completion terminal conformance fixture path is required"
#endif

const nlohmann::json &completion_terminal_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_COMPLETION_TERMINAL_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "completion terminal conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

foundation::call_id_t id (std::uint8_t value)
{
    return foundation::call_id_t{0, value};
}

foundation::operation_registry_t::clock_t::time_point injected_now{};
foundation::operation_registry_t::clock_t::duration injected_step{};

// Each read advances the injected clock by the configured step.
foundation::operation_registry_t::clock_t::time_point read_injected_clock ()
{
    const auto now = injected_now;
    injected_now += injected_step;
    return now;
}

template <typename Predicate> bool wait_until (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!predicate ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::yield ();
    }
    return true;
}

int main ()
{
    const auto &fixture = completion_terminal_fixture ();
    assert (fixture.at ("fixture") == "zlink.framework.completion-terminal");
    assert (fixture.at ("version") == 1);
    assert (fixture.at ("limits").at ("operationIdBits") == 128);
    assert (fixture.at ("limits").at ("replyRouteIdBits") == 64);
    assert (!fixture.at ("identityInvariants").at ("operationIdAllZeroAllowed").get<bool> ());
    assert (fixture.at ("registryInvariants").at ("terminalWinner") == "atomicTake");
    assert (fixture.at ("registryInvariants")
              .at ("completionDispatchedOutsideRegistryGate")
              .get<bool> ());
    assert (fixture.at ("registryInvariants").at ("completionUsesNewExecutionTurn").get<bool> ());
    assert (!fixture.at ("registryInvariants").at ("terminalTombstoneRetention").get<bool> ());
    runtime::exactly_once_table_t<foundation::call_id_t, int, runtime::call_id_hash_t> completions;
    assert (completions.reserve (id (10)));
    assert (completions.claim (id (10)).state == runtime::exactly_once_claim_state::pending);
    assert (completions.complete (id (10), 7));
    const auto completed = completions.claim (id (10));
    assert (completed.state == runtime::exactly_once_claim_state::completed && completed.value
            && *completed.value == 7);
    assert (!completions.complete (id (10), 8));
    int completion = 0;
    assert (completions.take (id (10), completion) && completion == 7);
    assert (!completions.take (id (10), completion));
    assert (completions.claim (id (11)).state == runtime::exactly_once_claim_state::claimed);
    assert (completions.erase (id (11)));
    assert (completions.claim (id (11)).state == runtime::exactly_once_claim_state::claimed);
    assert (completions.complete (id (12), 9));
    const auto unreserved_completion = completions.claim (id (12));
    assert (unreserved_completion.state == runtime::exactly_once_claim_state::completed
            && unreserved_completion.value && *unreserved_completion.value == 9);

    foundation::operation_registry_t registry;
    const auto now = foundation::operation_registry_t::clock_t::now ();
    {
        foundation::operation_registry_t ordered;
        std::array<bool, 128> active{};
        active.fill (true);
        for (std::size_t index = 0; index < active.size (); ++index)
            assert (ordered.register_operation (id (static_cast<std::uint8_t> (index + 1)),
                                                now + std::chrono::microseconds (index ^ 63),
                                                [] (auto, auto) {}));
        const auto first_tick = *ordered.next_deadline ();
        assert (first_tick >= now && first_tick <= now + std::chrono::milliseconds (1));
        // Hot-path E3: next_deadline is the bitmap-selected occupied 1ms wheel tick.
        for (std::size_t step = 0; step < active.size (); ++step) {
            std::optional<foundation::operation_registry_t::clock_t::time_point> expected;
            for (std::size_t index = 0; index < active.size (); ++index) {
                const auto deadline = now + std::chrono::microseconds (index ^ 63);
                if (active[index] && (!expected || deadline < *expected))
                    expected = deadline;
            }
            if (expected)
                expected = first_tick
                           + std::chrono::ceil<std::chrono::milliseconds> (*expected - first_tick);
            assert (ordered.next_deadline () == expected);
            const auto removed = step * 37 % active.size ();
            assert (ordered.unregister (id (static_cast<std::uint8_t> (removed + 1))));
            active[removed] = false;
        }
        assert (!ordered.next_deadline ());
    }
    {
        foundation::operation_registry_t wheel;
        const auto deadline =
          foundation::operation_registry_t::clock_t::now () + std::chrono::milliseconds (513);
        assert (wheel.register_operation (id (1), deadline, [] (auto, auto) {}));
        const auto first_tick = *wheel.next_deadline ();
        // Hot-path E3: remaining rounds prevent early expiry; the deadline rounds up by at most 1ms.
        assert (wheel.expire (first_tick) == 0);
        assert (*wheel.next_deadline () == first_tick + std::chrono::milliseconds (256));
        assert (wheel.expire (first_tick + std::chrono::milliseconds (256)) == 0);
        const auto expiry_tick = first_tick + std::chrono::milliseconds (512);
        assert (expiry_tick >= deadline && expiry_tick <= deadline + std::chrono::milliseconds (1));
        assert (wheel.expire (deadline - std::chrono::nanoseconds (1)) == 0);
        assert (wheel.expire (expiry_tick) == 1);
        assert (!wheel.next_deadline ());
    }
    {
        injected_now = {};
        injected_step = {};
        foundation::operation_registry_t yielding (&read_injected_clock);
        const auto origin = injected_now;
        for (std::size_t index = 0; index < 8; ++index)
            assert (yielding.register_operation (foundation::call_id_t{3, index + 1},
                                                 origin + std::chrono::milliseconds (257),
                                                 [] (auto, auto) {}));
        assert (yielding.register_operation (
          foundation::call_id_t{3, 9}, origin + std::chrono::milliseconds (1),
          [] (auto terminal, auto) {
              assert (terminal == foundation::operation_terminal_t::timed_out);
          }));
        // §4.2: every slot visit, a rounds-only one included, reads the management budget
        // counted from entry; four reads exhaust it.
        injected_step =
          std::chrono::duration_cast<foundation::operation_registry_t::clock_t::duration> (
            runtime::dispatch_limits::receive_batch_time)
          / 4;
        const auto tick = origin + std::chrono::milliseconds (1);
        assert (yielding.expire (tick) == 0);
        assert (yielding.contains (foundation::call_id_t{3, 9}));
        std::size_t turns = 1;
        while (yielding.expire (tick) == 0)
            assert (++turns < 9);
        assert (!yielding.contains (foundation::call_id_t{3, 9}));
        assert (yielding.size () == 8);
        injected_step = {};
        assert (yielding.expire (origin + std::chrono::milliseconds (257)) == 8);
        assert (yielding.size () == 0);
    }
    {
        injected_now = {};
        injected_step = {};
        foundation::operation_registry_t budgeted (&read_injected_clock);
        constexpr std::size_t pending_count = 10;
        const auto deadline = injected_now + std::chrono::milliseconds (1);
        for (std::size_t index = 0; index < pending_count; ++index)
            assert (budgeted.register_operation (
              foundation::call_id_t{1, index + 1}, deadline, [] (auto terminal, auto) {
                  assert (terminal == foundation::operation_terminal_t::timed_out);
              }));
        const auto tick = *budgeted.next_deadline ();
        // Hot-path E3 / §4.2: four budget reads on the injected clock end the turn after three
        // expiries; the rest of the slot resumes on the next turn.
        injected_step =
          std::chrono::duration_cast<foundation::operation_registry_t::clock_t::duration> (
            runtime::dispatch_limits::receive_batch_time)
          / 4;
        auto expired = budgeted.expire (tick);
        assert (expired == 3);
        assert (budgeted.unregister (foundation::call_id_t{1, expired + 1}));
        assert (
          budgeted.register_operation (foundation::call_id_t{2, 1}, deadline, [] (auto, auto) {}));
        while (budgeted.size () != 0) {
            const auto resumed = budgeted.expire (tick);
            assert (resumed > 0);
            expired += resumed;
        }
        assert (expired == pending_count);
        assert (!budgeted.next_deadline ());
    }
    bool zero_rejected = false;
    try {
        (void) registry.register_operation (foundation::call_id_t{}, now, [] (auto, auto) {});
    }
    catch (const std::invalid_argument &) {
        zero_rejected = true;
    }
    assert (zero_rejected);
    std::atomic_int first_terminal{0};
    std::atomic_int second_terminal{0};
    assert (registry.register_operation (
      id (1), now + std::chrono::seconds (1),
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) {
          assert (terminal == foundation::operation_terminal_t::completed);
          assert (payload == std::vector<std::uint8_t> ({7}));
          first_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (
      id (2), now, [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::timed_out);
          second_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (id (3), now, [] (auto, auto) {}));
    assert (registry.unregister (id (3)));
    assert (registry.complete (id (1), {7}));
    assert (!registry.complete (id (1), {8}));
    // Hot-path E3: the first occupied wheel tick is no later than deadline+1ms.
    assert (registry.expire (now + std::chrono::milliseconds (1)) == 1);
    assert (wait_until ([&] {
        return first_terminal.load (std::memory_order_acquire) == 1
               && second_terminal.load (std::memory_order_acquire) == 1;
    }));
    assert (registry.size () == 0);
    assert (registry.shutdown () == 0);
    assert (!registry.register_operation (id (4), now, [] (auto, auto) {}));

    std::atomic_int failed_terminals{0};
    foundation::operation_registry_t failed_registry;
    std::vector<std::uint8_t> failure_payload;
    assert (failed_registry.register_operation (
      id (4), now + std::chrono::seconds (1),
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) {
          assert (terminal == foundation::operation_terminal_t::transport_failed);
          failure_payload = std::move (payload);
          failed_terminals.fetch_add (1, std::memory_order_release);
      }));
    assert (
      failed_registry.fail (id (4), foundation::operation_terminal_t::transport_failed, {1, 2, 3}));
    assert (wait_until ([&] { return failed_terminals.load (std::memory_order_acquire) == 1; }));
    assert (failure_payload == std::vector<std::uint8_t> ({1, 2, 3}));
    assert (!failed_registry.fail (id (4), foundation::operation_terminal_t::transport_failed));
    assert (failed_terminals.load (std::memory_order_acquire) == 1);

    foundation::operation_registry_t abandoned_registry;
    std::atomic_int abandoned_callbacks{0};
    assert (
      abandoned_registry.register_operation (foundation::call_id_t{8, 1}, now, [&] (auto, auto) {
          abandoned_callbacks.fetch_add (1, std::memory_order_release);
      }));
    assert (abandoned_registry.unregister (foundation::call_id_t{8, 1}));
    assert (!abandoned_registry.unregister (foundation::call_id_t{8, 1}));
    assert (abandoned_registry.register_operation (
      foundation::call_id_t{8, 2}, now, [&] (foundation::operation_terminal_t terminal, auto) {
          assert (terminal == foundation::operation_terminal_t::completed);
          abandoned_callbacks.fetch_add (1, std::memory_order_release);
      }));
    std::atomic_int prepared{0};
    assert (abandoned_registry.complete (
      foundation::call_id_t{8, 2}, {}, [&] { prepared.fetch_add (1, std::memory_order_release); }));
    assert (!abandoned_registry.complete (
      foundation::call_id_t{8, 2}, {}, [&] { prepared.fetch_add (1, std::memory_order_release); }));
    assert (wait_until ([&] { return abandoned_callbacks.load (std::memory_order_acquire) == 1; }));
    assert (prepared.load (std::memory_order_acquire) == 1);

    std::atomic_int shutdown_terminals{0};
    {
        foundation::operation_registry_t scoped_registry;
        assert (scoped_registry.register_operation (
          id (5), now, [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
              throw std::runtime_error ("consumer failure");
          }));
        assert (scoped_registry.register_operation (
          id (6), now, [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
          }));
    }
    assert (wait_until ([&] { return shutdown_terminals.load (std::memory_order_acquire) == 2; }));

    std::atomic_int race_terminals{0};
    std::atomic_int winning_terminal{-1};
    foundation::operation_registry_t race_registry;
    assert (race_registry.register_operation (
      foundation::call_id_t{9, 1}, now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          winning_terminal.store (static_cast<int> (terminal), std::memory_order_release);
          race_terminals.fetch_add (1, std::memory_order_acq_rel);
      }));
    std::barrier race_start (3);
    std::thread reply ([&] {
        race_start.arrive_and_wait ();
        (void) race_registry.complete (foundation::call_id_t{9, 1}, {1});
    });
    std::thread timeout ([&] {
        race_start.arrive_and_wait ();
        (void) race_registry.expire (now + std::chrono::milliseconds (1));
    });
    race_start.arrive_and_wait ();
    reply.join ();
    timeout.join ();
    assert (wait_until ([&] { return race_terminals.load (std::memory_order_acquire) == 1; }));
    assert (race_terminals.load (std::memory_order_acquire) == 1);
    assert (winning_terminal.load (std::memory_order_acquire)
              == static_cast<int> (foundation::operation_terminal_t::completed)
            || winning_terminal.load (std::memory_order_acquire)
                 == static_cast<int> (foundation::operation_terminal_t::timed_out));
    assert (race_registry.size () == 0);

    std::atomic_int cancel_close_terminals{0};
    foundation::operation_registry_t cancel_close_registry;
    assert (cancel_close_registry.register_operation (
      foundation::call_id_t{9, 2}, now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::cancelled
                  || terminal == foundation::operation_terminal_t::shutdown);
          cancel_close_terminals.fetch_add (1, std::memory_order_acq_rel);
      }));
    std::barrier cancel_close_start (3);
    std::thread cancellation ([&] {
        cancel_close_start.arrive_and_wait ();
        (void) cancel_close_registry.cancel (foundation::call_id_t{9, 2});
    });
    std::thread close ([&] {
        cancel_close_start.arrive_and_wait ();
        (void) cancel_close_registry.shutdown ();
    });
    cancel_close_start.arrive_and_wait ();
    cancellation.join ();
    close.join ();
    assert (
      wait_until ([&] { return cancel_close_terminals.load (std::memory_order_acquire) == 1; }));
    assert (cancel_close_terminals.load (std::memory_order_acquire) == 1);

    foundation::operation_registry_t reentrant_registry;
    std::atomic_bool reentered{false};
    std::atomic_bool outer_callback_active{false};
    assert (reentrant_registry.register_operation (
      foundation::call_id_t{9, 3}, now,
      [&] (foundation::operation_terminal_t, std::vector<std::uint8_t>) {
          outer_callback_active.store (true, std::memory_order_release);
          assert (reentrant_registry.register_operation (
            foundation::call_id_t{9, 4}, now,
            [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
                assert (terminal == foundation::operation_terminal_t::cancelled);
                assert (!outer_callback_active.load (std::memory_order_acquire));
                reentered.store (true, std::memory_order_release);
            }));
          assert (reentrant_registry.cancel (foundation::call_id_t{9, 4}));
          outer_callback_active.store (false, std::memory_order_release);
      }));
    assert (reentrant_registry.complete (foundation::call_id_t{9, 3}, {}));
    assert (wait_until ([&] { return reentered.load (std::memory_order_acquire); }));
    assert (reentrant_registry.size () == 0);

    foundation::operation_registry_t turn_registry;
    const auto caller_thread = std::this_thread::get_id ();
    std::atomic_bool used_new_turn{false};
    assert (turn_registry.register_operation (
      foundation::call_id_t{10, 1}, now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::completed);
          used_new_turn.store (std::this_thread::get_id () != caller_thread,
                               std::memory_order_release);
      }));
    assert (turn_registry.complete (foundation::call_id_t{10, 1}, {}));
    assert (wait_until ([&] { return used_new_turn.load (std::memory_order_acquire); }));

    constexpr std::size_t operation_count = 4'200;
    foundation::operation_registry_t dispatcher_registry;
    std::atomic_size_t dispatched{0};
    std::atomic_bool blocked_callback_started{false};
    std::atomic_bool release_blocked_callback{false};
    for (std::size_t index = 1; index <= operation_count; ++index) {
        assert (dispatcher_registry.register_operation (
          foundation::call_id_t{11, index}, now,
          [&, index] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal
                      == (index == 1 ? foundation::operation_terminal_t::completed
                                     : foundation::operation_terminal_t::shutdown));
              if (index == 1) {
                  blocked_callback_started.store (true, std::memory_order_release);
                  while (!release_blocked_callback.load (std::memory_order_acquire)) {
                      std::this_thread::yield ();
                  }
              }
              dispatched.fetch_add (1, std::memory_order_release);
          }));
    }
    assert (dispatcher_registry.complete (foundation::call_id_t{11, 1}, {}));
    assert (wait_until ([&] { return blocked_callback_started.load (std::memory_order_acquire); }));
    std::atomic_bool dispatcher_shutdown_returned{false};
    std::thread dispatcher_shutdown ([&] {
        assert (dispatcher_registry.shutdown () == operation_count - 1);
        dispatcher_shutdown_returned.store (true, std::memory_order_release);
    });

    foundation::operation_registry_t backlog_probe_registry;
    assert (backlog_probe_registry.register_operation (
      foundation::call_id_t{12, 1}, now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::shutdown);
          dispatched.fetch_add (1, std::memory_order_release);
      }));
    assert (!dispatcher_shutdown_returned.load (std::memory_order_acquire));
    release_blocked_callback.store (true, std::memory_order_release);
    dispatcher_shutdown.join ();
    assert (dispatcher_shutdown_returned.load (std::memory_order_acquire));
    assert (
      wait_until ([&] { return dispatched.load (std::memory_order_acquire) == operation_count; }));
    assert (backlog_probe_registry.shutdown () == 1);
    assert (wait_until (
      [&] { return dispatched.load (std::memory_order_acquire) == operation_count + 1; }));

    foundation::operation_registry_t expiry_batch_registry;
    std::atomic_size_t expired_batch_callbacks{0};
    for (std::size_t index = 1; index <= operation_count; ++index) {
        assert (expiry_batch_registry.register_operation (
          foundation::call_id_t{13, index}, now,
          [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::timed_out);
              expired_batch_callbacks.fetch_add (1, std::memory_order_release);
          }));
    }
    // Hot-path E3: a budgeted slot resumes on subsequent management turns.
    std::size_t expired_count = expiry_batch_registry.expire (now + std::chrono::milliseconds (1));
    while (expired_count < operation_count)
        expired_count += expiry_batch_registry.expire (now + std::chrono::milliseconds (1));
    assert (expired_count == operation_count);
    assert (expiry_batch_registry.size () == 0);
    assert (wait_until (
      [&] { return expired_batch_callbacks.load (std::memory_order_acquire) == operation_count; }));
    return 0;
}
