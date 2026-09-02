/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

// The request suspension is completed by Core from its reply handler callback,
// so the coroutine resumes in that context. Keep socket and context lifetime
// with the owner that started the operation: closing them from inside the
// resumed continuation would tear Core down from one of its own callbacks.
detail::sample_task_t run_request (zlink::dealer_socket_t &dealer,
                                   std::string &reply_payload_out)
{
    // --8<-- [start:doc]
    zlink::message_t request = detail::make_message (detail::k_dealer_router_request);
    std::vector<zlink::message_t> reply_parts =
      co_await dealer.request ()
        .message (std::move (request))
        .timeout (std::chrono::milliseconds (2000))
        .async ();

    assert (!reply_parts.empty ());
    reply_payload_out = reply_parts.front ().to_string ();
    for (auto &part : reply_parts)
        part.close ();
    // --8<-- [end:doc]
}

int main ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const zlink::routing_id_t client_id = zlink::routing_id_t::from (std::string ("REQ-CLIENT"));
    dealer.set_routing_id (client_id);
    router.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = router.options ().last_endpoint ();
    dealer.connect (endpoint);
    assert (detail::wait_connected (router_monitor, dealer_monitor, 2000, &router));

    // 응답하는 쪽. 요청을 받아 캡슐화된 reply token으로 되돌려준다.
    std::future<void> responder = std::async (std::launch::async, [&] {
        zlink::received_t inbound;
        assert (router.recv (inbound) == 0);
        assert (inbound.routing_id ().has_value ());
        assert (inbound.reply_token ().has_value ());
        zlink::message_t reply = detail::make_message (detail::k_dealer_router_reply);
        inbound.reply ().message (reply).submit ();
        inbound.close ();
    });

    std::string reply_payload;
    run_request (dealer, reply_payload).get ();
    responder.get ();
    assert (reply_payload == detail::k_dealer_router_reply);

    std::printf ("[dealer-router/request-reply/async] send: \"%s\" -> recv: \"%s\"\n",
                 detail::k_dealer_router_request,
                 reply_payload.c_str ());
    return 0;
}
