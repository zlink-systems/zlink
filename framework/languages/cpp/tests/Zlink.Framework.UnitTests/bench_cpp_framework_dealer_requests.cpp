/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/backend/raw_dealer_port.hpp"
#include <zlink.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace fw = zlink::framework;
namespace backend = fw::detail::backend;
using namespace std::chrono_literals;

template <typename Port> void visit_deadlines (Port &port)
{
    // Older main owns a per-request scheduler; the revised port exposes the
    // management visit normally driven by its runtime owner.
    if constexpr (requires { port.expire_requests (std::chrono::steady_clock::now ()); })
        port.expire_requests (std::chrono::steady_clock::now ());
}

template <typename Port> zlink::poll_event_flag_t poll_events (Port &port)
{
    const auto events = port.poll (10ms);
    if constexpr (requires { events.value_or (zlink::poll_event_flag_t::none); })
        return events.value_or (zlink::poll_event_flag_t::none);
    else
        return events;
}

// Same raw request/reply path as raw_route_port_contract, including binding
// admission, completion polling and framework deadline registration.
void measure (std::size_t threads)
{
    constexpr std::size_t requests = 1000;
    zlink::context_t context;
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    server.options ().linger (0ms);
    client.options ().linger (0ms);
    server.bind ("inproc://framework-dealer-request-cost");
    client.connect ("inproc://framework-dealer-request-cost");
    zlink::poller_t poller;
    backend::raw_route_port_t responder (server);
    backend::raw_dealer_port_t requester (client, nullptr, &poller);
    std::atomic_bool stop{false};
    const backend::raw_message_t payload{backend::raw_bytes_t (1024, 0x5a)};
    std::thread responding ([&] {
        while (!stop.load (std::memory_order_acquire)) {
            auto received = responder.receive_if_ready (poll_events (responder));
            if (received && !responder.reply (*received, payload))
                std::abort ();
        }
    });
    std::thread pumping ([&] {
        while (!stop.load (std::memory_order_acquire)) {
            zlink::poll_event_t event;
            (void) poller.wait (&event, 1, 10ms);
            (void) requester.try_receive ();
            visit_deadlines (requester);
        }
    });
    std::barrier ready (static_cast<std::ptrdiff_t> (threads + 1));
    std::barrier start (static_cast<std::ptrdiff_t> (threads + 1));
    std::barrier finish (static_cast<std::ptrdiff_t> (threads + 1));
    std::vector<std::thread> callers;
    for (std::size_t thread = 0; thread < threads; ++thread) {
        callers.emplace_back ([&] {
            const auto call = [&] {
                auto pending = requester.request (payload, 2s);
                const auto &result = pending.result ();
                if (!result || result.value ().terminal != zlink::request_result_t::ok)
                    std::abort ();
            };
            for (std::size_t i = 0; i < 32; ++i)
                call ();
            ready.arrive_and_wait ();
            start.arrive_and_wait ();
            for (std::size_t i = 0; i < requests; ++i)
                call ();
            finish.arrive_and_wait ();
        });
    }
    ready.arrive_and_wait ();
    fw::tests::process_allocation_count.store (0);
    fw::tests::tracking_process_allocations.store (true);
    const auto began = std::chrono::steady_clock::now ();
    start.arrive_and_wait ();
    finish.arrive_and_wait ();
    const auto elapsed =
      std::chrono::duration<double> (std::chrono::steady_clock::now () - began).count ();
    fw::tests::tracking_process_allocations.store (false);
    const auto allocations = fw::tests::process_allocation_count.load ();
    for (auto &caller : callers)
        caller.join ();
    stop.store (true, std::memory_order_release);
    pumping.join ();
    responding.join ();
    requester.close ();
    responder.close ();
    const auto count = requests * threads;
    std::printf ("threads=%zu requests=%zu payload_bytes=1024 seconds=%.9f "
                 "requests_per_second=%.3f allocations=%zu allocations_per_request=%.6f\n",
                 threads, count, elapsed, count / elapsed, allocations,
                 double (allocations) / count);
}

int main ()
{
    fw::completion_owner_t owner;
    measure (1);
    measure (8);
}
