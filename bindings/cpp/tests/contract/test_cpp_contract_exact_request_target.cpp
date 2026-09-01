/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Sockets/socket_access.hpp>
#include <zlink.h>
#include <zlink/message/api.h>

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

// The standard DEALER request submit owns peer selection. Even when the next
// weighted candidate is backpressured, Core may admit the request to another
// eligible peer instead of the binding freezing that candidate into an exact
// transport-pair submit.
int test_dealer_request_selection_is_core_owned ()
{
    two_target_fixture_t fixture ("core-owned-request-target");
    fixture.fill_a_until_backpressured (std::string (16384, 'f'));
    fixture.dealer.options ().send_timeout (std::chrono::milliseconds (200));

    const std::string request_payload = "request-must-use-ready-peer";
    zlink::message_t request =
      zlink_cpp_contract::make_message (request_payload);
    std::vector<zlink::message_t> reply;
    bool delivered_to_b = false;
    std::string failure;
    try {
        request_test_task_t completion = await_request (
          fixture.dealer.request ()
            .message (std::move (request))
            .timeout (std::chrono::seconds (3))
            .async ());

        const auto deadline = std::chrono::steady_clock::now ()
                              + std::chrono::seconds (3);
        while (!delivered_to_b
               && std::chrono::steady_clock::now () < deadline) {
            delivered_to_b = poll_request_and_reply (
              fixture.router_b, request_payload, "reply:B");
            if (!delivered_to_b)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (delivered_to_b)
            reply = completion.get ();
    }
    catch (const zlink::binding_error_t &error) {
        failure = "request failed errno="
                  + std::to_string (error.internal_errno ());
    }

    const std::string reply_payload =
      reply.empty () ? std::string () : reply.front ().to_string ();
    if (!failure.empty () || !delivered_to_b || reply_payload != "reply:B") {
        std::fprintf (
          stderr,
          "Core-owned DEALER selection violation: %s on_B=%d reply=%s\n",
          failure.c_str (), delivered_to_b ? 1 : 0, reply_payload.c_str ());
        return 1;
    }
    return 0;
}

} // namespace

int main ()
{
    return test_dealer_request_selection_is_core_owned ();
}
