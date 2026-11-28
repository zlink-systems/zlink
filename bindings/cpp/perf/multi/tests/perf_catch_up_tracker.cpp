/* SPDX-License-Identifier: MPL-2.0 */
#include "../common/perf_multi_measurement.hpp"

#include <cassert>

namespace
{
void test_pubsub_catch_up_accepts_large_stable_backlog ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t tracker (100.0);
    assert (!tracker.observe (0, {100}, 1100));
    assert (!tracker.observe (catch_up_window_ns / 2, {125}, 1125));
    assert (tracker.observe (catch_up_window_ns, {150}, 1150));
    assert (tracker.in_flight_at_catch_up () == 1000);
    assert (tracker.window_start_in_flight () == 1000);
}

void test_pubsub_catch_up_rejects_growing_backlog ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t tracker (100.0);
    assert (!tracker.observe (0, {100}, 100));
    assert (!tracker.observe (catch_up_window_ns / 2, {125}, 126));
    assert (!tracker.observe (catch_up_window_ns, {150}, 151));
}

void test_pubsub_catch_up_rejects_receive_rate_below_target ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t tracker (100.0);
    assert (!tracker.observe (0, {103}, 100));
    assert (!tracker.observe (catch_up_window_ns, {150}, 150));
    assert (tracker.observe (2 * catch_up_window_ns, {200}, 200));
}

// Large messages: 216/s gives about 108 scheduled sends per window. The
// receive count is compared with the scheduled count of the same window,
// not with rate * elapsed, which an integer count can miss by one message.
void test_pubsub_catch_up_counts_scheduled_sends_in_window ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t tracker (216.0);
    assert (!tracker.observe (0, {0, 0}, 1));
    assert (tracker.observe (catch_up_window_ns + 300000, {108, 109}, 109));
}

// Each subscriber may be observed between two arrivals; one message short
// is quantization, two messages short is a receive rate below target.
void test_pubsub_catch_up_allows_one_message_per_subscriber_phase ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t short_by_one (216.0);
    assert (!short_by_one.observe (0, {1, 2}, 2));
    assert (short_by_one.observe (catch_up_window_ns, {110, 110}, 111));
    catch_up_tracker_t short_by_two (216.0);
    assert (!short_by_two.observe (0, {1, 3}, 3));
    assert (!short_by_two.observe (catch_up_window_ns, {111, 111}, 113));
}

void test_pubsub_catch_up_restarts_on_backlog_growth ()
{
    using namespace perf_multi_measurement;
    const uint64_t growth_ns = catch_up_window_ns / 2;
    catch_up_tracker_t tracker (100.0);
    assert (!tracker.observe (0, {100}, 100));
    assert (!tracker.observe (growth_ns, {124}, 125));
    assert (!tracker.observe (catch_up_window_ns, {150}, 150));
    assert (tracker.observe (growth_ns + catch_up_window_ns, {175}, 175));
    assert (tracker.in_flight_at_catch_up () == 0);
    assert (tracker.window_start_in_flight () == 1);
}

void test_pubsub_catch_up_checks_each_subscriber_in_the_window ()
{
    using namespace perf_multi_measurement;
    catch_up_tracker_t tracker (100.0);
    assert (!tracker.observe (0, {100, 130}, 100));
    assert (!tracker.observe (catch_up_window_ns, {161, 160}, 150));
    assert (tracker.observe (2 * catch_up_window_ns, {211, 210}, 200));
}

void test_slowest_subscriber_changes ()
{
    using namespace perf_multi_measurement;
    const uint64_t later = 2 * catch_up_window_ns;
    catch_up_tracker_t short_by_one (200.0);
    assert (!short_by_one.observe (0, {0, 10}, 500));
    assert (short_by_one.observe (catch_up_window_ns, {100, 109}, 600));
    catch_up_tracker_t short_by_two (200.0);
    assert (!short_by_two.observe (0, {0, 10}, 500));
    assert (!short_by_two.observe (catch_up_window_ns, {100, 108}, 600));
    catch_up_tracker_t grew (200.0);
    assert (!grew.observe (0, {0, 0}, 500));
    assert (!grew.observe (catch_up_window_ns / 2, {49, 49}, 550));
    assert (!grew.observe (catch_up_window_ns, {100, 100}, 600));
    assert (grew.observe (later, {300, 300}, 800));
}

} // namespace

int main ()
{
    static_assert (perf_multi_measurement::catch_up_tracker_t::receive_quantum == 1);
    test_pubsub_catch_up_accepts_large_stable_backlog ();
    test_pubsub_catch_up_rejects_growing_backlog ();
    test_pubsub_catch_up_rejects_receive_rate_below_target ();
    test_pubsub_catch_up_counts_scheduled_sends_in_window ();
    test_pubsub_catch_up_allows_one_message_per_subscriber_phase ();
    test_pubsub_catch_up_restarts_on_backlog_growth ();
    test_pubsub_catch_up_checks_each_subscriber_in_the_window ();
    test_slowest_subscriber_changes ();
}
