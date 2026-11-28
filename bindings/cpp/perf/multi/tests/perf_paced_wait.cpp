/* SPDX-License-Identifier: MPL-2.0 */
#include "../common/perf_multi_measurement.hpp"

#include <cassert>

int main ()
{
    using perf_multi_measurement::ns_per_ms;
    // rate 1/s: the next send after one acceptance is epoch + 1 s.
    perf_multi_measurement::schedule_t schedule (1.0, 0);
    schedule.accepted (0);
    const uint64_t next = schedule.next_ns ();
    const uint64_t far_end = next + 10 * 1000 * ns_per_ms;
    // Under 1 ms left: recheck now instead of sleeping past the send.
    assert (schedule.wait_until_due_ms (next - ns_per_ms / 2, far_end).count () == 0);
    // Floor to ms.
    assert (schedule.wait_until_due_ms (next - 7 * ns_per_ms - 900000ULL, far_end).count () == 7);
    // Cap at 50 ms.
    assert (schedule.wait_until_due_ms (next - 400 * ns_per_ms, far_end).count () == 50);
    // end_ns before the next send bounds the wait.
    assert (schedule.wait_until_due_ms (next - 400 * ns_per_ms, next - 397 * ns_per_ms).count () == 3);
    // Deadline reached or passed.
    assert (schedule.wait_until_due_ms (next, far_end).count () == 0);
    assert (schedule.wait_until_due_ms (next + ns_per_ms, far_end).count () == 0);
    assert (schedule.wait_until_due_ms (next - 400 * ns_per_ms, next - 400 * ns_per_ms).count () == 0);
}
