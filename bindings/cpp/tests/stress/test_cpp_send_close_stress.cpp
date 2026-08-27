/* SPDX-License-Identifier: MPL-2.0 */

#include "../contract/support.hpp"

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct stress_counts_t
{
    std::atomic<unsigned long long> accepted{0};
    std::atomic<unsigned long long> multipart_rejected{0};
    std::atomic<unsigned long long> backpressured{0};
    std::atomic<unsigned long long> terminated{0};
    std::atomic<unsigned long long> ownership_failures{0};
    std::atomic<unsigned long long> unexpected{0};
    std::atomic<unsigned long long> received{0};
    std::atomic<unsigned long long> bad_records{0};
    std::atomic<unsigned long long> close_ok{0};
    std::atomic<unsigned long long> close_busy{0};
};

bool expected_submit_failure (const zlink::submit_error_t &error_,
                              stress_counts_t &counts_)
{
    if (error_.result () == zlink::submit_result_t::invalid_argument
        && error_.internal_errno () == EINVAL) {
        counts_.multipart_rejected.fetch_add (1, std::memory_order_relaxed);
        return true;
    }
    if (error_.result () == zlink::submit_result_t::backpressured
        && error_.internal_errno () == EAGAIN) {
        counts_.backpressured.fetch_add (1, std::memory_order_relaxed);
        return true;
    }
    if (error_.result () == zlink::submit_result_t::terminated
        && error_.internal_errno () == ESHUTDOWN) {
        counts_.terminated.fetch_add (1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void validate_received (zlink::received_t &received_, stress_counts_t &counts_)
{
    const std::vector<zlink::message_t> &parts = received_.parts ();
    bool valid = false;
    if (parts.size () == 1) {
        valid = parts[0].to_string ().starts_with ("s:");
    } else if (parts.size () == 3) {
        const std::string first = parts[0].to_string ();
        if (first.size () > 2 && first.starts_with ("m:") && first.ends_with (":0")) {
            const std::string record = first.substr (0, first.size () - 2);
            valid = parts[1].to_string () == record + ":1"
                    && parts[2].to_string () == record + ":2";
        }
    }
    if (!valid)
        counts_.bad_records.fetch_add (1, std::memory_order_relaxed);
    counts_.received.fetch_add (1, std::memory_order_relaxed);
}

void run_round (int round_, int sender_count_, int attempts_per_sender_,
                stress_counts_t &counts_)
{
    zlink::context_t ctx;
    zlink::pair_socket_t receiver (ctx);
    zlink::pair_socket_t sender (ctx);
    zlink::socket_monitor_t receiver_monitor = receiver.monitor_open ();
    zlink::socket_monitor_t sender_monitor = sender.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-send-close-stress");
    receiver.bind (endpoint);
    sender.connect (endpoint);
    if (!zlink_cpp_contract::wait_for_socket_monitor_event (
          receiver_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000)
        || !zlink_cpp_contract::wait_for_socket_monitor_event (
          sender_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000)) {
        counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    std::atomic<bool> senders_done{false};
    std::atomic<bool> stop_senders{false};
    std::atomic<int> ready_senders{0};
    std::atomic<int> finished_senders{0};
    std::atomic<int> in_submit{0};
    std::atomic<int> completed_submits{0};
    std::barrier start_line (sender_count_ + 1);
    std::thread receiver_thread ([&] {
        const auto drain_deadline = std::chrono::steady_clock::now ()
                                    + std::chrono::seconds (15);
        for (;;) {
            zlink::received_t received;
            const int rc = receiver.recv (received, zlink::recv_flags_t::dontwait);
            if (rc == 0) {
                validate_received (received, counts_);
                continue;
            }
            if (senders_done.load (std::memory_order_acquire)
                && counts_.received.load (std::memory_order_relaxed)
                     >= counts_.accepted.load (std::memory_order_relaxed))
                return;
            if (std::chrono::steady_clock::now () >= drain_deadline) {
                counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield ();
        }
    });

    std::vector<std::thread> senders;
    senders.reserve (static_cast<size_t> (sender_count_));
    for (int thread_id = 0; thread_id < sender_count_; ++thread_id) {
        senders.emplace_back ([&, thread_id] {
            start_line.arrive_and_wait ();
            ready_senders.fetch_add (1, std::memory_order_release);
            for (int attempt = 0; attempt < attempts_per_sender_; ++attempt) {
                if (stop_senders.load (std::memory_order_acquire))
                    break;
                const bool multipart = (attempt & 1) != 0;
                const std::string id = std::to_string (round_) + ":"
                                       + std::to_string (thread_id) + ":"
                                       + std::to_string (attempt);
                zlink::message_t first = zlink_cpp_contract::make_message (
                  (multipart ? "m:" : "s:") + id + (multipart ? ":0" : ""));
                zlink::message_t second;
                zlink::message_t third;
                if (multipart) {
                    second = zlink_cpp_contract::make_message ("m:" + id + ":1");
                    third = zlink_cpp_contract::make_message ("m:" + id + ":2");
                }

                in_submit.fetch_add (1, std::memory_order_acq_rel);
                bool accepted = false;
                bool expected_failure = false;
                try {
                    accepted = multipart
                      ? sender.send ()
                          .message (first)
                          .message (second)
                          .message (third)
                          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
                          .submit ()
                      : sender.send ()
                          .message (first)
                          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
                          .submit ();
                    if (!accepted)
                        counts_.backpressured.fetch_add (1, std::memory_order_relaxed);
                }
                catch (const zlink::submit_error_t &error) {
                    expected_failure = expected_submit_failure (error, counts_);
                    if (!expected_failure)
                        counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
                }
                catch (...) {
                    counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
                }
                in_submit.fetch_sub (1, std::memory_order_acq_rel);
                completed_submits.fetch_add (1, std::memory_order_release);

                if (accepted) {
                    counts_.accepted.fetch_add (1, std::memory_order_relaxed);
                    if (first.valid () || (multipart && (second.valid () || third.valid ())))
                        counts_.ownership_failures.fetch_add (1, std::memory_order_relaxed);
                } else if (expected_failure) {
                    if (!first.valid ()
                        || (multipart && (!second.valid () || !third.valid ())))
                        counts_.ownership_failures.fetch_add (1, std::memory_order_relaxed);
                }
            }
            finished_senders.fetch_add (1, std::memory_order_release);
        });
    }

    start_line.arrive_and_wait ();
    while (ready_senders.load (std::memory_order_acquire) < sender_count_)
        std::this_thread::yield ();
    while (completed_submits.load (std::memory_order_acquire) < 7500)
        std::this_thread::yield ();
    while (in_submit.load (std::memory_order_acquire) == 0
           && finished_senders.load (std::memory_order_acquire) < sender_count_)
        std::this_thread::yield ();
    bool close_succeeded = false;
    try {
        sender.close ();
        counts_.close_ok.fetch_add (1, std::memory_order_relaxed);
        close_succeeded = true;
        stop_senders.store (true, std::memory_order_release);
    }
    catch (const zlink::close_error_t &error) {
        if (error.result () == zlink::close_result_t::busy
            && error.internal_errno () == EBUSY)
            counts_.close_busy.fetch_add (1, std::memory_order_relaxed);
        else
            counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
    }

    for (std::thread &thread : senders)
        thread.join ();
    if (!close_succeeded) {
        try {
            sender.close ();
            counts_.close_ok.fetch_add (1, std::memory_order_relaxed);
        }
        catch (...) {
            counts_.unexpected.fetch_add (1, std::memory_order_relaxed);
        }
    }
    senders_done.store (true, std::memory_order_release);
    receiver_thread.join ();
}

} // namespace

int main ()
{
    constexpr int k_rounds = 5;
    constexpr int k_sender_count = 4;
    constexpr int k_attempts_per_sender = 2500;
    stress_counts_t counts;

    for (int round = 0; round < k_rounds; ++round)
        run_round (round, k_sender_count, k_attempts_per_sender, counts);

    const unsigned long long classified =
      counts.accepted.load () + counts.multipart_rejected.load ()
      + counts.backpressured.load () + counts.terminated.load ();
    std::cout << "attempts=" << classified + counts.unexpected.load ()
              << " accepted=" << counts.accepted.load ()
              << " multipart_rejected=" << counts.multipart_rejected.load ()
              << " backpressured=" << counts.backpressured.load ()
              << " terminated=" << counts.terminated.load ()
              << " received=" << counts.received.load ()
              << " ownership_failures=" << counts.ownership_failures.load ()
              << " bad_records=" << counts.bad_records.load ()
              << " close_ok=" << counts.close_ok.load ()
              << " close_busy=" << counts.close_busy.load ()
              << " unexpected=" << counts.unexpected.load () << '\n';

    return classified >= 20000
                   && counts.received.load () == counts.accepted.load ()
                   && counts.ownership_failures.load () == 0
                   && counts.bad_records.load () == 0
                   && counts.close_ok.load () == k_rounds
                   && counts.unexpected.load () == 0
             ? 0
             : 1;
}
