/* SPDX-License-Identifier: MPL-2.0 */
#include "../../common/perf_socket_adapter.hpp"
#include "../common/perf_client_helpers.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace std::chrono;
using schedule_t = perf_multi_measurement::schedule_t;

struct counts_t
{
    size_t blocked = 0;
    size_t while_blocked = 0;
    size_t resumed = 0;
};

uint64_t now_ns ()
{
    return duration_cast<nanoseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

void connect_pair (zlink::dealer_socket_t &sender_,
                   zlink::dealer_socket_t &receiver_,
                   const char *endpoint_,
                   uint64_t hwm_)
{
    sender_.options ().send_hwm (zlink::byte_count_t::bytes (hwm_));
    receiver_.options ().recv_hwm (zlink::byte_count_t::bytes (hwm_));
    sender_.options ().send_timeout (seconds (1));
    receiver_.options ().recv_timeout (seconds (1));
    receiver_.bind (endpoint_);
    sender_.connect (endpoint_);

    auto probe = zlink::message_t::from (std::string ("ready"));
    sender_.send ().message (probe).submit ();
    zlink::received_t received;
    if (receiver_.recv (received) != 0)
        throw std::runtime_error ("inproc pair did not become ready");
}

size_t drain (zlink::dealer_socket_t &receiver_)
{
    size_t count = 0;
    zlink::received_t received;
    while (receiver_.recv (received, zlink::recv_flags_t::dontwait) == 0)
        ++count;
    return count;
}

perf::async_task_t<bool> send_until_blocked (perf::application_ready_queue_t &queue_,
                                             schedule_t &schedule_,
                                             zlink::dealer_socket_t &sender_,
                                             steady_clock::time_point end_,
                                             counts_t &counts_)
{
    co_await queue_.schedule ();
    while (steady_clock::now () < end_) {
        if (!schedule_.due (now_ns ())) {
            co_await queue_.schedule ();
            continue;
        }
        auto message = zlink::message_t::from (std::string ("blocked payload"));
        auto submission = sender_.send ().message (message).async ();
        const auto due = schedule_.submitted ();
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            ++counts_.blocked;
            co_await std::move (submission.admitted);
            ++counts_.resumed;
        } else if (submission.result != ZLINK_SUBMIT_OK) {
            co_return false;
        }
        schedule_.accepted (now_ns (), due);
        if (counts_.resumed != 0)
            co_return true;
    }
    co_return false;
}

perf::async_task_t<bool> send_on_healthy_socket (perf::application_ready_queue_t &queue_,
                                                 schedule_t &schedule_,
                                                 zlink::dealer_socket_t &sender_,
                                                 zlink::dealer_socket_t &blocked_receiver_,
                                                 steady_clock::time_point end_,
                                                 counts_t &counts_)
{
    co_await queue_.schedule ();
    while (steady_clock::now () < end_) {
        if (!schedule_.due (now_ns ())) {
            co_await queue_.schedule ();
            continue;
        }
        auto message = zlink::message_t::from (std::string ("healthy payload"));
        auto submission = sender_.send ().message (message).async ();
        if (submission.result != ZLINK_SUBMIT_OK)
            co_return false;
        schedule_.accepted (now_ns (), schedule_.submitted ());
        if (counts_.blocked != 0 && counts_.resumed == 0 && ++counts_.while_blocked == 4) {
            // Release the blocked pipe only after the other sender has progressed.
            co_return drain (blocked_receiver_) != 0;
        }
    }
    co_return false;
}
}

bool check_sender_turns ()
{
    using namespace perf_multi_measurement;
    message_burst_t burst;
    for (size_t sender = 0; sender < 3; ++sender) {
        for (size_t n = 1; n <= 64; ++n) {
            if (burst.complete () != (n == 64))
                return false;
        }
    }
    burst.complete ();
    burst.reset ();
    for (size_t n = 1; n <= 64; ++n) {
        if (burst.complete () != (n == 64))
            return false;
    }
    return true;
}

bool check_reply_turns ()
{
    using namespace perf_multi_measurement;
    perf::multi::echo_reply_drain_t replies;
    schedule_t schedule (1000, 0);
    const auto submit_due = [&] {
        if (schedule.due (1000000))
            schedule.accepted (1000000);
    };
    for (size_t n = 1; n <= 192; ++n) {
        replies.service_paced_reply (nullptr, 0, submit_due);
        replies.service_paced_reply (&schedule, -1, submit_due);
        replies.service_paced_reply (&schedule, 0, submit_due);
        if (schedule.accepted_count != std::min<size_t> (n / 64, 2))
            return false;
    }
    return true;
}

bool check_catch_up ()
{
    using namespace perf_multi_measurement;
    const uint64_t later = 2 * catch_up_window_ns;
    // The slowest cumulative subscriber changes. Each subscriber's delta matters.
    catch_up_tracker_t short_by_one (200.0);
    const bool starts_caught_up = short_by_one.observe (0, {0, 10}, 500);
    const bool caught_up = short_by_one.observe (catch_up_window_ns, {100, 109}, 600);
    catch_up_tracker_t short_by_two (200.0);
    short_by_two.observe (0, {0, 10}, 500);
    const bool missed = short_by_two.observe (catch_up_window_ns, {100, 108}, 600);
    catch_up_tracker_t grew (200.0);
    grew.observe (0, {0, 0}, 500);
    grew.observe (catch_up_window_ns / 2, {49, 49}, 550);
    const bool early = grew.observe (catch_up_window_ns, {100, 100}, 600);
    const bool settled = grew.observe (later, {300, 300}, 800);
    return !starts_caught_up && caught_up && !missed && !early && settled;
}

int main ()
{
    const bool sender_turns = check_sender_turns ();
    const bool reply_turns = check_reply_turns ();
    const bool catch_up = check_catch_up ();
    std::cout << "rule 1 sender turns: " << (sender_turns ? "PASS" : "FAIL")
              << "\nrule 4 reply turns: " << (reply_turns ? "PASS" : "FAIL")
              << "\nrule 5 catch-up: " << (catch_up ? "PASS" : "FAIL") << std::endl;
    if (!sender_turns || !reply_turns || !catch_up)
        return 1;

    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::dealer_socket_t blocked (context);
    zlink::dealer_socket_t healthy (context);
    zlink::dealer_socket_t blocked_receiver (context);
    zlink::dealer_socket_t healthy_receiver (context);
    connect_pair (blocked, blocked_receiver, "inproc://perf-paced-blocked", 128);
    connect_pair (healthy, healthy_receiver, "inproc://perf-paced-healthy", 4096000);

    zlink::poller_t poller;
    poller.add (blocked, zlink::poll_event_flag_t::pollcompletion, 0);
    poller.add (healthy, zlink::poll_event_flag_t::pollcompletion, 1);
    perf::application_poller_coordinator_t coordinator (poller, 2);
    schedule_t schedule (20000, now_ns ());
    const auto end = steady_clock::now () + milliseconds (250);
    counts_t counts;
    std::vector<perf::async_task_t<bool>> senders;
    senders.emplace_back (
      send_until_blocked (coordinator.ready_queue (), schedule, blocked, end, counts));
    senders.emplace_back (send_on_healthy_socket (coordinator.ready_queue (), schedule, healthy,
                                                  blocked_receiver, end, counts));
    const bool drained = coordinator.run_until_senders_drained (
      end, end + seconds (1), senders, [] (const zlink::poll_event_t *, size_t) { return true; },
      &schedule);
    const size_t received = drain (healthy_receiver);
    std::cout << "blocked=" << counts.blocked << " healthy_while_blocked=" << counts.while_blocked
              << " resumed=" << counts.resumed << " healthy_received=" << received << '\n';
    if (!drained || counts.blocked != 1 || counts.while_blocked != 4 || counts.resumed != 1
        || received < counts.while_blocked)
        return 1;
    return senders[0].get () && senders[1].get () ? 0 : 1;
}
