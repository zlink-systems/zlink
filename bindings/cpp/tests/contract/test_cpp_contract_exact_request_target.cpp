/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cassert>
#include <chrono>
#include <thread>

int main ()
{
    zlink::context_t context;
    zlink::router_socket_t requester (context);
    zlink::router_socket_t responder (context);
    requester.set_routing_id (zlink::routing_id_t::from ("requester"));
    responder.set_routing_id (zlink::routing_id_t::from ("responder"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("logical-request-target");
    responder.bind (endpoint);
    requester.connect (endpoint);

    std::thread server ([&] {
        zlink::received_t received;
        while (responder.recv (received, zlink::recv_flags_t::dontwait) != 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        assert (received.first_part ().to_string () == "request");
        assert (received.reply_token ().has_value ());
        zlink::message_t reply = zlink_cpp_contract::make_message ("reply");
        received.reply ().message (reply).submit ();
    });

    zlink::message_t request = zlink_cpp_contract::make_message ("request");
    auto reply = requester.request (zlink::routing_id_t::from ("responder"))
                   .message (request).timeout (std::chrono::seconds (2)).submit ();
    server.join ();
    assert (reply.size () == 1 && reply[0].to_string () == "reply");
    return 0;
}
