/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Every runtime poll loop that cannot be woken states its tick as a duration and
// then re-reads the state it is waiting on. If the wait silently rounds that tick
// up, the loop runs slower than its call site says for reasons the call site
// never wrote down. std::this_thread::sleep_for does exactly that on Windows: the
// default scheduler timer period is ~15.6 ms and a shorter sleep still waits a
// whole period, so a 1 ms tick costs 15.3 ms and a 100 us backoff costs 15.2 ms.
//
// These cases pin the tick to the duration that was asked for. Replacing
// wait_poll_interval with std::this_thread::sleep_for makes both fail on Windows
// and neither fail on Linux, which is the platform gap they exist to catch.

#include "runtime/utils/poll_interval_wait.hpp"

#include <chrono>
#include <cstdio>

namespace
{
using clock_type = std::chrono::steady_clock;

// Generous against scheduler noise on a loaded machine, and still far below the
// ~15.6 ms a rounded-up sleep would cost.
constexpr auto millisecond_tick_budget = std::chrono::microseconds (6000);
constexpr auto sub_millisecond_tick_budget = std::chrono::microseconds (4000);
constexpr int iterations = 50;

// Report the median rather than the mean so one descheduled iteration on a busy
// machine cannot fail a run on its own.
std::chrono::microseconds median_wait (std::chrono::microseconds interval)
{
    std::chrono::microseconds samples[iterations];
    for (int i = 0; i < iterations; ++i) {
        const auto started = clock_type::now ();
        zlink::framework::runtime::wait_poll_interval (interval);
        samples[i] =
          std::chrono::duration_cast<std::chrono::microseconds> (clock_type::now () - started);
    }
    for (int i = 1; i < iterations; ++i) {
        const auto value = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > value) {
            samples[j + 1] = samples[j];
            --j;
        }
        samples[j + 1] = value;
    }
    return samples[iterations / 2];
}

int check (const char *label,
           std::chrono::microseconds interval,
           std::chrono::microseconds budget,
           int failure_code)
{
    const auto observed = median_wait (interval);
    std::printf ("%s requested=%lldus median=%lldus budget=%lldus\n", label,
                 static_cast<long long> (interval.count ()),
                 static_cast<long long> (observed.count ()),
                 static_cast<long long> (budget.count ()));
    if (observed < interval) {
        std::printf ("%s returned before the requested interval elapsed\n", label);
        return failure_code;
    }
    if (observed > budget) {
        std::printf ("%s exceeded its budget: the wait is rounding the tick up\n", label);
        return failure_code;
    }
    return 0;
}
}

int main ()
{
    if (const int rc =
          check ("poll tick 1ms", std::chrono::microseconds (1000), millisecond_tick_budget, 1))
        return rc;
    if (const int rc = check ("poll tick 100us", std::chrono::microseconds (100),
                              sub_millisecond_tick_budget, 2))
        return rc;

    // A zero or negative interval must not wait at all: callers past their
    // deadline re-check immediately.
    const auto started = clock_type::now ();
    zlink::framework::runtime::wait_poll_interval (std::chrono::microseconds (0));
    zlink::framework::runtime::wait_poll_interval (std::chrono::microseconds (-5));
    const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds> (clock_type::now () - started);
    if (elapsed > std::chrono::microseconds (2000)) {
        std::printf ("non-positive interval waited %lldus\n",
                     static_cast<long long> (elapsed.count ()));
        return 3;
    }

    std::printf ("poll interval wait: passed\n");
    return 0;
}
