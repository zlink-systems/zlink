/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

detail::sample_task_t run_sample ()
{
    // --8<-- [start:doc]
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    router.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = router.options ().last_endpoint ();
    assert (!endpoint.empty ());
    dealer.connect (endpoint);
    assert (detail::wait_connected (router_monitor, dealer_monitor, 2000, &router));

    const std::string sent = detail::k_dealer_router_request;
    zlink::message_t outbound = detail::make_message (sent);
    co_await dealer.send ().message (std::move (outbound)).async ();

    zlink::received_t inbound;
    assert (router.recv (inbound) == 0);
    assert (inbound.routing_id ().has_value ());
    assert (inbound.parts ().size () == 1);
    assert (inbound.parts ()[0].to_string () == detail::k_dealer_router_request);

    const std::string reply_payload = detail::k_dealer_router_reply;
    zlink::message_t reply = detail::make_message (reply_payload);
    // Reply on the ROUTER socket, addressed by the received envelope's routing id.
    co_await router.send (*inbound.routing_id ()).message (std::move (reply)).async ();
    inbound.close ();

    zlink::received_t echoed;
    assert (dealer.recv (echoed) == 0);
    assert (echoed.parts ().size () == 1);
    const std::string received = echoed.parts ()[0].to_string ();
    assert (received == detail::k_dealer_router_reply);
    echoed.close ();
    std::printf ("[dealer-router/recv] send: \"%s\" → recv: \"%s\"\n", sent.c_str (),
                 received.c_str ());
    // --8<-- [end:doc]
}

int main ()
{
    run_sample ().get ();
    return 0;
}
