/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime/dispatch/offload_executor.hpp"

#include <atomic>
#include <cassert>
#include <future>
#include <stdexcept>
#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rt = zlink::framework::runtime;
using namespace std::chrono_literals;

namespace zlink::framework::runtime
{
struct offload_executor_test_access_t
{
    static std::size_t idle (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._idle_workers;
    }
    static bool stopping (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._stopping;
    }
};
}

namespace
{
template <typename Predicate> void await (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!predicate ()) {
        assert (std::chrono::steady_clock::now () < deadline);
        std::this_thread::yield ();
    }
}
template <typename T> void await (std::future<T> &future)
{
    assert (future.wait_for (5s) == std::future_status::ready);
    future.get ();
}
struct test_role_t final : rt::offload_executor_t::role_t
{
    std::function<bool ()> run;
    bool run_until_handoff () override { return run (); }
};

void handoff_keeps_receiving_and_drains_application_before_roles ()
{
    rt::offload_executor_t executor (2);
    auto role = std::make_shared<test_role_t> ();
    std::promise<void> first_entered, assign, application_started, release_application;
    std::promise<void> receiving, stop_receiving, internal_ran;
    auto assigned = assign.get_future ();
    auto release = release_application.get_future ();
    auto stop = stop_receiving.get_future ();
    std::atomic_size_t turns{0};
    std::atomic_bool rotating{false};
    std::thread::id first, second;
    role->run = [&] () -> bool {
        assert (!rotating.exchange (true));
        const auto turn = turns.fetch_add (1);
        if (turn == 0) {
            first = std::this_thread::get_id ();
            rotating.store (false);
            first_entered.set_value ();
            await (assigned);
            const auto threads = executor.owned_thread_count ();
            assert (executor.try_assign_role (role));
            assert (executor.owned_thread_count () == threads);
            rt::offload_executor_t::run_application_turn ([&] {
                application_started.set_value ();
                await (release);
                throw std::runtime_error ("application exception remains isolated");
            });
            return true;
        }
        second = std::this_thread::get_id ();
        rotating.store (false);
        receiving.set_value ();
        await (stop);
        return false;
    };
    assert (executor.start_role (role));
    auto entered = first_entered.get_future ();
    await (entered);
    await ([&] { return rt::offload_executor_test_access_t::idle (executor) == 1; });
    assign.set_value ();
    auto application = application_started.get_future ();
    auto receiver = receiving.get_future ();
    await (application);
    await (receiver);
    assert (first != second && turns == 2);
    assert (executor.owned_thread_count () == 2);
    auto draining = std::async (std::launch::async, [&] { executor.drain_application (); });
    await ([&] { return rt::offload_executor_test_access_t::stopping (executor); });
    assert (!executor.try_assign_role (role));
    assert (executor.try_submit_internal ([&] { internal_ran.set_value (); }));
    assert (draining.wait_for (0s) != std::future_status::ready);
    release_application.set_value ();
    auto internal = internal_ran.get_future ();
    await (internal);
    await (draining);
    assert (!executor.drained ());
    assert (!executor.try_submit_internal ([] {}));
    assert (!executor.start_role (role));
    stop_receiving.set_value ();
    executor.drain ();
    assert (executor.drained () && executor.live_worker_count () == 0);
}

void no_idle_worker_preserves_holder_and_existing_queue ()
{
    rt::offload_executor_t executor (1);
    auto role = std::make_shared<test_role_t> ();
    std::promise<void> retained, stop_receiving;
    auto stop = stop_receiving.get_future ();
    std::atomic_bool ran{false};
    role->run = [&] () -> bool {
        const auto holder = std::this_thread::get_id ();
        assert (!executor.try_assign_role (role));
        assert (executor.try_submit_internal ([&] { ran.store (true); }));
        assert (executor.owned_thread_count () == 1 && !ran.load ());
        retained.set_value ();
        await (stop);
        assert (std::this_thread::get_id () == holder);
        return false;
    };
    assert (executor.start_role (role));
    auto fallback = retained.get_future ();
    await (fallback);
    stop_receiving.set_value ();
    executor.drain ();
    assert (ran.load () && executor.live_worker_count () == 0);
}

void roles_precede_application_queue ()
{
    rt::offload_executor_t executor (1);
    std::promise<void> busy, release;
    auto released = release.get_future ();
    assert (executor.try_submit ([&] {
        busy.set_value ();
        await (released);
    }));
    auto started = busy.get_future ();
    await (started);
    std::atomic_bool application_ran{false}, role_ran{false};
    assert (executor.try_submit_internal ([&] { application_ran.store (true); }));
    auto role = std::make_shared<test_role_t> ();
    role->run = [&] () -> bool {
        assert (!application_ran.load ());
        role_ran.store (true);
        return false;
    };
    assert (executor.start_role (role));
    release.set_value ();
    executor.drain ();
    assert (role_ran.load () && application_ran.load ());
}

void empty_role_list_preserves_retirement_and_internal_drain ()
{
    rt::offload_executor_t executor (0, 1, 1ms);
    std::promise<void> started, release, nested;
    auto released = release.get_future ();
    assert (executor.try_submit ([&] {
        started.set_value ();
        await (released);
        assert (executor.try_submit_internal ([&] { nested.set_value (); }));
    }));
    auto running = started.get_future ();
    await (running);
    release.set_value ();
    auto continuation = nested.get_future ();
    await (continuation);
    await ([&] { return executor.live_worker_count () == 0; });
    assert (executor.drained ());
    std::atomic_bool restarted{false};
    assert (executor.try_submit_internal ([&] { restarted.store (true); }));
    executor.drain ();
    assert (restarted.load () && executor.live_worker_count () == 0);
    assert (!executor.try_submit_internal ([] {}));
}

void continuous_handoffs_stop_without_losing_turns ()
{
    rt::offload_executor_t executor (2);
    auto role = std::make_shared<test_role_t> ();
    std::atomic_bool stop{false}, rotating{false};
    std::atomic_size_t assigned{0}, delivered{0};
    std::size_t receive_state = 0;
    role->run = [&] {
        while (!stop.load ()) {
            assert (!rotating.exchange (true));
            ++receive_state;
            rotating.store (false);
            if (executor.try_assign_role (role)) {
                ++assigned;
                rt::offload_executor_t::run_application_turn ([&] { ++delivered; });
                return true;
            }
            std::this_thread::yield ();
        }
        return false;
    };
    assert (executor.start_role (role));
    await ([&] { return delivered.load () >= 1000; });
    executor.drain_application ();
    stop.store (true);
    executor.drain ();
    assert (assigned == delivered && receive_state >= assigned);
    assert (executor.owned_thread_count () == 0 && executor.live_worker_count () == 0);
}

void two_roles_cannot_reserve_the_same_idle_worker ()
{
    rt::offload_executor_t executor (3);
    std::promise<void> go_promise, stop_promise, release_promise;
    auto go = go_promise.get_future ().share ();
    auto stop = stop_promise.get_future ().share ();
    auto release = release_promise.get_future ().share ();
    std::atomic_size_t entered{0}, attempted{0}, succeeded{0};
    std::atomic_size_t calls[2]{};
    std::shared_ptr<test_role_t> roles[2];
    for (std::size_t index = 0; index < 2; ++index) {
        roles[index] = std::make_shared<test_role_t> ();
        roles[index]->run = [&, index] {
            if (++calls[index] == 1) {
                ++entered;
                assert (go.wait_for (5s) == std::future_status::ready);
                const bool assigned = executor.try_assign_role (roles[index]);
                if (assigned)
                    ++succeeded;
                ++attempted;
                if (assigned) {
                    rt::offload_executor_t::run_application_turn (
                      [&] { assert (release.wait_for (5s) == std::future_status::ready); });
                    return true;
                }
            }
            assert (stop.wait_for (5s) == std::future_status::ready);
            return false;
        };
        assert (executor.start_role (roles[index]));
    }
    await (
      [&] { return entered == 2 && rt::offload_executor_test_access_t::idle (executor) == 1; });
    go_promise.set_value ();
    await ([&] { return attempted == 2; });
    assert (succeeded == 1 && executor.owned_thread_count () == 3);
    release_promise.set_value ();
    executor.drain_application ();
    stop_promise.set_value ();
    executor.drain ();
    assert (executor.live_worker_count () == 0);
}

void rotation_exception_terminates ()
{
#if defined(__unix__)
    const auto child = fork ();
    assert (child >= 0);
    if (child == 0) {
        std::set_terminate ([] { _exit (73); });
        rt::offload_executor_t executor (1);
        auto role = std::make_shared<test_role_t> ();
        role->run = [] () -> bool { throw std::runtime_error ("rotation"); };
        assert (executor.start_role (role));
        executor.drain ();
        _exit (74);
    }
    int status = 0;
    assert (waitpid (child, &status, 0) == child);
    assert (WIFEXITED (status) && WEXITSTATUS (status) == 73);
#endif
}
} // namespace

int main ()
{
    rotation_exception_terminates ();
    handoff_keeps_receiving_and_drains_application_before_roles ();
    no_idle_worker_preserves_holder_and_existing_queue ();
    roles_precede_application_queue ();
    continuous_handoffs_stop_without_losing_turns ();
    two_roles_cannot_reserve_the_same_idle_worker ();
    empty_role_list_preserves_retirement_and_internal_drain ();
}
