/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Sockets/socket_access.hpp>
#include <zlink.h>
#include <zlink/message/api.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

class request_test_task_t
{
  public:
    struct promise_type
    {
        std::promise<std::vector<zlink::message_t>> completion;

        request_test_task_t get_return_object ()
        {
            return request_test_task_t (completion.get_future ());
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_value (std::vector<zlink::message_t> value_)
        {
            completion.set_value (std::move (value_));
        }
        void unhandled_exception ()
        {
            completion.set_exception (std::current_exception ());
        }
    };

    explicit request_test_task_t (
      std::future<std::vector<zlink::message_t>> completion_) :
        _completion (std::move (completion_))
    {
    }

    std::vector<zlink::message_t> get ()
    {
        assert (_completion.wait_for (std::chrono::seconds (5))
                == std::future_status::ready);
        return _completion.get ();
    }

  private:
    std::future<std::vector<zlink::message_t>> _completion;
};

request_test_task_t await_request (
  zlink::async_result_t<std::vector<zlink::message_t>> result_)
{
    co_return co_await std::move (result_);
}

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

struct two_target_fixture_t
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer{context};
    zlink::router_socket_t router_a{context};
    zlink::router_socket_t router_b{context};
    std::string endpoint_a;
    std::string endpoint_b;
    zlink_routed_submit_target_t target_a{};

    explicit two_target_fixture_t (const char *name_)
    {
        dealer.set_routing_id (
          zlink::routing_id_t::from (std::string (name_) + "-source"));
        router_a.set_routing_id (zlink::routing_id_t::from ("A"));
        router_b.set_routing_id (zlink::routing_id_t::from ("B"));

        const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
        dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
        router_a.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
        router_b.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

        endpoint_a = zlink_cpp_contract::unique_inproc (name_);
        endpoint_b = zlink_cpp_contract::unique_inproc (name_);
        router_a.bind (endpoint_a);
        router_b.bind (endpoint_b);
        dealer.connect (endpoint_a);
        dealer.connect (endpoint_b);

        bool have_a = false;
        bool have_b = false;
        bool aligned_after_b = false;
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
                    have_b = true;
                }
                if (have_a && have_b && rid == "B") {
                    aligned_after_b = true;
                    break;
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        assert (have_a && have_b && aligned_after_b);
    }

    void fill_a_until_backpressured (const std::string &filler_)
    {
        bool admitted = false;
        bool backpressured = false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const zlink_submit_result_t result =
              send_exact (dealer, target_a, filler_);
            if (result == ZLINK_SUBMIT_OK) {
                admitted = true;
                continue;
            }
            if (result == ZLINK_SUBMIT_BACKPRESSURED) {
                backpressured = true;
                break;
            }
            std::fprintf (stderr,
                          "unexpected exact filler result=%d errno=%d\n",
                          static_cast<int> (result), zlink_errno ());
            break;
        }
        assert (admitted && backpressured);
    }
};

bool poll_request_and_reply (zlink::router_socket_t &router_,
                             const std::string &request_payload_,
                             const std::string &reply_payload_)
{
    zlink::received_t received;
    if (router_.recv (received, zlink::recv_flags_t::dontwait) != 0)
        return false;
    if (received.first_part ().to_string () != request_payload_) {
        received.close ();
        return false;
    }
    assert (received.request_seq ().has_value ());
    zlink::message_t reply =
      zlink_cpp_contract::make_message (reply_payload_);
    received.reply ().message (reply).submit ();
    return true;
}

bool wait_for_request_on_either (two_target_fixture_t &fixture_,
                                 const std::string &request_payload_,
                                 bool &on_a_, bool &on_b_)
{
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        if (poll_request_and_reply (fixture_.router_a, request_payload_,
                                    "reply:A")) {
            on_a_ = true;
            return true;
        }
        if (poll_request_and_reply (fixture_.router_b, request_payload_,
                                    "reply:B")) {
            on_b_ = true;
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

// The request terminal picks one exact target and submits to it synchronously.
// The binding no longer parks or retries the submit, so what has to hold is
// that the send waits inside Core for that exact target's credit and never
// gets rerouted to the other pipe.
int test_request_stays_on_the_initially_selected_exact_target ()
{
    two_target_fixture_t fixture ("exact-request-credit");
    const std::string filler (16384, 'f');
    fixture.fill_a_until_backpressured (filler);
    fixture.dealer.options ().send_timeout (std::chrono::seconds (5));

    // Only Core can release the parked submit; drain A from another thread.
    std::atomic<bool> stop_drain{false};
    std::thread drain_a ([&] {
        while (!stop_drain.load (std::memory_order_acquire)) {
            zlink::received_t received;
            if (fixture.router_a.recv (received, zlink::recv_flags_t::dontwait)
                != 0) {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                continue;
            }
            if (received.first_part ().to_string () == filler) {
                received.close ();
                continue;
            }
            // The request itself: reply on A so the suspension can complete.
            if (received.request_seq ().has_value ()) {
                zlink::message_t reply =
                  zlink_cpp_contract::make_message ("reply:A");
                received.reply ().message (reply).submit ();
                continue;
            }
            received.close ();
        }
    });

    const std::string request_payload = "request-must-stay-on-A";
    zlink::message_t request =
      zlink_cpp_contract::make_message (request_payload);
    request_test_task_t completion = await_request (
      fixture.dealer.request ()
        .message (std::move (request))
        .timeout (std::chrono::seconds (5))
        .async ());

    std::vector<zlink::message_t> reply;
    std::string failure;
    try {
        reply = completion.get ();
    }
    catch (const zlink::binding_error_t &error) {
        failure = "request failed errno="
                  + std::to_string (error.internal_errno ());
    }
    stop_drain.store (true, std::memory_order_release);
    drain_a.join ();

    bool rerouted_to_b = false;
    zlink::received_t stray;
    if (fixture.router_b.recv (stray, zlink::recv_flags_t::dontwait) == 0) {
        rerouted_to_b = stray.first_part ().to_string () == request_payload;
        stray.close ();
    }

    const std::string reply_payload =
      reply.empty () ? std::string () : reply.front ().to_string ();
    if (!failure.empty () || rerouted_to_b || reply_payload != "reply:A") {
        std::fprintf (
          stderr,
          "exact request violation: %s on_B=%d reply=%s\n",
          failure.c_str (), rerouted_to_b ? 1 : 0, reply_payload.c_str ());
        return 1;
    }
    return 0;
}

// A target that never regains credit fails the submit on the caller thread and
// is never rerouted to the other pipe.
int test_detached_exact_request_is_terminal_without_reroute ()
{
    two_target_fixture_t fixture ("exact-request-detach");
    const std::string filler (16384, 'd');
    fixture.fill_a_until_backpressured (filler);
    fixture.dealer.options ().send_timeout (std::chrono::milliseconds (200));

    const std::string request_payload = "detached-request-must-not-reroute";
    zlink::message_t request =
      zlink_cpp_contract::make_message (request_payload);
    bool terminal = false;
    zlink::submit_result_t terminal_result = zlink::submit_result_t::ok;
    try {
        (void) fixture.dealer.request ()
          .message (request)
          .timeout (std::chrono::seconds (3))
          .async ();
    }
    catch (const zlink::submit_error_t &error) {
        terminal = true;
        terminal_result = error.result ();
    }

    bool rerouted_to_b = false;
    const auto b_deadline = std::chrono::steady_clock::now ()
                            + std::chrono::milliseconds (500);
    while (std::chrono::steady_clock::now () < b_deadline
           && !rerouted_to_b) {
        rerouted_to_b = poll_request_and_reply (
          fixture.router_b, request_payload, "reply:B");
        if (!rerouted_to_b)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    if (!terminal || rerouted_to_b) {
        std::fprintf (
          stderr,
          "detached exact request did not terminate without reroute: "
          "terminal=%d result=%d on_B=%d\n",
          terminal ? 1 : 0, static_cast<int> (terminal_result),
          rerouted_to_b ? 1 : 0);
        return 1;
    }
    // The C++ staging policy leaves the public request lvalue with the caller
    // after Core consumes the failed synchronous attempt's native part.
    if (!request.valid ()) {
        std::fprintf (stderr, "refused request submit consumed the part\n");
        return 2;
    }
    return 0;
}

} // namespace

int main ()
{
    const int retry_result =
      test_request_stays_on_the_initially_selected_exact_target ();
    if (retry_result != 0)
        return retry_result;
    return test_detached_exact_request_is_terminal_without_reroute ();
}
