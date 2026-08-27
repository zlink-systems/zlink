/* SPDX-License-Identifier: MPL-2.0 */

// Routed send is a synchronous submit that wraps the Core send. The binding
// owns no park queue, no WRITABLE-callback retry, and no deadline timer, so
// the properties worth pinning here are Core's: a blocking submit waits inside
// Core and resumes on the Core credit signal, SNDTIMEO bounds that wait, and a
// send that never gets credit is reported to the caller with its part intact.

#include "support.hpp"

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <zlink/message/api.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace
{

bool send_dealer_dontwait (zlink::dealer_socket_t &dealer_,
                           const std::string &payload_)
{
    zlink_msg_t part;
    assert (zlink_msg_init_size (&part, payload_.size ()) == ZLINK_CONFIG_OK);
    if (!payload_.empty ())
        std::memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
    const zlink_submit_result_t result = zlink_send_part (
      zlink::detail::native_handle (dealer_), &part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    assert (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    return result == ZLINK_SUBMIT_OK;
}

bool fill_until_backpressured (zlink::dealer_socket_t &dealer_,
                               const std::string &filler_)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!send_dealer_dontwait (dealer_, filler_))
            return true;
    }
    return false;
}

int test_blocking_routed_send_resumes_on_core_credit ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("resume-source"));
    router.set_routing_id (zlink::routing_id_t::from ("resume-sink"));

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
    dealer.options ().send_timeout (std::chrono::seconds (5));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-resume");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    const std::string filler (65536, 'f');
    if (!fill_until_backpressured (dealer, filler)) {
        std::fprintf (stderr, "could not reach the send HWM\n");
        return 1;
    }

    // Nothing but Core can release this submit: the binding has no reactor
    // thread, no retry queue, and no timer left to move it along.
    std::atomic<bool> submitted{false};
    std::atomic<bool> failed{false};
    const auto started = std::chrono::steady_clock::now ();
    std::thread sender ([&] {
        zlink::message_t payload =
          zlink_cpp_contract::make_message ("resumed-by-core");
        try {
            dealer.send ().message (std::move (payload)).submit ();
        }
        catch (const zlink::submit_error_t &error) {
            std::fprintf (stderr, "blocking submit failed result=%d errno=%d\n",
                          static_cast<int> (error.result ()),
                          error.internal_errno ());
            failed.store (true, std::memory_order_release);
        }
        submitted.store (true, std::memory_order_release);
    });

    // The submit must still be parked inside Core while no credit exists.
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    const bool parked = !submitted.load (std::memory_order_acquire);

    bool resumed_payload_seen = false;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline && !resumed_payload_seen) {
        zlink::received_t received;
        if (router.recv (received, zlink::recv_flags_t::dontwait) != 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
            continue;
        }
        resumed_payload_seen =
          received.first_part ().to_string () == "resumed-by-core";
        received.close ();
    }
    sender.join ();
    const auto elapsed = std::chrono::steady_clock::now () - started;

    if (!parked) {
        std::fprintf (stderr, "blocking submit did not wait for credit\n");
        return 2;
    }
    if (failed.load (std::memory_order_acquire)) {
        std::fprintf (stderr, "blocking submit did not resume on Core credit\n");
        return 3;
    }
    if (!resumed_payload_seen) {
        std::fprintf (stderr, "resumed send was never delivered\n");
        return 4;
    }
    if (elapsed >= std::chrono::seconds (10)) {
        std::fprintf (stderr, "blocking submit took too long to resume\n");
        return 5;
    }
    return 0;
}

int test_routed_send_without_credit_reports_backpressure ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("timeout-source"));

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-timeout");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    const std::string filler (65536, 'f');
    if (!fill_until_backpressured (dealer, filler)) {
        std::fprintf (stderr, "could not reach the send HWM\n");
        return 1;
    }

    dealer.options ().send_timeout (std::chrono::milliseconds (100));
    zlink::message_t payload =
      zlink_cpp_contract::make_message ("not-admitted");
    const auto started = std::chrono::steady_clock::now ();
    try {
        dealer.send ().message (payload).submit ();
        std::fprintf (stderr, "expired SNDTIMEO must fail the submit\n");
        return 2;
    }
    catch (const zlink::submit_error_t &error) {
        if (error.result () != zlink::submit_result_t::backpressured) {
            std::fprintf (stderr, "unexpected result=%d errno=%d\n",
                          static_cast<int> (error.result ()),
                          error.internal_errno ());
            return 3;
        }
    }
    if (std::chrono::steady_clock::now () - started >= std::chrono::seconds (2)) {
        std::fprintf (stderr, "SNDTIMEO was not the wait bound\n");
        return 4;
    }
    // The C++ staging policy preserves the public lvalue even though Core
    // consumes the native part used for the failed synchronous attempt.
    if (!payload.valid ()) {
        std::fprintf (stderr, "a refused submit must not consume the part\n");
        return 5;
    }
    return 0;
}

} // namespace

int main ()
{
    if (const int rc = test_blocking_routed_send_resumes_on_core_credit ())
        return rc;
    if (const int rc = test_routed_send_without_credit_reports_backpressure ())
        return 10 + rc;
    return 0;
}
