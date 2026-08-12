/* SPDX-License-Identifier: FSL-1.1-ALv2 */

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
    } ();
    return fixture;
}

foundation::call_id_t id (std::uint8_t value)
{
    return foundation::call_id_t{0, value};
}

template <typename Predicate>
bool wait_until (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (5);
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
    assert (fixture.at ("fixture")
              == "zlink.framework.completion-terminal");
    assert (fixture.at ("version") == 1);
    assert (fixture.at ("limits").at ("pendingOperationCapacity")
            == foundation::default_operation_capacity);
    assert (fixture.at ("limits").at ("operationIdBits") == 128);
    assert (fixture.at ("limits").at ("replyRouteIdBits") == 64);
    assert (!fixture.at ("identityInvariants")
               .at ("operationIdAllZeroAllowed")
               .get<bool> ());
    assert (fixture.at ("registryInvariants")
              .at ("terminalWinner")
            == "atomicTake");
    assert (fixture.at ("registryInvariants")
              .at ("completionDispatchedOutsideRegistryGate")
              .get<bool> ());
    assert (fixture.at ("registryInvariants")
              .at ("completionUsesNewExecutionTurn")
              .get<bool> ());
    assert (!fixture.at ("registryInvariants")
               .at ("terminalTombstoneRetention")
               .get<bool> ());
    runtime::exactly_once_table_t<foundation::call_id_t,
                                  int,
                                  runtime::call_id_hash_t>
      completions (2);
    assert (completions.reserve (id (10)));
    assert (completions.claim (id (10)).state
            == runtime::exactly_once_claim_state::pending);
    assert (completions.complete (id (10), 7));
    const auto completed = completions.claim (id (10));
    assert (completed.state == runtime::exactly_once_claim_state::completed
            && completed.value && *completed.value == 7);
    assert (!completions.complete (id (10), 8));
    int completion = 0;
    assert (completions.take (id (10), completion) && completion == 7);
    assert (!completions.take (id (10), completion));
    assert (completions.claim (id (11)).state
            == runtime::exactly_once_claim_state::claimed);
    assert (completions.erase (id (11)));
    assert (completions.claim (id (11)).state
            == runtime::exactly_once_claim_state::claimed);
    assert (completions.complete (id (12), 9));
    const auto unreserved_completion = completions.claim (id (12));
    assert (unreserved_completion.state
              == runtime::exactly_once_claim_state::completed
            && unreserved_completion.value && *unreserved_completion.value == 9);

    foundation::operation_registry_t registry (2);
    const auto now = foundation::operation_registry_t::clock_t::now ();
    bool zero_rejected = false;
    try {
        (void) registry.register_operation (
          foundation::call_id_t{}, now, [] (auto, auto) {});
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
      id (2), now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::timed_out);
          second_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (!registry.register_operation (id (3), now, [] (auto, auto) {}));
    assert (registry.complete (id (1), {7}));
    assert (!registry.complete (id (1), {8}));
    assert (registry.expire (now) == 1);
    assert (wait_until ([&] {
        return first_terminal.load (std::memory_order_acquire) == 1
               && second_terminal.load (std::memory_order_acquire) == 1;
    }));
    assert (registry.size () == 0);
    assert (registry.shutdown () == 0);
    assert (!registry.register_operation (id (4), now, [] (auto, auto) {}));

    std::atomic_int failed_terminals{0};
    foundation::operation_registry_t failed_registry (1);
    std::vector<std::uint8_t> failure_payload;
    assert (failed_registry.register_operation (
      id (4), now + std::chrono::seconds (1),
      [&] (foundation::operation_terminal_t terminal,
           std::vector<std::uint8_t> payload) {
          assert (terminal
                  == foundation::operation_terminal_t::transport_failed);
          failure_payload = std::move (payload);
          failed_terminals.fetch_add (1, std::memory_order_release);
      }));
    assert (failed_registry.fail (
      id (4), foundation::operation_terminal_t::transport_failed,
      {1, 2, 3}));
    assert (wait_until ([&] {
        return failed_terminals.load (std::memory_order_acquire) == 1;
    }));
    assert (failure_payload == std::vector<std::uint8_t> ({1, 2, 3}));
    assert (!failed_registry.fail (
      id (4), foundation::operation_terminal_t::transport_failed));
    assert (failed_terminals.load (std::memory_order_acquire) == 1);

    std::atomic_int shutdown_terminals{0};
    {
        foundation::operation_registry_t scoped_registry (2);
        assert (scoped_registry.register_operation (
          id (5), now,
          [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
              throw std::runtime_error ("consumer failure");
          }));
        assert (scoped_registry.register_operation (
          id (6), now,
          [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
          }));
    }
    assert (wait_until ([&] {
        return shutdown_terminals.load (std::memory_order_acquire) == 2;
    }));

    std::atomic_int race_terminals{0};
    std::atomic_int winning_terminal{-1};
    foundation::operation_registry_t race_registry (1);
    assert (race_registry.register_operation (
      foundation::call_id_t{9, 1}, now,
      [&] (foundation::operation_terminal_t terminal,
           std::vector<std::uint8_t>) {
          winning_terminal.store (static_cast<int> (terminal),
                                  std::memory_order_release);
          race_terminals.fetch_add (1, std::memory_order_acq_rel);
      }));
    std::barrier race_start (3);
    std::thread reply ([&] {
        race_start.arrive_and_wait ();
        (void) race_registry.complete (foundation::call_id_t{9, 1}, {1});
    });
    std::thread timeout ([&] {
        race_start.arrive_and_wait ();
        (void) race_registry.expire (now);
    });
    race_start.arrive_and_wait ();
    reply.join ();
    timeout.join ();
    assert (wait_until ([&] {
        return race_terminals.load (std::memory_order_acquire) == 1;
    }));
    assert (race_terminals.load (std::memory_order_acquire) == 1);
    assert (winning_terminal.load (std::memory_order_acquire)
              == static_cast<int> (foundation::operation_terminal_t::completed)
            || winning_terminal.load (std::memory_order_acquire)
                 == static_cast<int> (foundation::operation_terminal_t::timed_out));
    assert (race_registry.size () == 0);

    std::atomic_int cancel_close_terminals{0};
    foundation::operation_registry_t cancel_close_registry (1);
    assert (cancel_close_registry.register_operation (
      foundation::call_id_t{9, 2}, now,
      [&] (foundation::operation_terminal_t terminal,
           std::vector<std::uint8_t>) {
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
    assert (wait_until ([&] {
        return cancel_close_terminals.load (std::memory_order_acquire) == 1;
    }));
    assert (cancel_close_terminals.load (std::memory_order_acquire) == 1);

    foundation::operation_registry_t reentrant_registry (2);
    std::atomic_bool reentered{false};
    std::atomic_bool outer_callback_active{false};
    assert (reentrant_registry.register_operation (
      foundation::call_id_t{9, 3}, now,
      [&] (foundation::operation_terminal_t,
           std::vector<std::uint8_t>) {
          outer_callback_active.store (true, std::memory_order_release);
          assert (reentrant_registry.register_operation (
            foundation::call_id_t{9, 4}, now,
            [&] (foundation::operation_terminal_t terminal,
                 std::vector<std::uint8_t>) {
                assert (terminal
                        == foundation::operation_terminal_t::cancelled);
                assert (!outer_callback_active.load (
                  std::memory_order_acquire));
                reentered.store (true, std::memory_order_release);
            }));
          assert (reentrant_registry.cancel (foundation::call_id_t{9, 4}));
          outer_callback_active.store (false, std::memory_order_release);
      }));
    assert (reentrant_registry.complete (foundation::call_id_t{9, 3}, {}));
    assert (wait_until ([&] {
        return reentered.load (std::memory_order_acquire);
    }));
    assert (reentrant_registry.size () == 0);

    foundation::operation_registry_t turn_registry (1);
    const auto caller_thread = std::this_thread::get_id ();
    std::atomic_bool used_new_turn{false};
    assert (turn_registry.register_operation (
      foundation::call_id_t{10, 1}, now,
      [&] (foundation::operation_terminal_t terminal,
           std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::completed);
          used_new_turn.store (
            std::this_thread::get_id () != caller_thread,
            std::memory_order_release);
      }));
    assert (turn_registry.complete (foundation::call_id_t{10, 1}, {}));
    assert (wait_until ([&] {
        return used_new_turn.load (std::memory_order_acquire);
    }));

    foundation::operation_registry_t dispatcher_bounded_registry (
      foundation::default_operation_capacity + 1);
    std::atomic_size_t dispatched{0};
    std::atomic_bool blocked_callback_started{false};
    std::atomic_bool release_blocked_callback{false};
    for (std::size_t index = 1;
         index <= foundation::default_operation_capacity; ++index) {
        assert (wait_until ([&, index] {
            return dispatcher_bounded_registry.register_operation (
              foundation::call_id_t{11, index}, now,
              [&, index] (foundation::operation_terminal_t terminal,
                          std::vector<std::uint8_t>) {
                  assert (terminal
                          == (index == 1
                                ? foundation::operation_terminal_t::completed
                                : foundation::operation_terminal_t::shutdown));
                  if (index == 1) {
                      blocked_callback_started.store (
                        true, std::memory_order_release);
                      while (!release_blocked_callback.load (
                        std::memory_order_acquire)) {
                          std::this_thread::yield ();
                      }
                  }
                  dispatched.fetch_add (1, std::memory_order_release);
              });
        }));
    }
    assert (!dispatcher_bounded_registry.register_operation (
      foundation::call_id_t{
        11, foundation::default_operation_capacity + 1},
      now, [] (auto, auto) {}));
    assert (dispatcher_bounded_registry.complete (
      foundation::call_id_t{11, 1}, {}));
    assert (wait_until ([&] {
        return blocked_callback_started.load (std::memory_order_acquire);
    }));
    assert (dispatcher_bounded_registry.shutdown ()
            == foundation::default_operation_capacity - 1);

    foundation::operation_registry_t backlog_probe_registry (1);
    assert (!backlog_probe_registry.register_operation (
      foundation::call_id_t{12, 1}, now, [] (auto, auto) {}));
    release_blocked_callback.store (true, std::memory_order_release);
    assert (wait_until ([&] {
        return dispatched.load (std::memory_order_acquire)
               == foundation::default_operation_capacity;
    }));
    std::atomic_bool probe_drained{false};
    assert (wait_until ([&] {
        return backlog_probe_registry.register_operation (
          foundation::call_id_t{12, 1}, now,
          [&] (foundation::operation_terminal_t terminal,
               std::vector<std::uint8_t>) {
              assert (terminal
                      == foundation::operation_terminal_t::shutdown);
              probe_drained.store (true, std::memory_order_release);
          });
    }));
    assert (backlog_probe_registry.shutdown () == 1);
    assert (wait_until ([&] {
        return probe_drained.load (std::memory_order_acquire);
    }));

    foundation::operation_registry_t expiry_batch_registry (
      foundation::default_operation_capacity);
    std::atomic_size_t expired_batch_callbacks{0};
    for (std::size_t index = 1;
         index <= foundation::default_operation_capacity; ++index) {
        assert (expiry_batch_registry.register_operation (
          foundation::call_id_t{13, index}, now,
          [&] (foundation::operation_terminal_t terminal,
               std::vector<std::uint8_t>) {
              assert (terminal
                      == foundation::operation_terminal_t::timed_out);
              expired_batch_callbacks.fetch_add (1,
                                                   std::memory_order_release);
          }));
    }
    assert (expiry_batch_registry.expire (now)
            == foundation::default_operation_capacity);
    assert (expiry_batch_registry.size () == 0);
    assert (wait_until ([&] {
        return expired_batch_callbacks.load (std::memory_order_acquire)
               == foundation::default_operation_capacity;
    }));
    return 0;
}
