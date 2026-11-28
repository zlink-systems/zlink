/* SPDX-License-Identifier: MPL-2.0 */
#include "../common/perf_dealer_dealer_receive.hpp"

#include <iostream>

int main ()
{
    using clock_t = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    const auto start = clock_t::time_point (1s);
    const auto deadline = start + 1s;
    const size_t msg_size = 64;
    perf_metric::header_t header;
    perf_metric::init_header (&header, 1U, perf_metric::phase_active, msg_size, 1, 1);

    const auto check = [&] (const char *label, clock_t::time_point now, uint64_t expected) {
        uint64_t count = 0;
        const bool matched = perf::multi::record_dealer_dealer_receive (
          header, perf_metric::phase_active, msg_size, now, start, deadline, count);
        if (matched && count == expected)
            return true;
        std::cerr << label << ": expected active count " << expected << ", got " << count
                  << " (matched=" << matched << ")\n";
        return false;
    };

    const bool before = check ("one nanosecond before deadline", deadline - 1ns, 1);
    const bool at = check ("at deadline", deadline, 0);
    const bool after = check ("one nanosecond after deadline", deadline + 1ns, 0);
    return before && at && after ? 0 : 1;
}
