/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../common/bench_common.hpp"

#include <cstdio>

namespace
{
using clock_type = std::chrono::steady_clock;
const auto before_close = clock_type::time_point{};
const auto close_time = before_close + std::chrono::milliseconds (1);

struct submissions_t
{
    size_t in_flight = 0;
    size_t submitted = 0;
    size_t admission_checks = 0;
    bool admitted = true;

    template <typename T> bool submit (int window, clock_type::time_point now = before_close)
    {
        if (!zlink_cpp_bench::can_submit_framework_task<T> (now, close_time, in_flight, window,
                                                            [&] {
                                                                ++admission_checks;
                                                                return admitted;
                                                            }))
            return false;
        ++in_flight;
        ++submitted;
        return true;
    }
};

int failures = 0;
void expect (bool condition, const char *message)
{
    if (!condition) {
        std::fprintf (stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main ()
{
    using zlink_cpp_bench::unlimited_request_window;
    // Keep every send unfinished: eight streams must submit before any completion.
    submissions_t sends;
    for (int stream = 0; stream < 8; ++stream)
        sends.submit<void> (8);
    std::printf ("unfinished send submissions: %zu (expected 8)\n", sends.submitted);
    expect (sends.submitted == 8, "all eight send streams can submit independently");
    expect (!sends.submit<void> (8), "the ninth send waits for completion");
    --sends.in_flight;
    expect (sends.submit<void> (8), "one completion permits one replacement send");
    expect (!sends.submit<void> (8), "replacement preserves the eight-send limit");
    expect (sends.admission_checks == 0, "sends use completion rather than request admission");

    submissions_t serial;
    expect (serial.submit<int> (1), "serial request starts with an empty window");
    expect (!serial.submit<int> (1), "serial request waits for its reply");
    --serial.in_flight;
    expect (serial.submit<int> (1), "serial reply permits the next request");

    submissions_t requests;
    expect (requests.submit<int> (unlimited_request_window), "uncapped request starts");
    requests.admitted = false;
    expect (!requests.submit<int> (unlimited_request_window),
            "uncapped request still waits for admission");
    requests.admitted = true;
    for (int request = 0; request < 101; ++request)
        expect (requests.submit<int> (unlimited_request_window),
                "admitted uncapped requests have no reply limit");
    expect (requests.in_flight == 102, "uncapped requests exceed the finite request window");

    submissions_t windowed;
    for (int request = 0; request < 100; ++request)
        expect (windowed.submit<int> (100), "finite request window permits each available slot");
    expect (!windowed.submit<int> (100), "finite request window stops at its configured limit");
    expect (!submissions_t{}.submit<void> (8, close_time), "send stops at the phase boundary");
    expect (!submissions_t{}.submit<int> (unlimited_request_window, close_time),
            "uncapped request stops at the phase boundary");
    expect (!submissions_t{}.submit<void> (8, close_time + std::chrono::milliseconds (1)),
            "send stays closed after the phase boundary");
    return failures == 0 ? 0 : 1;
}
