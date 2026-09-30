/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/framework/contracts/dispatch/task.hpp>

#include <barrier>
#include <future>
#include <thread>
#include <type_traits>

using namespace zlink::framework;
using namespace std::chrono_literals;

static_assert (!std::is_copy_constructible_v<task_completion_source_t<int>>);
static_assert (!std::is_copy_assignable_v<task_completion_source_t<void>>);
static_assert (std::is_nothrow_move_constructible_v<task_completion_source_t<int>>);
static_assert (
  std::is_same_v<decltype (std::declval<const task_completion_source_t<int> &> ().task ()),
                 task_t<int>>);
static_assert (std::is_same_v<decltype (std::declval<task_completion_source_t<void> &> ().complete (
                                result_t<void>::success ())),
                              bool>);

task_t<int> observe_value (task_t<int> task)
{
    co_return co_await task;
}

task_t<void> observe_void (task_t<void> task)
{
    co_await task;
}

int main ()
{
    task_completion_source_t<int> source;
    const auto &const_source = source;
    auto first = observe_value (const_source.task ());
    auto second = const_source.task ();
    if (first.result_for (0ms) || second.result_for (1ms))
        return 1;
    std::thread producer ([&] {
        if (!source.complete (result_t<int>::success (42)))
            std::terminate ();
    });
    producer.join ();
    if (first.result_for (1s)->value () != 42 || second.result ().value () != 42
        || source.complete (result_t<int>::success (99)))
        return 2;
    auto ready_waiter = observe_value (source.task ());
    if (ready_waiter.result ().value () != 42)
        return 3;

    task_completion_source_t<void> failure;
    auto failed = observe_void (failure.task ());
    std::thread failing_producer ([&] {
        failure.complete (
          result_t<void>::failure (framework_error_kind_t::unavailable, "external event failed"));
    });
    failing_producer.join ();
    const auto failure_result = failed.result_for (1s);
    if (!failure_result || *failure_result
        || failure_result->error_kind () != framework_error_kind_t::unavailable)
        return 4;

    for (int iteration = 0; iteration != 100; ++iteration) {
        task_completion_source_t<int> racing;
        std::barrier registration (2);
        std::thread completer ([&] {
            registration.arrive_and_wait ();
            racing.complete (result_t<int>::success (iteration));
        });
        registration.arrive_and_wait ();
        auto waiter = observe_value (racing.task ());
        completer.join ();
        if (waiter.result_for (1s)->value () != iteration)
            return 5;
    }

    task_t<int> orphan (result_t<int>::success (0));
    {
        task_completion_source_t<int> owner;
        orphan = owner.task ();
    }
    if (orphan.result_for (0ms))
        return 6;
    task_completion_source_t<int> survivor;
    {
        auto discarded = survivor.task ();
    }
    auto retained = survivor.task ();
    if (!survivor.complete (result_t<int>::success (7)) || retained.result ().value () != 7)
        return 7;
    return 0;
}
