/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <boost/asio/execution/occupancy.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_executor.hpp>
#include <future>
#include <iostream>
#include <string_view>
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/operations/exactly_once_table.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <atomic>
#include <array>
#include <vector>
#include <barrier>
#include <latch>
#include <chrono>
#include <cstdlib>
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

void verify_independent_completion_turns (std::size_t producer_count)
{
    constexpr std::size_t count = 256;
    constexpr std::uint64_t operation_group = 21;
    foundation::operation_registry_t registry;
    std::array<std::atomic_size_t, count> delivered{};
    std::atomic_size_t completed{0};
    std::latch consumer_entered (1), consume (1);
    for (std::size_t index = 0; index < count; ++index) {
        assert (registry.register_operation (
          foundation::call_id_t{operation_group, index + 1},
          foundation::operation_registry_t::clock_t::time_point::max (),
          [&, index] (foundation::operation_terminal_t terminal, auto) {
              assert (terminal == foundation::operation_terminal_t::completed);
              if (index == 0) {
                  consumer_entered.count_down ();
                  consume.wait ();
              }
              delivered[index].fetch_add (1, std::memory_order_release);
              completed.fetch_add (1, std::memory_order_release);
          }));
    }
    assert (registry.complete (foundation::call_id_t{operation_group, 1}, {}));
    consumer_entered.wait ();
    std::barrier begin (static_cast<std::ptrdiff_t> (producer_count + 1));
    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back ([&, producer] {
            begin.arrive_and_wait ();
            for (std::size_t index = producer + 1; index < count; index += producer_count)
                assert (registry.complete (foundation::call_id_t{operation_group, index + 1}, {}));
        });
    }
    begin.arrive_and_wait ();
    for (auto &producer : producers)
        producer.join ();
    // Messaging hot path §4.4: unrelated callbacks progress independently of
    // the first callback's return. Submit and completion §11 requires new turns,
    // not registry-wide FIFO or per-producer callback order.
    const bool independent =
      wait_until ([&] { return completed.load (std::memory_order_acquire) == count - 1; });
    assert (delivered.front ().load (std::memory_order_acquire) == 0);
    for (std::size_t index = 0; index < count; ++index)
        assert (!registry.complete (foundation::call_id_t{operation_group, index + 1}, {}));
    // Release and drain before asserting so a serial-drain probe can finish.
    consume.count_down ();
    assert (registry.shutdown () == 0);
    assert (independent);
    // Submit and completion §10: atomic terminal take delivers exactly once.
    assert (completed.load (std::memory_order_acquire) == count);
    for (const auto &calls : delivered)
        assert (calls.load (std::memory_order_acquire) == 1);
}

void verify_shutdown_during_completion_publication ()
{
    foundation::operation_registry_t registry;
    const auto deadline = foundation::operation_registry_t::clock_t::time_point::max ();
    std::latch publication_entered (1), publish (1), finish_callback (1);
    std::atomic_bool closed{false}, callback_entered{false}, shutdown_returned{false};
    std::atomic_size_t delivered{0};
    assert (registry.register_operation (
      id (1), deadline, [&] (foundation::operation_terminal_t terminal, auto) {
          assert (terminal == foundation::operation_terminal_t::completed);
          callback_entered.store (true, std::memory_order_release);
          finish_callback.wait ();
          delivered.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (
      id (2), deadline, [&] (foundation::operation_terminal_t terminal, auto) {
          assert (terminal == foundation::operation_terminal_t::shutdown);
          closed.store (true, std::memory_order_release);
      }));
    std::thread producer ([&] {
        assert (registry.complete (id (1), {}, [&] {
            publication_entered.count_down ();
            publish.wait ();
        }));
    });
    publication_entered.wait ();
    std::thread shutdown ([&] {
        assert (registry.shutdown () == 1);
        assert (delivered.load (std::memory_order_acquire) == 1);
        shutdown_returned.store (true, std::memory_order_release);
    });
    // Submit and completion §10–11: the reservation survives atomic take and
    // publication after close; shutdown drains it through callback destruction.
    assert (wait_until ([&] { return closed.load (std::memory_order_acquire); }));
    assert (!shutdown_returned.load (std::memory_order_acquire));
    publish.count_down ();
    producer.join ();
    assert (wait_until ([&] { return callback_entered.load (std::memory_order_acquire); }));
    assert (!shutdown_returned.load (std::memory_order_acquire));
    finish_callback.count_down ();
    const bool drained =
      wait_until ([&] { return shutdown_returned.load (std::memory_order_acquire); });
    assert (drained);
    shutdown.join ();
    assert (delivered.load (std::memory_order_acquire) == 1);
    assert (registry.shutdown () == 0);
}

void verify_completion_payload_storage ()
{
    // Payload ownership and codec §2: move the native payload into its completion turn;
    // a heap-backed message (as in the raw route port contract) retains storage.
    constexpr std::size_t payload_size = 4096;
    constexpr std::uint8_t payload_value = 0x5a;
    for (const auto terminal : {foundation::operation_terminal_t::completed,
                                foundation::operation_terminal_t::transport_failed}) {
        foundation::operation_registry_t registry;
        auto payload =
          zlink::message_t::from (std::vector<std::uint8_t> (payload_size, payload_value));
        const void *storage = payload.data ();
        std::atomic_size_t delivered{0};
        assert (registry.register_operation (
          id (1), foundation::operation_registry_t::clock_t::time_point::max (),
          [&, storage] (foundation::operation_terminal_t observed, zlink::message_t received) {
              assert (observed == terminal);
              assert (received.data () == storage);
              assert (received.size () == payload_size);
              const auto *bytes = received.data ();
              for (std::size_t index = 0; index < payload_size; ++index)
                  assert (bytes[index] == std::byte{payload_value});
              delivered.fetch_add (1, std::memory_order_release);
          }));
        if (terminal == foundation::operation_terminal_t::completed)
            assert (registry.complete (id (1), std::move (payload)));
        else
            assert (registry.fail (id (1), terminal, std::move (payload)));
        payload.close ();
        assert (registry.shutdown () == 0);
        assert (delivered.load (std::memory_order_acquire) == 1);
    }
}

void verify_idle_completion_precedes_handoff ()
{
    foundation::operation_registry_t registry;
    const auto receiver = std::this_thread::get_id ();
    auto delivered = std::make_shared<std::promise<void>> ();
    auto finished = delivered->get_future ();
    std::atomic_size_t handoffs{0};
    assert (registry.register_operation (
      id (1), foundation::operation_registry_t::clock_t::time_point::max (),
      [&, delivered] (auto terminal, auto) {
          assert (terminal == foundation::operation_terminal_t::completed);
          assert (std::this_thread::get_id () != receiver);
          assert (handoffs.load () == 0);
          delivered->set_value ();
      }));
    // This is still inside the receive rotation. No boundary hook has run.
    assert (registry.complete (id (1), {}));
    assert (finished.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
    assert (!zlink::framework::detail::dispatch_completion_receive_handoff ([&] {
        ++handoffs;
        return false;
    }));
    assert (handoffs == 1 && registry.shutdown () == 0);
}

namespace
{
constexpr std::size_t shutdown_round_trips = 64;
std::atomic_size_t shutdown_delivered{0};
std::atomic_bool shutdown_release{false};
}

void verify_provider_completion_releases_dispatcher ()
{
    namespace framework = zlink::framework;
    framework::task_completion_source_t<int> source;
    auto pending = source.task ();
    std::promise<int> delivered;
    auto result = delivered.get_future ();
    framework::detail::observe_task_completion (
      pending, [&] (const auto &value) { delivered.set_value (value.value ()); });
    assert (source.complete (framework::result_t<int>::success (1867)));
    assert (result.get () == 1867);
    assert (
      wait_until ([] { return framework::detail::completion_dispatcher_worker_count () == 0; }));
    framework::detail::wait_for_completion_dispatcher_shutdown ();
}

void verify_worker_releases_last_dispatcher_reference ()
{
    namespace detail = zlink::framework::detail;
    auto dispatcher = detail::acquire_completion_dispatcher ();
    const std::weak_ptr<detail::completion_dispatcher_t> observed (dispatcher);
    std::latch entered (1), release (1);
    detail::dispatch_completion (
      [&, dispatcher] {
          assert (dispatcher);
          entered.count_down ();
          release.wait ();
      },
      false, nullptr, dispatcher);
    entered.wait ();
    dispatcher.reset ();
    assert (!observed.expired ());
    release.count_down ();
    detail::wait_for_completion_dispatcher_shutdown ();
    assert (observed.expired ());
    assert (detail::completion_dispatcher_worker_count () == 0);
}

void verify_registry_owns_dispatcher_until_delivery ()
{
    foundation::operation_registry_t registry;
    std::promise<void> delivered;
    auto result = delivered.get_future ();
    assert (registry.register_operation (
      id (1), foundation::operation_registry_t::clock_t::time_point::max (),
      [&] (auto terminal, auto) {
          assert (terminal == foundation::operation_terminal_t::completed);
          delivered.set_value ();
      }));
    assert (zlink::framework::detail::completion_dispatcher_worker_count () != 0);
    assert (registry.complete (id (1), {}));
    result.get ();
    assert (registry.shutdown () == 0);
    zlink::framework::detail::wait_for_completion_dispatcher_shutdown ();
    assert (zlink::framework::detail::completion_dispatcher_worker_count () == 0);
}

void verify_dispatcher_shutdown_drains ()
{
    namespace detail = zlink::framework::detail;
    auto dispatcher = detail::acquire_completion_dispatcher ();
    const auto workers = detail::completion_dispatcher_worker_count ();
    auto occupied = std::make_shared<std::latch> (workers);
    for (std::size_t index = 0; index < workers; ++index)
        detail::dispatch_completion ([occupied, dispatcher] {
            assert (dispatcher);
            occupied->count_down ();
            shutdown_release.wait (false);
        });
    occupied->wait ();
    for (std::size_t index = 0; index < shutdown_round_trips; ++index)
        detail::dispatch_completion ([dispatcher] {
            ++shutdown_delivered;
            detail::dispatch_completion ([] { ++shutdown_delivered; }, false, &shutdown_delivered,
                                         dispatcher);
        });
    assert (shutdown_delivered == 0);
    dispatcher.reset ();
    shutdown_release = true;
    shutdown_release.notify_all ();
    detail::wait_for_completion_dispatcher_shutdown ();
    assert (shutdown_delivered == shutdown_round_trips * 2);
    assert (detail::completion_dispatcher_worker_count () == 0);
}

void verify_handoff_excludes_application_system_handlers ()
{
    namespace detail = zlink::framework::detail;
    assert (detail::completion_dispatcher_worker_count () == 0);
    assert (detail::dispatch_completion_receive_handoff ([] { return true; }));
    assert (detail::completion_dispatcher_worker_count () == 0);
    const auto dispatcher = detail::acquire_completion_dispatcher ();
    const auto framework_workers = detail::completion_dispatcher_worker_count ();
    std::latch framework_occupied (framework_workers), framework_release (1),
      framework_exited (framework_workers);
    for (std::size_t index = 0; index < framework_workers; ++index)
        detail::dispatch_completion ([&] {
            framework_occupied.count_down ();
            framework_release.wait ();
            framework_exited.count_down ();
        });
    framework_occupied.wait ();
    const auto receiver = std::this_thread::get_id ();
    bool framework_on_receiver = false;
    detail::dispatch_completion (
      [&] { framework_on_receiver = std::this_thread::get_id () == receiver; });
    const auto workers =
      boost::asio::query (boost::asio::system_executor{}, boost::asio::execution::occupancy);
    std::latch occupied (workers), release (1), exited (workers);
    for (std::size_t index = 0; index < workers; ++index)
        boost::asio::post (boost::asio::system_executor{}, [&] {
            occupied.count_down ();
            release.wait ();
            exited.count_down ();
        });
    occupied.wait ();
    std::atomic_bool external_on_receiver{false};
    std::promise<void> external;
    auto done = external.get_future ();
    boost::asio::post (boost::asio::system_executor{}, [&] {
        external_on_receiver = std::this_thread::get_id () == receiver;
        external.set_value ();
    });
    // Every system worker is occupied, so only the handoff resource could
    // consume the external handler before the release below.
    assert (detail::dispatch_completion_receive_handoff ([] { return true; }));
    assert (done.wait_for (std::chrono::seconds (0)) == std::future_status::timeout);
    assert (framework_on_receiver);
    framework_release.count_down ();
    framework_exited.wait ();
    release.count_down ();
    exited.wait ();
    assert (done.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
    assert (!external_on_receiver);
}

void verify_completion_resource_transitions ()
{
    namespace detail = zlink::framework::detail;
    constexpr std::size_t round_trips = 64;
    const auto receiver = std::this_thread::get_id ();
    std::atomic_size_t before{0}, after{0}, delivered{0};
    foundation::operation_registry_t registry;
    for (std::size_t index = 0; index < round_trips; ++index) {
        auto returned = std::make_shared<std::promise<void>> ();
        auto reply = returned->get_future ();
        assert (registry.register_operation (
          {41, index + 1}, foundation::operation_registry_t::clock_t::time_point::max (),
          [&, returned] (auto terminal, auto) {
              assert (terminal == foundation::operation_terminal_t::completed);
              before += std::this_thread::get_id () != receiver;
              ++delivered;
              returned->set_value ();
          }));
        assert (registry.complete ({41, index + 1}, {}));
        assert (reply.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
    }
    assert (before == round_trips);

    // Occupy the dispatcher's existing workers so the joining resource wins
    // dequeue deterministically. This neither stops nor replaces its queue.
    const auto workers = detail::completion_dispatcher_worker_count ();
    auto occupied = std::make_shared<std::latch> (workers);
    auto release = std::make_shared<std::latch> (1);
    auto exited = std::make_shared<std::latch> (workers);
    for (std::size_t index = 0; index < workers; ++index)
        detail::dispatch_completion ([occupied, release, exited] {
            occupied->count_down ();
            release->wait ();
            exited->count_down ();
        });
    occupied->wait ();
    std::size_t handoffs = 0;
    for (std::size_t index = 0; index < round_trips; ++index) {
        bool rotation_returned = false;
        assert (registry.register_operation (
          {42, index + 1}, foundation::operation_registry_t::clock_t::time_point::max (),
          [&] (auto terminal, auto) {
              assert (rotation_returned);
              assert (detail::current_completion_owner () != nullptr);
              assert (terminal == foundation::operation_terminal_t::completed);
              after += std::this_thread::get_id () != receiver;
              ++delivered;
              if (index == 0)
                  throw std::runtime_error ("joining worker releases a throwing callback slot");
          }));
        assert (registry.complete ({42, index + 1}, {}));
        assert (delivered == round_trips + index);
        rotation_returned = true;
        assert (detail::dispatch_completion_receive_handoff ([&] {
            ++handoffs;
            return true;
        }));
        assert (delivered == round_trips + index + 1);
        assert (detail::current_completion_owner () == nullptr);
    }
    assert (registry.shutdown () == 0 && after == 0 && handoffs == round_trips);
    // A successful handoff is valid even if idle workers emptied the queue.
    assert (detail::dispatch_completion_receive_handoff ([] { return true; }));
    foundation::operation_registry_t queued;
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < 2; ++index) {
        assert (queued.register_operation (
          {43, index + 1}, foundation::operation_registry_t::clock_t::time_point::max (),
          [&, index] (auto, auto) { order.push_back (index); }));
        assert (queued.complete ({43, index + 1}, {}));
    }
    assert (detail::dispatch_completion_receive_handoff ([] { return true; }));
    assert ((order == std::vector<std::size_t>{0}));
    assert (detail::dispatch_completion_receive_handoff ([] { return true; }));
    assert ((order == std::vector<std::size_t>{0, 1}));
    assert (queued.shutdown () == 0);

    foundation::operation_registry_t fallback;
    std::atomic_size_t fallback_calls{0};
    assert (fallback.register_operation (
      {44, 1}, foundation::operation_registry_t::clock_t::time_point::max (), [&] (auto, auto) {
          assert (std::this_thread::get_id () != receiver);
          ++fallback_calls;
      }));
    assert (fallback.complete ({44, 1}, {}));
    assert (!detail::dispatch_completion_receive_handoff ([] { return false; }));
    assert (fallback_calls == 0);
    release->count_down ();
    exited->wait ();
    assert (fallback.shutdown () == 0 && fallback_calls == 1);
    std::cout << "completion_round_trips=" << round_trips << " transitions_before=" << before
              << " transitions_after=" << after << " handoffs=" << handoffs << '\n';
}

int main (int argc, char **argv)
{
    if (argc == 2 && std::string_view (argv[1]) == "--dispatcher-lifetime") {
        verify_provider_completion_releases_dispatcher ();
        verify_worker_releases_last_dispatcher_reference ();
        verify_registry_owns_dispatcher_until_delivery ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--dispatcher-shutdown") {
        verify_dispatcher_shutdown_drains ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--isolation-only") {
        verify_handoff_excludes_application_system_handlers ();
        return 0;
    }
    verify_handoff_excludes_application_system_handlers ();
    const auto dispatcher = zlink::framework::detail::acquire_completion_dispatcher ();
    if (argc == 2 && std::string_view (argv[1]) == "--handoff-counts") {
        verify_completion_resource_transitions ();
        return 0;
    }
    verify_idle_completion_precedes_handoff ();
    verify_completion_resource_transitions ();
    verify_independent_completion_turns (1);
    verify_independent_completion_turns (4);
    verify_shutdown_during_completion_publication ();
    verify_completion_payload_storage ();
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
      [&] (foundation::operation_terminal_t terminal, zlink::message_t native_payload) {
          auto payload = native_payload.to_bytes ();
          assert (terminal == foundation::operation_terminal_t::completed);
          assert (payload == std::vector<std::uint8_t> ({7}));
          first_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (
      id (2), now, [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
          assert (terminal == foundation::operation_terminal_t::timed_out);
          second_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (id (3), now, [] (auto, auto) {}));
    assert (registry.unregister (id (3)));
    assert (registry.complete (id (1), zlink::message_t::from (std::vector<std::uint8_t>{7})));
    assert (!registry.complete (id (1), zlink::message_t::from (std::vector<std::uint8_t>{8})));
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
      [&] (foundation::operation_terminal_t terminal, zlink::message_t native_payload) {
          auto payload = native_payload.to_bytes ();
          assert (terminal == foundation::operation_terminal_t::transport_failed);
          failure_payload = std::move (payload);
          failed_terminals.fetch_add (1, std::memory_order_release);
      }));
    assert (failed_registry.fail (id (4), foundation::operation_terminal_t::transport_failed,
                                  zlink::message_t::from (std::vector<std::uint8_t>{1, 2, 3})));
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
          id (5), now, [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
              throw std::runtime_error ("consumer failure");
          }));
        assert (scoped_registry.register_operation (
          id (6), now, [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
      [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
          winning_terminal.store (static_cast<int> (terminal), std::memory_order_release);
          race_terminals.fetch_add (1, std::memory_order_acq_rel);
      }));
    std::barrier race_start (3);
    std::thread reply ([&] {
        race_start.arrive_and_wait ();
        (void) race_registry.complete (foundation::call_id_t{9, 1},
                                       zlink::message_t::from (std::vector<std::uint8_t>{1}));
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
      [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
    static thread_local bool outer_callback_active = false;
    assert (reentrant_registry.register_operation (
      foundation::call_id_t{9, 3}, now, [&] (foundation::operation_terminal_t, zlink::message_t) {
          outer_callback_active = true;
          assert (reentrant_registry.register_operation (
            foundation::call_id_t{9, 4}, now,
            [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
                assert (terminal == foundation::operation_terminal_t::cancelled);
                assert (!outer_callback_active);
                reentered.store (true, std::memory_order_release);
            }));
          assert (reentrant_registry.cancel (foundation::call_id_t{9, 4}));
          outer_callback_active = false;
      }));
    assert (reentrant_registry.complete (foundation::call_id_t{9, 3}, {}));
    assert (wait_until ([&] { return reentered.load (std::memory_order_acquire); }));
    assert (reentrant_registry.size () == 0);

    foundation::operation_registry_t turn_registry;
    const auto caller_thread = std::this_thread::get_id ();
    std::atomic_bool used_new_turn{false};
    assert (turn_registry.register_operation (
      foundation::call_id_t{10, 1}, now,
      [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
          [&, index] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
      [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
          [&] (foundation::operation_terminal_t terminal, zlink::message_t) {
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
