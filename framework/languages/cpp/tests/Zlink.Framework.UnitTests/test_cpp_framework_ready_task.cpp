/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>

using namespace zlink::framework;

namespace
{
thread_local bool count_sync = false;
thread_local unsigned mutex_locks = 0;
thread_local unsigned notifications = 0;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void begin_count ()
{
    mutex_locks = 0;
    notifications = 0;
    count_sync = true;
}

void end_count ()
{
    count_sync = false;
}

template <typename T> task_t<T> make_ready (result_t<T> result, const char *name)
{
    begin_count ();
    task_t<T> task (std::move (result));
    end_count ();
    std::cout << name << ": locks=" << mutex_locks << " notifications=" << notifications << '\n';
    require (mutex_locks == 0, "ready task construction locked an unpublished result");
    require (notifications == 0, "ready task construction notified without a waiter");
    require (task.await_ready (), "ready task was not ready");
    return task;
}

void check_failure (const result_t<int> &result)
{
    require (!result.has_value (), "failure became success");
    require (result.error_kind () == framework_error_kind_t::rejected, "failure kind changed");
    try {
        result.value ();
        require (false, "failure did not throw");
    }
    catch (const framework_exception_t &error) {
        require (std::string (error.what ()) == "ready failure", "failure message changed");
    }
}
} // namespace

extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (count_sync)
        ++mutex_locks;
    return __real_pthread_mutex_lock (mutex);
}

extern "C" void __real__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition)
{
    if (count_sync)
        ++notifications;
    __real__ZNSt18condition_variable10notify_allEv (condition);
}

int main ()
{
    try {
        std::mutex mutex;
        std::condition_variable condition;
        begin_count ();
        mutex.lock ();
        mutex.unlock ();
        condition.notify_all ();
        end_count ();
        require (mutex_locks == 1 && notifications == 1, "synchronization counter calibration");
        std::cout << "counter calibration: locks=1 notifications=1\n";

        auto value = make_ready (result_t<int>::success (42), "value success");
        require (value.result ().value () == 42, "ready value changed");
        auto empty = make_ready (result_t<void>::success (), "void success");
        empty.await_resume ();
        auto failed =
          make_ready (result_t<int>::failure (framework_error_kind_t::rejected, "ready failure"),
                      "value failure");
        check_failure (failed.result ());
        auto void_failed =
          make_ready (result_t<void>::failure (framework_error_kind_t::rejected, "ready failure"),
                      "void failure");
        require (!void_failed.result ().has_value (), "void failure became success");
        try {
            void_failed.await_resume ();
            require (false, "void failure did not throw");
        }
        catch (const framework_exception_t &error) {
            require (error.kind () == framework_error_kind_t::rejected,
                     "void failure kind changed");
            require (std::string (error.what ()) == "ready failure",
                     "void failure message changed");
        }
        auto owned = make_ready (
          result_t<std::unique_ptr<int>>::success (std::make_unique<int> (7)), "move-only success");
        require (*owned.result ().value () == 7, "move-only value changed");
        auto moved = std::move (owned);
        require (*moved.result ().value () == 7, "moved task lost result");

        task_completion_source_t<int> pending;
        auto pending_task = pending.task ();
        require (!pending_task.await_ready (), "pending task started ready");
        require (pending.complete (result_t<int>::success (9)), "pending completion rejected");
        require (pending_task.await_ready () && pending_task.result ().value () == 9,
                 "pending result not published");
        require (!pending.complete (result_t<int>::success (10)), "duplicate completion accepted");
        require (pending_task.result ().value () == 9, "duplicate completion replaced result");
        task_completion_source_t<void> pending_void;
        auto pending_void_task = pending_void.task ();
        require (!pending_void_task.await_ready (), "void pending task started ready");
        require (pending_void.complete (result_t<void>::success ()),
                 "void pending completion rejected");
        require (pending_void_task.await_ready (), "void pending result not published");
        pending_void_task.await_resume ();
        std::cout << "ready task and pending completion semantics passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        end_count ();
        std::cerr << error.what () << '\n';
        return 1;
    }
}
