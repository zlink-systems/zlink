/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

int main ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t server (ctx);
    zlink::pair_socket_t client (ctx);
    zlink::socket_monitor_t server_monitor =
      server.monitor_open (zlink::monitor_event::connection_ready);
    zlink::socket_monitor_t client_monitor =
      client.monitor_open (zlink::monitor_event::connection_ready);

    assert (!server_monitor.recv (zlink::recv_flags_t::dontwait));
    assert (!client_monitor.recv (zlink::recv_flags_t::dontwait));

    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());
    client.connect (endpoint);

    const std::optional<zlink::monitor_event_t> server_event = server_monitor.recv ();
    const std::optional<zlink::monitor_event_t> client_event = client_monitor.recv ();
    assert (server_event.has_value ());
    assert (client_event.has_value ());
    assert (server_event->event == zlink::monitor_event::connection_ready);
    assert (client_event->event == zlink::monitor_event::connection_ready);
    assert (!server_monitor.recv (zlink::recv_flags_t::dontwait));
    assert (!client_monitor.recv (zlink::recv_flags_t::dontwait));

    std::printf ("[monitor/recv] recv: \"connection-ready\" -> recv(non_blocking): empty\n");
    return 0;
}
