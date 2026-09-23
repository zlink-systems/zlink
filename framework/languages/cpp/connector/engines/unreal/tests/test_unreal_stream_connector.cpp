/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "ZLinkStreamConnector.h"

#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/stream_connector.hpp>

#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

zlink::message_t frame (zlink::stream_connector::message_kind_t kind,
                        std::uint64_t sequence,
                        std::string name,
                        std::string payload)
{
    using namespace zlink::stream_connector;
    detail::stream_header_t header;
    header.kind = kind;
    header.codec = codec_t::json;
    header.flags = header_flags_t::none;
    header.name = std::move (name);
    if (kind == message_kind_t::response) {
        header.request_seq = sequence;
    }
    auto encoded_header = detail::header_codec_t{}.encode (header);
    std::vector<std::uint8_t> bytes (payload.begin (), payload.end ());
    auto encoded =
      detail::frame_codec_t::encode (encoded_header.value (), bytes, connector_options_t{});
    return zlink::message_t::from (
      std::string (encoded.value ().begin (), encoded.value ().end ()));
}

} // namespace

int main ()
{
    zlink::context_t context;
    zlink::stream_socket_t server (context);
    server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    server.options ().notify (false);
    server.bind ("tcp://127.0.0.1:0");

    std::thread sender ([&server] {
        zlink::received_t incoming;
        if (server.recv (incoming) != 0 || incoming.parts ().empty ()) {
            return;
        }
        const auto wire = incoming.parts ()[0].to_string ();
        if (wire.size () < 6) {
            return;
        }
        const auto header_size =
          (static_cast<std::uint8_t> (wire[0]) << 8) | static_cast<std::uint8_t> (wire[1]);
        std::vector<std::uint8_t> header_bytes (wire.begin () + 6, wire.begin () + 6 + header_size);
        auto request = zlink::stream_connector::detail::header_codec_t{}.decode (header_bytes);
        if (!request || !request.value ().request_seq) {
            return;
        }
        incoming.send ()
          .message (frame (zlink::stream_connector::message_kind_t::send, 0, "chat.notify", "{}"))
          .submit ();
        incoming.send ()
          .message (frame (zlink::stream_connector::message_kind_t::send, 0, "other.notify", "{}"))
          .submit ();
        incoming.send ()
          .message (frame (zlink::stream_connector::message_kind_t::response,
                           *request.value ().request_seq, "", "{}"))
          .submit ();
        zlink::received_t ignored;
        server.recv (ignored);
        incoming.close ();
    });

    UZLinkStreamConnector connector;
    std::vector<std::string> pushed_names;
    std::vector<std::string> other_names;
    std::vector<FZLinkStreamRequestResult> replies;
    std::vector<FZLinkStreamRequestResult> failures;
    FZLinkStreamSubscriptionHandle push_handle;
    push_handle = connector.On ("chat.notify", [&] (const FZLinkStreamPacket &packet) {
        pushed_names.push_back (packet.PacketName);
        connector.Unsubscribe (push_handle);
    });
    auto other_handle = connector.On ("other.notify", [&] (const FZLinkStreamPacket &packet) {
        other_names.push_back (packet.PacketName);
    });
    connector.Connect (server.options ().last_endpoint ());
    if (!connector.IsConnected ()) {
        sender.join ();
        std::cerr << "connect failed\n";
        return 1;
    }
    connector.RequestJson (
      "chat.request", "{}", 2.0f,
      [&] (const FZLinkStreamRequestResult &result) { replies.push_back (result); });
    connector.RequestJson (
      "chat.failure", "{}", 0.01f,
      [&] (const FZLinkStreamRequestResult &result) { failures.push_back (result); });
    sender.join ();
    connector.Unsubscribe (other_handle);
    if (!pushed_names.empty () || !replies.empty () || !failures.empty ()) {
        std::cerr << "callback ran before dispatch\n";
        return 2;
    }
    for (int i = 0; i < 300 && (pushed_names.empty () || replies.empty () || failures.empty ());
         ++i) {
        connector.Dispatch ();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    connector.Close ();
    if (pushed_names != std::vector<std::string>{"chat.notify"} || !other_names.empty ()) {
        std::cerr << "named push routing failed\n";
        return 3;
    }
    if (replies.size () != 1 || !replies.front ().bSuccess
        || replies.front ().Packet.Payload != std::vector<std::uint8_t>{'{', '}'}) {
        std::cerr << "request reply callback failed: count=" << replies.size ();
        if (!replies.empty ()) {
            std::cerr << " success=" << replies.front ().bSuccess
                      << " name=" << replies.front ().Packet.PacketName
                      << " payload-size=" << replies.front ().Packet.Payload.size ()
                      << " error=" << replies.front ().ErrorMessage;
        }
        std::cerr << '\n';
        return 4;
    }
    if (failures.size () != 1 || failures.front ().bSuccess
        || failures.front ().ErrorMessage.empty ()) {
        std::cerr << "request failure callback failed: count=" << failures.size ();
        if (!failures.empty ()) {
            std::cerr << " success=" << failures.front ().bSuccess
                      << " code=" << failures.front ().ErrorCode
                      << " error=" << failures.front ().ErrorMessage;
        }
        std::cerr << '\n';
        return 5;
    }
    return 0;
}
