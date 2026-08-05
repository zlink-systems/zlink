/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

namespace
{

struct callback_result_t
{
    std::optional<zlink::routing_id_t> routing_id;
    std::string payload;
};

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

    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());

    std::promise<callback_result_t> result_promise;
    std::future<callback_result_t> result_future = result_promise.get_future ();
    server.set_packet_handler ([&result_promise] (const zlink::routing_id_t &source_rid_,
                                                  zlink::message_t header_,
                                                  zlink::message_t body_) {
        callback_result_t result;
        result.routing_id = source_rid_;
        result.payload = packet_payload (header_, body_);
        result_promise.set_value (result);
    });

    detail::raw_tcp_client_t client (endpoint);
    assert (detail::wait_stream_connected (server_monitor));

    const std::string request = detail::k_stream_payload;
    const std::vector<unsigned char> request_frame = encode_packet_frame (request);
    client.send_all (reinterpret_cast<const char *> (request_frame.data ()), request_frame.size ());

    const callback_result_t result = detail::wait_future (result_future, 2000);
    assert (result.payload == detail::k_stream_payload);

    zlink::message_t reply = detail::make_message (detail::k_stream_payload);
    assert (result.routing_id.has_value ());
    server.send (*result.routing_id).message (reply).submit ();

    char response[64];
    const int received = client.recv_exact (response, request.size ());
    assert (received == static_cast<int> (std::strlen (detail::k_stream_payload)));
    assert (std::memcmp (response, detail::k_stream_payload, received) == 0);
    std::printf ("[stream/packet-callback] send: \"%s\" → recv: \"%.*s\"\n", request.c_str (),
                 received, response);

    client.close ();
    return 0;
    // --8<-- [end:doc]
}
