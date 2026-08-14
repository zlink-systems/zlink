/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <zlink/message/api.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace
{

std::string rid_text (const zlink_routed_submit_target_t &target_)
{
    return std::string (
      reinterpret_cast<const char *> (target_.peer_rid.data),
      target_.peer_rid.size);
}

zlink_submit_result_t send_exact (
  zlink::dealer_socket_t &dealer_,
  const zlink_routed_submit_target_t &target_,
  const std::string &payload_)
{
    zlink_msg_t part;
    assert (zlink_msg_init_size (&part, payload_.size ())
            == ZLINK_CONFIG_OK);
    if (!payload_.empty ())
        std::memcpy (zlink_msg_data (&part), payload_.data (),
                     payload_.size ());
    const zlink_submit_result_t result =
      zlink_dealer_send_transport_pair_part (
        zlink::detail::native_handle (dealer_), &target_, &part,
        ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    assert (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    return result;
}

bool fill_exact_until_backpressured (
  zlink::dealer_socket_t &dealer_,
  const zlink_routed_submit_target_t &target_,
  const std::string &filler_)
{
    bool admitted = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const zlink_submit_result_t result =
          send_exact (dealer_, target_, filler_);
        if (result == ZLINK_SUBMIT_OK) {
            admitted = true;
            continue;
        }
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            return admitted;
        std::fprintf (stderr,
                      "unexpected exact filler result=%d errno=%d\n",
                      static_cast<int> (result), zlink_errno ());
        return false;
    }
    return false;
}

bool recv_payload (zlink::router_socket_t &router_,
                   const std::string &expected_,
                   std::chrono::milliseconds timeout_)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout_;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink::received_t received;
        if (router_.recv (received, zlink::recv_flags_t::dontwait) == 0) {
            const bool matched = received.parts ().size () == 1
                                 && received.first_part ().to_string ()
                                      == expected_;
            received.close ();
            if (matched)
                return true;
        } else {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }
    return false;
}

struct drain_result_t
{
    int filler_count = 0;
    bool pending_seen = false;
};

drain_result_t drain_a_until_quiet (
  zlink::router_socket_t &router_, const std::string &filler_,
  const std::string &pending_)
{
    drain_result_t result;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (2);
    auto quiet_since = std::chrono::steady_clock::now ();
    while (std::chrono::steady_clock::now () < deadline) {
        zlink::received_t received;
        if (router_.recv (received, zlink::recv_flags_t::dontwait) == 0) {
            const std::string payload = received.first_part ().to_string ();
            received.close ();
            if (payload == filler_)
                ++result.filler_count;
            else if (payload == pending_)
                result.pending_seen = true;
            quiet_since = std::chrono::steady_clock::now ();
            continue;
        }
        if (result.filler_count > 0
            && std::chrono::steady_clock::now () - quiet_since
                 >= std::chrono::milliseconds (100))
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return result;
}

int test_dealer_retry_keeps_the_initial_exact_target ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router_a (ctx);
    zlink::router_socket_t router_b (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("retry-source"));
    router_a.set_routing_id (zlink::routing_id_t::from ("A"));
    router_b.set_routing_id (zlink::routing_id_t::from ("B"));

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    dealer.options ().send_timeout (std::chrono::seconds (3));
    router_a.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
    router_b.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint_a =
      zlink_cpp_contract::unique_inproc ("exact-retry-a");
    const std::string endpoint_b =
      zlink_cpp_contract::unique_inproc ("exact-retry-b");
    router_a.bind (endpoint_a);
    router_b.bind (endpoint_b);
    dealer.connect (endpoint_a);
    dealer.connect (endpoint_b);

    zlink_routed_submit_target_t target_a{};
    zlink_routed_submit_target_t target_b{};
    bool have_a = false;
    bool have_b = false;
    bool selector_aligned_after_b = false;
    for (int attempt = 0; attempt < 2000; ++attempt) {
        zlink_routed_submit_target_t selected{};
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (
            zlink::detail::native_handle (dealer), nullptr, &selected);
        if (result == ZLINK_SUBMIT_OK) {
            const std::string rid = rid_text (selected);
            if (rid == "A") {
                target_a = selected;
                have_a = true;
            } else if (rid == "B") {
                target_b = selected;
                have_b = true;
            }
            if (have_a && have_b && rid == "B") {
                selector_aligned_after_b = true;
                break;
            }
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    assert (have_a && have_b && selector_aligned_after_b);

    const std::string filler (16384, 'f');
    assert (fill_exact_until_backpressured (dealer, target_a, filler));

    const std::string pending_payload = "must-stay-on-initial-A";
    zlink::message_t pending =
      zlink_cpp_contract::make_message (pending_payload);
    zlink_cpp_contract::routed_send_test_task_t completion =
      zlink_cpp_contract::await_routed_send (
        dealer.send ().message (std::move (pending)).async ());

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    const drain_result_t drained =
      drain_a_until_quiet (router_a, filler, pending_payload);
    assert (drained.filler_count > 0);
    completion.get ();

    const bool delivered_to_a = drained.pending_seen || recv_payload (
      router_a, pending_payload, std::chrono::milliseconds (500));
    const bool rerouted_to_b = recv_payload (
      router_b, pending_payload, std::chrono::milliseconds (500));
    if (!delivered_to_a || rerouted_to_b) {
        std::fprintf (
          stderr,
          "exact-target retry violation: delivered_to_A=%d rerouted_to_B=%d\n",
          delivered_to_a ? 1 : 0, rerouted_to_b ? 1 : 0);
        return 1;
    }

    bool detach_selector_aligned_after_b = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        zlink_routed_submit_target_t selected{};
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (
            zlink::detail::native_handle (dealer), nullptr, &selected);
        if (result == ZLINK_SUBMIT_OK && rid_text (selected) == "B") {
            detach_selector_aligned_after_b = true;
            break;
        }
    }
    assert (detach_selector_aligned_after_b);
    assert (fill_exact_until_backpressured (dealer, target_a, filler));

    const std::string detached_payload = "detached-A-must-not-reroute";
    zlink::message_t detached =
      zlink_cpp_contract::make_message (detached_payload);
    zlink_cpp_contract::routed_send_test_task_t detached_completion =
      zlink_cpp_contract::await_routed_send (
        dealer.send ().message (std::move (detached)).async ());
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    router_a.close ();

    bool detached_terminal = false;
    try {
        detached_completion.get ();
    }
    catch (const zlink::submit_error_t &error) {
        detached_terminal = true;
        if (error.result () != zlink::submit_result_t::not_connected) {
            std::fprintf (
              stderr, "detached exact target result=%d errno=%d\n",
              static_cast<int> (error.result ()), error.internal_errno ());
            return 3;
        }
    }
    if (!detached_terminal
        || recv_payload (router_b, detached_payload,
                         std::chrono::milliseconds (500))) {
        std::fprintf (
          stderr,
          "detached exact target did not terminate without reroute\n");
        return 4;
    }
    return 0;
}

} // namespace

int main ()
{
    return test_dealer_retry_keeps_the_initial_exact_target ();
}
