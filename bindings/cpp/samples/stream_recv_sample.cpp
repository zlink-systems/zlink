/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

int main ()
{
    // --8<-- [start:doc]
    zlink::context_t ctx;
    zlink::stream_socket_t server (ctx);
    zlink::socket_monitor_t server_monitor = server.monitor_open ();
    server.options ().notify (false);

    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());

    detail::raw_tcp_client_t client (endpoint);
    assert (detail::wait_stream_connected (server_monitor));

    const char *request = detail::k_stream_payload;
    const size_t request_size = std::strlen (request);
    client.send_all (request, request_size);

    zlink::received_t inbound;
    assert (server.recv (inbound) == 0);
    assert (inbound.routing_id ().has_value ());
    assert (inbound.parts ().size () == 1);
    assert (inbound.parts ()[0].to_string () == detail::k_stream_payload);

    zlink::message_t reply = detail::make_message (detail::k_stream_payload);
    // Reply on the STREAM socket, addressed by the received envelope's routing id.
    server.send (*inbound.routing_id ()).message (reply).submit ();
    inbound.close ();

    char response[64];
    const int received = client.recv_exact (response, request_size);
    assert (received == static_cast<int> (std::strlen (detail::k_stream_payload)));
    assert (std::memcmp (response, detail::k_stream_payload, received) == 0);
    std::printf ("[stream/recv] send: \"%s\" → recv: \"%.*s\"\n", request, received, response);

    client.close ();
    return 0;
    // --8<-- [end:doc]
}
