/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/dispatch_limits.hpp"
#include "allocation_counter.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <future>
#include <iostream>
#include <string_view>
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/operations/exactly_once_table.hpp"
#include <zlink/framework/detail/deadline_scheduler.hpp>

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
#include <filesystem>
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
std::size_t clock_reads = 0;

// Each read advances the injected clock by the configured step.
foundation::operation_registry_t::clock_t::time_point read_injected_clock ()
{
    ++clock_reads;
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
          [&, storage] (foundation::operation_terminal_t observed,
                        ::zlink::framework::result_t<zlink::message_t> completion_payload) {
              auto received = std::move (completion_payload.value ());
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

namespace
{
constexpr std::size_t shutdown_round_trips = 64;
std::atomic_size_t shutdown_delivered{0};
std::atomic_bool shutdown_release{false};
}

void verify_provider_completion_releases_dispatcher ()
{
    namespace framework = zlink::framework;
    framework::completion_owner_t owner;
    framework::task_completion_source_t<int> source;
    auto pending = source.task ();
    std::promise<int> delivered;
    auto result = delivered.get_future ();
    framework::detail::observe_task_completion (
      pending, [&] (const auto &value) { delivered.set_value (value.value ()); });
    assert (source.complete (framework::result_t<int>::success (1867)));
    assert (result.get () == 1867);
    owner.close ();
    assert (framework::detail::completion_dispatcher_worker_count () == 0);
}

void verify_worker_lifecycle_is_rejected ()
{
    using namespace zlink::framework;
    completion_owner_t owner;
    const auto observed = detail::observe_completion_dispatcher ();
    std::promise<bool> rejected;
    auto result = rejected.get_future ();
    detail::dispatch_completion ([&] {
        try {
            owner.close ();
            rejected.set_value (false);
        }
        catch (const framework_exception_t &error) {
            rejected.set_value (error.kind () == framework_error_kind_t::invalid_operation);
        }
    });
    assert (result.get ());
    assert (!observed.expired ());
    owner.close ();
    owner.close ();
    assert (observed.expired ());
    assert (detail::completion_dispatcher_worker_count () == 0);
}

void verify_registry_borrows_dispatcher_until_delivery ()
{
    zlink::framework::completion_owner_t owner;
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
    owner.close ();
    assert (zlink::framework::detail::completion_dispatcher_worker_count () == 0);
}

void verify_dispatcher_shutdown_drains ()
{
    namespace detail = zlink::framework::detail;
    zlink::framework::completion_owner_t owner;
    auto dispatcher = detail::borrow_completion_dispatcher ();
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
        detail::dispatch_completion ([] {
            ++shutdown_delivered;
            detail::dispatch_completion ([] { ++shutdown_delivered; }, false, &shutdown_delivered);
        });
    assert (shutdown_delivered == 0);
    const auto observed = detail::observe_completion_dispatcher ();
    std::jthread unblock ([&] {
        while (!observed.expired ())
            std::this_thread::yield ();
        shutdown_release = true;
        shutdown_release.notify_all ();
    });
    owner.close ();
    assert (shutdown_delivered == shutdown_round_trips * 2);
    assert (detail::completion_dispatcher_worker_count () == 0);
}

void verify_shutdown_waits_for_callback_capture_destruction ()
{
    std::latch capture_destruction_entered (1);
    std::latch release_capture_destruction (1);
    std::atomic_bool shutdown_returned{false};
    std::atomic_bool shutdown_returned_before_capture_destruction{false};
    struct capture_t
    {
        std::latch &entered;
        std::latch &release;
        std::atomic_bool &shutdown_returned;
        std::atomic_bool &returned_before_destruction;

        capture_t (std::latch &entered_,
                   std::latch &release_,
                   std::atomic_bool &shutdown_returned_,
                   std::atomic_bool &returned_before_destruction_) :
            entered (entered_),
            release (release_),
            shutdown_returned (shutdown_returned_),
            returned_before_destruction (returned_before_destruction_)
        {
        }

        ~capture_t ()
        {
            entered.count_down ();
            release.wait ();
            returned_before_destruction.store (shutdown_returned.load (std::memory_order_acquire),
                                               std::memory_order_release);
        }
    };

    foundation::operation_registry_t registry;
    auto capture =
      std::make_shared<capture_t> (capture_destruction_entered, release_capture_destruction,
                                   shutdown_returned, shutdown_returned_before_capture_destruction);
    foundation::operation_registry_t::callback_t callback = [capture] (auto terminal, auto) {
        assert (terminal == foundation::operation_terminal_t::shutdown);
    };
    capture.reset ();
    assert (registry.register_operation (
      {45, 1}, foundation::operation_registry_t::clock_t::time_point::max (),
      std::move (callback)));

    auto shutdown_finished = std::make_shared<std::promise<void>> ();
    auto shutdown_done = shutdown_finished->get_future ();
    std::thread shutdown ([&] {
        assert (registry.shutdown () == 1);
        shutdown_returned.store (true, std::memory_order_release);
        shutdown_finished->set_value ();
    });
    capture_destruction_entered.wait ();
    const bool returned_while_capture_destruction_was_blocked =
      shutdown_done.wait_for (std::chrono::milliseconds (100)) == std::future_status::ready;
    release_capture_destruction.count_down ();
    shutdown.join ();

    assert (!returned_while_capture_destruction_was_blocked);
    assert (!shutdown_returned_before_capture_destruction.load (std::memory_order_acquire));
}

void verify_completion_callbacks_use_dispatcher ()
{
    constexpr std::size_t round_trips = 64;
    const auto receiver = std::this_thread::get_id ();
    std::atomic_size_t delivered{0};
    foundation::operation_registry_t registry;
    for (std::size_t index = 0; index < round_trips; ++index) {
        auto returned = std::make_shared<std::promise<void>> ();
        auto reply = returned->get_future ();
        assert (registry.register_operation (
          {41, index + 1}, foundation::operation_registry_t::clock_t::time_point::max (),
          [&, returned] (auto terminal, auto) {
              assert (terminal == foundation::operation_terminal_t::completed);
              assert (std::this_thread::get_id () != receiver);
              ++delivered;
              returned->set_value ();
          }));
        assert (registry.complete ({41, index + 1}, {}));
        assert (reply.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
    }
    assert (delivered == round_trips);
    assert (registry.shutdown () == 0);
}

void verify_request_deadline_index_contract ()
{
    using namespace std::chrono_literals;
    using registry_t = foundation::operation_registry_t;
    using terminal_t = foundation::operation_terminal_t;
    const auto timers =
      zlink::framework::detail::deadline_scheduler_t::instance ().scheduled_count ();
    injected_now = {};
    injected_step = {};
    std::size_t wakeups = 0;
    registry_t registry (&read_injected_clock, [&] { ++wakeups; });
    std::size_t visits = 0, reservations = 0;
    std::optional<registry_t::clock_t::time_point> reserved;
    for (std::size_t i = 0; i < 1000; ++i) {
        const foundation::call_id_t key{1001, i + 1};
        assert (registry.register_operation (key, injected_now + 30s, [] (auto, auto) {}));
        const auto next = registry.next_deadline ();
        if (!reserved || next != reserved) {
            ++reservations;
            reserved = next;
        }
        const auto reads = clock_reads;
        assert (registry.complete (key, {}));
        assert (clock_reads == reads);
        assert (registry.size () == 0);
        assert (registry.next_deadline () == reserved);
        injected_now += 1ms;
        if (injected_now >= *reserved) {
            assert (registry.expire (injected_now).result ().value () == 0);
            ++visits;
            assert (registry.index_size () == 0);
            assert (!registry.next_deadline ());
            reserved.reset ();
        }
    }
    assert (reservations <= 4 && wakeups == reservations);
    assert (visits <= 3);
    // #2889: retain the already booked visit, then stop completely at idle.
    assert (registry.next_deadline ());
    injected_now = *registry.next_deadline ();
    assert (registry.expire (injected_now).result ().value () == 0);
    assert (registry.index_size () == 0);
    assert (!registry.next_deadline ());
    for (int i = 0; i < 1000; ++i) {
        injected_now += 1ms;
        assert (registry.expire (injected_now).result ().value () == 0);
        assert (!registry.next_deadline ());
    }
    assert (wakeups == reservations);
    // Long deadlines in every slot must not schedule one turn per occupied slot.
    const auto started = injected_now;
    for (std::size_t i = 0; i < 1000; ++i)
        assert (
          registry.register_operation ({1002, i + 1}, started + 30s + i * 1ms, [] (auto, auto) {}));
    assert (registry.next_deadline () == started + 256ms);
    for (std::size_t i = 0; i < 1000; ++i)
        assert (registry.cancel ({1002, i + 1}));
    assert (registry.size () == 0 && registry.index_size () == 1000);
    injected_now = started + 256ms;
    registry.expire (injected_now).result ().value ();
    assert (registry.index_size () == 0 && !registry.next_deadline ());
    std::stop_source cancel_batch;
    for (std::size_t i = 0; i < 1000; ++i)
        assert (registry.register_operation (
          {1006, i + 1}, injected_now + 30s, [] (auto, auto) {}, {}, {}, {},
          cancel_batch.get_token ()));
    cancel_batch.request_stop ();
    assert (registry.size () == 0 && registry.index_size () == 1000);
    injected_now += 256ms;
    registry.expire (injected_now).result ().value ();
    assert (registry.index_size () == 0 && !registry.next_deadline ());
    // A previously cancelled caller still uses the same C1 terminal take.
    assert (registry.register_operation (
      {1006, 1001}, injected_now + 30s, [] (auto, auto) {}, {}, {}, {}, cancel_batch.get_token ()));
    assert (registry.size () == 0);
    injected_now += 256ms;
    registry.expire (injected_now).result ().value ();
    assert (registry.index_size () == 0);
    // Reusing an id must not let an old index reference expire the new entry.
    assert (registry.register_operation ({1003, 1}, injected_now + 1ms, [] (auto, auto) {}));
    assert (registry.unregister ({1003, 1}));
    assert (registry.register_operation ({1003, 1}, injected_now + 1s, [] (auto, auto) {}));
    injected_now += 1ms;
    assert (registry.expire (injected_now).result ().value () == 0);
    assert (registry.contains ({1003, 1}) && registry.index_size () == 1);
    assert (registry.unregister ({1003, 1}));
    registry.shutdown ();
    assert (zlink::framework::detail::deadline_scheduler_t::instance ().scheduled_count ()
            == timers);

    std::size_t earlier_wakes = 0;
    registry_t earlier (&read_injected_clock, [&] { ++earlier_wakes; });
    assert (earlier.register_operation ({1007, 1}, injected_now + 1s, [] (auto, auto) {}));
    assert (earlier.register_operation ({1007, 2}, injected_now + 5ms, [] (auto, auto) {}));
    assert (earlier.register_operation ({1007, 3}, injected_now + 50ms, [] (auto, auto) {}));
    assert (earlier_wakes == 2 && earlier.next_deadline () == injected_now + 5ms);
    earlier.shutdown ();
    // Terminal removal must work while the operation owner's lane is occupied.
    runtime::offload_executor_t executor;
    runtime::state_lane_t lane (executor);
    registry_t direct (lane);
    std::stop_source direct_cancel;
    assert (direct.register_operation (
      {1004, 1}, registry_t::clock_t::time_point::max (), [] (auto, auto) {}, {}, {}, {},
      direct_cancel.get_token ()));
    lane.run_sync ([] {});
    const auto reserved_direct = direct.next_deadline ();
    std::latch entered (1), release (1);
    std::thread blocked ([&] {
        lane.run_sync ([&] {
            entered.count_down ();
            release.wait ();
        });
    });
    entered.wait ();
    direct_cancel.request_stop ();
    const auto taken = !direct.contains ({1004, 1});
    const auto pending = direct.size ();
    const auto unchanged = direct.next_deadline () == reserved_direct;
    const auto idle_expiry = direct.expire (registry_t::clock_t::time_point{});
    release.count_down ();
    blocked.join ();
    assert (taken && pending == 0 && unchanged && idle_expiry.result ().value () == 0);
    assert (direct.index_size () == 1);
    direct.shutdown ();
    lane.run_sync ([] {});
    std::shared_ptr<registry_t> late;
    {
        runtime::offload_executor_t late_executor;
        runtime::state_lane_t late_lane (late_executor);
        late = std::make_shared<registry_t> (late_lane);
        assert (late->register_operation ({1005, 1}, registry_t::clock_t::time_point::max (),
                                          [] (auto, auto) {}));
        assert (late->shutdown () == 1);
    }
    assert (!late->complete ({1005, 1}, {}));
    assert (late->shutdown () == 0);
    late.reset ();
    std::cout << "request_deadline_index_contract serial_requests=1000 batch_requests=2000 "
                 "timers=0 serial_reservations="
              << reservations << " maintenance_visits=" << visits + 1
              << " pending=0 index=0 owner_blocked_take=1\n";
}

void verify_scheduled_deadline_visits (bool drive_visits = true)
{
    using namespace std::chrono_literals;
    using registry_t = foundation::operation_registry_t;
    zlink::framework::completion_owner_t completion_owner;
    runtime::offload_executor_t executor (1, "deadline-visit-test");
    auto lane = std::make_unique<runtime::state_lane_t> (executor);
    auto registry =
      drive_visits ? registry_t::create_scheduled (*lane) : std::make_shared<registry_t> (*lane);
    std::promise<foundation::operation_terminal_t> early_promise;
    auto early = early_promise.get_future ();
    std::promise<foundation::operation_terminal_t> later_promise;
    auto later = later_promise.get_future ();
    const auto now = registry_t::clock_t::now ();
    assert (registry->register_operation (
      id (201), now + 5s, [&] (auto terminal, auto) { later_promise.set_value (terminal); }));
    assert (registry->register_operation (
      id (202), now + 20ms, [&] (auto terminal, auto) { early_promise.set_value (terminal); }));
    // No receive pump, explicit expire call or owner turn drives the deadline.
    assert (early.wait_for (1s) == std::future_status::ready);
    assert (early.get () == foundation::operation_terminal_t::timed_out);
    assert (registry_t::clock_t::now () >= now + 20ms);
    assert (registry->contains (id (201)));
    assert (registry->complete (id (201), {}));
    assert (later.get () == foundation::operation_terminal_t::completed);
    assert (wait_until ([&] { return !registry->next_deadline (); }));
    assert (registry->size () == 0 && registry->index_size () == 0);

    std::promise<foundation::operation_terminal_t> close_promise;
    auto closed = close_promise.get_future ();
    assert (registry->register_operation (
      id (203), registry_t::clock_t::now () + 5s,
      [&] (auto terminal, auto) { close_promise.set_value (terminal); }));
    assert (registry->shutdown () == 1);
    assert (closed.get () == foundation::operation_terminal_t::shutdown);
    // A cancelled worker reservation must not access a destroyed borrowed lane.
    lane.reset ();
    registry.reset ();
}

void verify_no_global_registry_thread ()
{
#ifdef __linux__
    const auto threads = [] {
        return std::distance (std::filesystem::directory_iterator ("/proc/self/task"),
                              std::filesystem::directory_iterator{});
    };
    const auto before = threads ();
    foundation::operation_registry_t registry;
    assert (threads () == before);
#endif
}

void verify_registration_does_not_wait_for_owner ()
{
    runtime::offload_executor_t executor;
    runtime::state_lane_t lane (executor);
    auto registry = std::make_unique<foundation::operation_registry_t> (lane);
    std::latch entered (1), release (1);
    std::atomic_bool returned{false};
    std::size_t stopped = 0;
    assert (lane.try_post ([&] {
        entered.count_down ();
        release.wait ();
    }));
    entered.wait ();
    std::thread caller ([&] {
        assert (registry->register_operation (
          {2001, 1}, foundation::operation_registry_t::clock_t::now (), [] (auto, auto) {}));
        assert (registry->contains ({2001, 1}));
        (void) registry->expire (foundation::operation_registry_t::clock_t::now ());
        (void) registry->index_size ();
        stopped = registry->shutdown ();
        registry.reset (); // Posted index and cleanup turns retain their own state.
        returned.store (true, std::memory_order_release);
    });
    const bool independent = wait_until ([&] { return returned.load (std::memory_order_acquire); });
    release.count_down ();
    caller.join ();
    lane.run_sync ([] {});
    assert (independent);
    assert (stopped == 1);
    std::cout << "registry_no_hop owner_blocked_register_expire_index_shutdown=1\n";
}

void verify_publication_allocation_failure ()
{
    runtime::offload_executor_t executor;
    runtime::state_lane_t lane (executor);
    std::latch entered (1), release (1);
    assert (lane.try_post ([&] {
        entered.count_down ();
        release.wait ();
    }));
    entered.wait ();
    std::size_t rejected = 0, accepted = 0;
    for (std::size_t failure = 0; failure < 8; ++failure) {
        std::stop_source cancellation;
        {
            foundation::operation_registry_t registry (lane);
            zlink::framework::tests::allocations_before_failure = failure;
            try {
                const bool registered = registry.register_operation (
                  {2002, failure + 1},
                  foundation::operation_registry_t::clock_t::time_point::max (), [] (auto, auto) {},
                  {}, {}, {}, cancellation.get_token ());
                zlink::framework::tests::allocations_before_failure.reset ();
                assert (registered && registry.size () == 1);
                assert (registry.unregister ({2002, failure + 1}));
                ++accepted;
            }
            catch (const std::bad_alloc &) {
                zlink::framework::tests::allocations_before_failure.reset ();
                assert (registry.size () == 0 && !registry.contains ({2002, failure + 1}));
                ++rejected;
            }
            assert (registry.shutdown () == 0);
        }
        // A failed publication must have removed its stop subscription before
        // the registry is destroyed, even while its owner lane is unavailable.
        cancellation.request_stop ();
    }
    release.count_down ();
    lane.run_sync ([] {});
    assert (rejected >= 4 && accepted > 0);
}

void verify_shutdown_cleanup_owner_lifetime ()
{
    struct held_executor_t final : runtime::offload_executor_t
    {
        std::latch entered{1}, release{1};
        std::size_t submissions = 0;
        bool try_submit_internal (std::function<void ()> work) override
        {
            ++submissions;
            return offload_executor_t::try_submit_internal ([this, work = std::move (work)] {
                entered.count_down ();
                release.wait ();
                work ();
            });
        }
    };
    struct release_scope_t
    {
        std::latch &release;
        ~release_scope_t () { release.count_down (); }
    };
    using registry_t = foundation::operation_registry_t;
    // An empty registry has no cleanup turn and needs no completion owner.
    {
        held_executor_t executor;
        runtime::state_lane_t lane (executor);
        registry_t registry (lane);
        const release_scope_t release{executor.release};
        assert (registry.shutdown () == 0);
        assert (executor.submissions == 0);
        auto next = lane.run_task ([] { return true; });
        assert (next.await_ready () && next.result ().value ());
    }
    zlink::framework::completion_owner_t completion_owner;
    // A nonempty index still cleans up in FIFO order before the next turn.
    {
        held_executor_t executor;
        runtime::state_lane_t lane (executor);
        registry_t registry (lane);
        lane.run_sync ([&] {
            assert (registry.register_operation (
              {2004, 1}, registry_t::clock_t::time_point::max (), [] (auto terminal, auto) {
                  assert (terminal == foundation::operation_terminal_t::shutdown);
              }));
        });
        assert (registry.index_size () == 1);
        assert (registry.shutdown () == 1);
        executor.entered.wait ();
        assert (executor.submissions == 1 && registry.index_size () == 1);
        auto next = [&] {
            const release_scope_t release{executor.release};
            auto task = lane.run_task ([&] { return registry.index_size (); });
            assert (!task.await_ready ());
            return task;
        }();
        assert (next.result ().value () == 0);
    }
    // A settled request has no pending completion but still owns indexed storage.
    {
        held_executor_t executor;
        runtime::state_lane_t lane (executor);
        registry_t registry (lane);
        lane.run_sync ([&] {
            assert (registry.register_operation ({2004, 3}, registry_t::clock_t::time_point::max (),
                                                 [] (auto, auto) { assert (false); }));
        });
        assert (registry.unregister ({2004, 3}));
        assert (registry.size () == 0 && registry.index_size () == 1);
        assert (registry.shutdown () == 0);
        executor.entered.wait ();
        auto next = [&] {
            const release_scope_t release{executor.release};
            return lane.run_task ([&] { return registry.index_size (); });
        }();
        assert (next.result ().value () == 0);
    }
    // Zero indexed entries can still have a publication queued ahead of cleanup.
    {
        held_executor_t executor;
        runtime::state_lane_t lane (executor);
        registry_t registry (lane);
        assert (registry.register_operation (
          {2004, 2}, registry_t::clock_t::time_point::max (), [] (auto terminal, auto) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
          }));
        executor.entered.wait ();
        assert (registry.index_size () == 0);
        assert (registry.shutdown () == 1);
        auto next = [&] {
            const release_scope_t release{executor.release};
            return lane.run_task ([&] { return registry.index_size (); });
        }();
        assert (next.result ().value () == 0);
    }
    std::cout
      << "shutdown_cleanup empty_posts=0 indexed_fifo=1 retired_fifo=1 publication_fifo=1\n";
}

void verify_expiry_reports_owner_result ()
{
    using namespace std::chrono_literals;
    using registry_t = foundation::operation_registry_t;
    zlink::framework::completion_owner_t completion_owner;
    runtime::offload_executor_t executor (1, "expiry-result-test");
    runtime::state_lane_t lane (executor);
    registry_t registry (lane);
    std::promise<foundation::operation_terminal_t> completion;
    auto terminal = completion.get_future ();
    const auto now = registry_t::clock_t::now ();
    assert (registry.register_operation (
      {2003, 1}, now + 30s, [&] (auto result, auto) { completion.set_value (result); }));
    // Exclude publication lag: an off-owner visit must report the owner's count.
    lane.run_sync ([] {});
    auto expiry = registry.expire (now + 31s);
    assert (expiry.result ().value () == 1);
    assert (terminal.get () == foundation::operation_terminal_t::timed_out);
    assert (registry.size () == 0);

    // A queued visit is a pending task, never a fabricated zero or a blocking wait.
    std::latch entered (1), release (1);
    assert (lane.try_post ([&] {
        entered.count_down ();
        release.wait ();
    }));
    entered.wait ();
    assert (registry.register_operation ({2003, 2}, now + 30s, [] (auto result, auto) {
        assert (result == foundation::operation_terminal_t::timed_out);
    }));
    auto queued = registry.expire (now + 31s);
    assert (!queued.await_ready ());
    release.count_down ();
    assert (queued.result ().value () == 1);
    lane.run_sync ([&] {
        auto ready = registry.expire (now + 31s);
        assert (ready.await_ready ());
        zlink::framework::detail::observe_task_terminal (
          ready, [] (const auto &result) { assert (result && result.value () == 0); });
    });
    registry.shutdown ();
    auto closed = registry.expire (now + 31s);
    assert (!closed.result ());
    assert (closed.result ().error_kind ()
            == zlink::framework::framework_error_kind_t::shutting_down);
    // A mailbox turn abandoned when its executor rejects drain ownership must
    // fail its task too; accepted publication must not strand the observer.
    struct rejecting_executor_t final : runtime::offload_executor_t
    {
        bool try_submit_internal (std::function<void ()>) override { return false; }
    } rejecting_executor;
    runtime::state_lane_t rejected_lane (rejecting_executor);
    registry_t rejected_registry (rejected_lane);
    rejected_lane.run_sync ([&] {
        assert (rejected_registry.register_operation ({2003, 3}, now + 30s, [] (auto, auto) {}));
    });
    auto abandoned = rejected_registry.expire (now + 31s);
    assert (!abandoned.result ());
    assert (rejected_registry.contains ({2003, 3}));
    assert (rejected_registry.shutdown () == 1);
    std::cout << "expiry_owner_result off_owner=1 queued=1 ready_on_owner=1 closed_failure=1 "
                 "abandoned_failure=1\n";
}

int main (int argc, char **argv)
{
    if (argc == 2 && std::string_view (argv[1]) == "--shutdown-cleanup-owner") {
        verify_shutdown_cleanup_owner_lifetime ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--expiry-owner-result") {
        verify_expiry_reports_owner_result ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--publication-failure") {
        zlink::framework::completion_owner_t owner;
        verify_publication_allocation_failure ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--no-global-registry-thread") {
        verify_no_global_registry_thread ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--registry-no-hop") {
        zlink::framework::completion_owner_t owner;
        verify_registration_does_not_wait_for_owner ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--scheduled-deadline-visits") {
        verify_scheduled_deadline_visits ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--scheduled-deadline-visits-red") {
        verify_scheduled_deadline_visits (false);
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--request-deadline") {
        zlink::framework::completion_owner_t owner;
        verify_request_deadline_index_contract ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--dispatcher-lifetime") {
        verify_provider_completion_releases_dispatcher ();
        verify_worker_lifecycle_is_rejected ();
        verify_registry_borrows_dispatcher_until_delivery ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--dispatcher-shutdown") {
        verify_dispatcher_shutdown_drains ();
        return 0;
    }
    if (argc == 2 && std::string_view (argv[1]) == "--callback-capture-lifetime") {
        zlink::framework::completion_owner_t owner;
        verify_shutdown_waits_for_callback_capture_destruction ();
        return 0;
    }
    zlink::framework::completion_owner_t owner;
    verify_completion_callbacks_use_dispatcher ();
    verify_shutdown_waits_for_callback_capture_destruction ();
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
        for (std::size_t index = 0; index < active.size (); ++index)
            assert (ordered.register_operation (id (static_cast<std::uint8_t> (index + 1)),
                                                now + std::chrono::microseconds (index ^ 63),
                                                [] (auto, auto) {}));
        const auto first_tick = *ordered.next_deadline ();
        assert (first_tick >= now && first_tick <= now + std::chrono::milliseconds (1));
        // #2889 preserves the existing reservation when C1 removes entries.
        for (std::size_t step = 0; step < active.size (); ++step) {
            assert (ordered.next_deadline () == first_tick);
            const auto removed = step * 37 % active.size ();
            assert (ordered.unregister (id (static_cast<std::uint8_t> (removed + 1))));
        }
        ordered.expire (now + std::chrono::milliseconds (256)).result ().value ();
        assert (!ordered.next_deadline () && ordered.index_size () == 0);
    }
    {
        injected_now = {};
        injected_step = {};
        foundation::operation_registry_t wheel (&read_injected_clock);
        const auto deadline = injected_now + std::chrono::milliseconds (513);
        assert (wheel.register_operation (id (1), deadline, [] (auto, auto) {}));
        const auto first_tick = *wheel.next_deadline ();
        assert (first_tick == injected_now + std::chrono::milliseconds (256));
        assert (wheel.expire (first_tick).result ().value () == 0);
        assert (*wheel.next_deadline () == first_tick + std::chrono::milliseconds (256));
        assert (wheel.expire (first_tick + std::chrono::milliseconds (256)).result ().value ()
                == 0);
        assert (*wheel.next_deadline () == deadline);
        assert (wheel.expire (deadline - std::chrono::nanoseconds (1)).result ().value () == 0);
        assert (wheel.expire (deadline).result ().value () == 1);
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
        assert (yielding.expire (tick).result ().value () == 0);
        assert (yielding.contains (foundation::call_id_t{3, 9}));
        std::size_t turns = 1;
        while (yielding.expire (tick).result ().value () == 0)
            assert (++turns < 9);
        assert (!yielding.contains (foundation::call_id_t{3, 9}));
        assert (yielding.size () == 8);
        injected_step = {};
        assert (yielding.expire (origin + std::chrono::milliseconds (257)).result ().value () == 8);
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
        auto expired = budgeted.expire (tick).result ().value ();
        assert (expired == 3);
        assert (budgeted.unregister (foundation::call_id_t{1, expired + 1}));
        assert (
          budgeted.register_operation (foundation::call_id_t{2, 1}, deadline, [] (auto, auto) {}));
        while (budgeted.size () != 0) {
            const auto resumed = budgeted.expire (tick).result ().value ();
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          assert (terminal == foundation::operation_terminal_t::completed);
          assert (payload == std::vector<std::uint8_t> ({7}));
          first_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (
      id (2), now,
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t>) {
          assert (terminal == foundation::operation_terminal_t::timed_out);
          second_terminal.fetch_add (1, std::memory_order_release);
      }));
    assert (registry.register_operation (id (3), now, [] (auto, auto) {}));
    assert (registry.unregister (id (3)));
    assert (registry.complete (id (1), zlink::message_t::from (std::vector<std::uint8_t>{7})));
    assert (!registry.complete (id (1), zlink::message_t::from (std::vector<std::uint8_t>{8})));
    // Hot-path E3: the first occupied wheel tick is no later than deadline+1ms.
    assert (registry.expire (now + std::chrono::milliseconds (1)).result ().value () == 1);
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto native_payload = std::move (completion_payload.value ());
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
          id (5), now,
          [&] (foundation::operation_terminal_t terminal,
               ::zlink::framework::result_t<zlink::message_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              shutdown_terminals.fetch_add (1, std::memory_order_release);
              throw std::runtime_error ("consumer failure");
          }));
        assert (scoped_registry.register_operation (
          id (6), now,
          [&] (foundation::operation_terminal_t terminal,
               ::zlink::framework::result_t<zlink::message_t>) {
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t>) {
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
        (void) race_registry.expire (now + std::chrono::milliseconds (1)).result ().value ();
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t>) {
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
      foundation::call_id_t{9, 3}, now,
      [&] (foundation::operation_terminal_t, ::zlink::framework::result_t<zlink::message_t>) {
          outer_callback_active = true;
          assert (reentrant_registry.register_operation (
            foundation::call_id_t{9, 4}, now,
            [&] (foundation::operation_terminal_t terminal,
                 ::zlink::framework::result_t<zlink::message_t>) {
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t>) {
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
          [&, index] (foundation::operation_terminal_t terminal,
                      ::zlink::framework::result_t<zlink::message_t>) {
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
      [&] (foundation::operation_terminal_t terminal,
           ::zlink::framework::result_t<zlink::message_t>) {
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
          [&] (foundation::operation_terminal_t terminal,
               ::zlink::framework::result_t<zlink::message_t>) {
              assert (terminal == foundation::operation_terminal_t::timed_out);
              expired_batch_callbacks.fetch_add (1, std::memory_order_release);
          }));
    }
    // Hot-path E3: a budgeted slot resumes on subsequent management turns.
    std::size_t expired_count =
      expiry_batch_registry.expire (now + std::chrono::milliseconds (1)).result ().value ();
    while (expired_count < operation_count)
        expired_count +=
          expiry_batch_registry.expire (now + std::chrono::milliseconds (1)).result ().value ();
    assert (expired_count == operation_count);
    assert (expiry_batch_registry.size () == 0);
    assert (wait_until (
      [&] { return expired_batch_callbacks.load (std::memory_order_acquire) == operation_count; }));
    verify_scheduled_deadline_visits ();
    return 0;
}
