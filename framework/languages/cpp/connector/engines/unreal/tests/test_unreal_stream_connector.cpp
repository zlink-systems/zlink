/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "ZLinkStreamConnector.h"

#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/stream_connector.hpp>

#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

    std::atomic_bool hook_metadata_seen{false};
    std::thread sender ([&server, &hook_metadata_seen] {
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
        hook_metadata_seen.store (request.value ().metadata.values.contains ("unreal-hook")
                                  && request.value ().metadata.values.at ("unreal-hook") == "yes");
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
    std::vector<std::string> sending_order;
    std::vector<std::string> reply_names;
    auto removed = connector.OnRequestSending (
      [&] (FZLinkStreamRequestSendingContext &) { sending_order.push_back ("removed"); });
    connector.Unsubscribe (removed);
    auto throwing_sending = connector.OnRequestSending (
      [] (FZLinkStreamRequestSendingContext &) { throw std::runtime_error ("sending hook"); });
    auto sending = connector.OnRequestSending ([&] (FZLinkStreamRequestSendingContext &hook) {
        sending_order.push_back ("first");
        if (hook.RequestPacketName == "chat.request") {
            hook.SetMetadata ("unreal-hook", "yes");
        }
    });
    FZLinkStreamRequestSendingDelegate sending_delegate;
    sending_delegate.BindLambda (
      [&] (FZLinkStreamRequestSendingContext &) { sending_order.push_back ("second"); });
    auto sending_second = connector.OnRequestSending (sending_delegate);
    auto removed_after_connect = connector.OnRequestSending (
      [&] (FZLinkStreamRequestSendingContext &) { sending_order.push_back ("removed later"); });
    FZLinkStreamReplyReceivedDelegate reply_delegate;
    auto throwing_reply = connector.OnReplyReceived (
      [] (const FZLinkStreamReplyReceivedContext &) { throw std::runtime_error ("reply hook"); });
    reply_delegate.BindLambda ([&] (const FZLinkStreamReplyReceivedContext &hook) {
        if (hook.bSucceeded && hook.bHasReply && !hook.bHasError && hook.ElapsedMilliseconds >= 0) {
            reply_names.push_back (hook.RequestPacketName);
        }
    });
    auto reply = connector.OnReplyReceived (reply_delegate);
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
    connector.Unsubscribe (removed_after_connect);
    connector.RequestJson (
      "chat.request", "{}", 2.0f,
      [&] (const FZLinkStreamRequestResult &result) { replies.push_back (result); });
    if (sending_order != std::vector<std::string>{"first", "second"}) {
        sender.join ();
        std::cerr << "sending hook did not run synchronously in registration order\n";
        return 5;
    }
    connector.RequestJson (
      "chat.failure", "{}", 0.01f,
      [&] (const FZLinkStreamRequestResult &result) { failures.push_back (result); });
    sender.join ();
    if (!pushed_names.empty () || !replies.empty () || !failures.empty ()
        || !reply_names.empty ()) {
        std::cerr << "callback ran before dispatch\n";
        return 2;
    }
    connector.Unsubscribe (other_handle);
    for (int i = 0; i < 300
                    && (pushed_names.empty () || replies.empty () || failures.empty ()
                        || reply_names.empty ());
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
    if (!hook_metadata_seen) {
        std::cerr << "request hook metadata missing from server frame\n";
        return 6;
    }
    if (reply_names != std::vector<std::string>{"chat.request"}) {
        std::cerr << "queued reply hook missing\n";
        return 7;
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
        return 11;
    }

    zlink::stream_socket_t pending_server (context);
    pending_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    pending_server.options ().notify (false);
    pending_server.bind ("tcp://127.0.0.1:0");
    std::atomic_bool pending_seen{false};
    std::atomic_bool release_pending{false};
    std::thread pending_sender ([&] {
        zlink::received_t pending;
        if (pending_server.recv (pending) != 0 || pending.parts ().empty ()) {
            return;
        }
        pending_seen.store (true);
        while (!release_pending.load ()) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        pending.close ();
    });
    UZLinkStreamConnector reconnecting;
    int close_replies = 0;
    int close_completions = 0;
    auto close_hook =
      reconnecting.OnReplyReceived ([&] (const FZLinkStreamReplyReceivedContext &hook) {
          if (hook.RequestPacketName == "chat.pending" && !hook.bSucceeded && hook.bHasError
              && hook.ErrorCode == EZLinkStreamErrorCode::Disconnected) {
              ++close_replies;
          }
      });
    reconnecting.Connect (pending_server.options ().last_endpoint ());
    reconnecting.RequestJson (
      "chat.pending", "{}", 2.0f, [&] (const FZLinkStreamRequestResult &result) {
          if (!result.bSuccess
              && result.ErrorCode == static_cast<int> (EZLinkStreamErrorCode::Disconnected)) {
              ++close_completions;
          }
      });
    for (int i = 0; i < 200 && !pending_seen.load (); ++i) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (!pending_seen.load ()) {
        pending_server.close ();
        pending_sender.join ();
        std::cerr << "pending request did not reach server\n";
        return 8;
    }
    reconnecting.Connect ("tcp://127.0.0.1:1");
    release_pending.store (true);
    pending_sender.join ();
    if (close_replies != 0 || close_completions != 0) {
        std::cerr << "close callbacks ran before dispatch\n";
        return 9;
    }
    reconnecting.Dispatch ();
    if (close_replies != 1 || close_completions != 1) {
        std::cerr << "pending request callbacks lost on reconnect: hooks=" << close_replies
                  << " completions=" << close_completions << '\n';
        return 10;
    }
    return 0;
}
