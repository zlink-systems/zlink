/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

namespace
{

std::string packet_payload (const zlink::message_t &header_, const zlink::message_t &body_)
{
    assert (header_.size () == 0);
    return body_.to_string ();
}

std::vector<unsigned char> encode_packet_frame (const std::string &payload_)
{
    std::vector<unsigned char> frame (6u + payload_.size (), 0u);
    const uint32_t body_size = static_cast<uint32_t> (payload_.size ());
    frame[2] = static_cast<unsigned char> ((body_size >> 24) & 0xFFu);
    frame[3] = static_cast<unsigned char> ((body_size >> 16) & 0xFFu);
    frame[4] = static_cast<unsigned char> ((body_size >> 8) & 0xFFu);
    frame[5] = static_cast<unsigned char> (body_size & 0xFFu);
    if (!payload_.empty ()) {
        std::memcpy (frame.data () + 6u, payload_.data (), payload_.size ());
    }
    return frame;
}

} // namespace

int main ()
{
    // --8<-- [start:doc]
    zlink::context_t ctx;
    zlink::stream_socket_t server (ctx);
    zlink::socket_monitor_t server_monitor = server.monitor_open ();
    server.options ().notify (false);
    server.options ().recv_mode (zlink::stream_recv_mode_t::packet);

    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());

    detail::raw_tcp_client_t client (endpoint);
    assert (detail::wait_stream_connected (server_monitor));

    const std::string request = detail::k_stream_payload;
    const std::vector<unsigned char> request_frame = encode_packet_frame (request);
    client.send_all (reinterpret_cast<const char *> (request_frame.data ()), request_frame.size ());

    zlink::stream_packet_t packet;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!server.recv_packet (packet, zlink::recv_flags_t::dontwait)) {
        assert (std::chrono::steady_clock::now () < deadline);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    assert (packet_payload (packet.header (), packet.body ()) == detail::k_stream_payload);

    zlink::message_t reply = detail::make_message (detail::k_stream_payload);
    assert (packet.routing_id ().has_value ());
    server.send (*packet.routing_id ()).message (reply).submit ();

    char response[64];
    const int received = client.recv_exact (response, request.size ());
    assert (received == static_cast<int> (std::strlen (detail::k_stream_payload)));
    assert (std::memcmp (response, detail::k_stream_payload, received) == 0);
    std::printf ("[stream/packet-pull] send: \"%s\" → recv: \"%.*s\"\n", request.c_str (),
                 received, response);

    client.close ();
    return 0;
    // --8<-- [end:doc]
}
