/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"

#include <zlink/framework.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace zlink::framework;

struct session_info_t
{
    std::string session_id;
    int revision;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (session_info_t, session_id, revision)
};

template <typename TStream> task_t<void> reply_with_session_info (TStream &stream)
{
    if constexpr (requires { stream.reply_packet (message_t::from (session_info_t{})); }) {
        auto pushed = message_t::from (session_info_t{stream.session_id (), 6});
        co_await stream.write_packet (pushed).async ();
        auto reply = message_t::from (session_info_t{stream.session_id (), 7});
        co_await stream.reply_packet (reply).async ();
        try {
            co_await stream.reply_packet (reply).async ();
        }
        catch (const framework_exception_t &error) {
            if (error.kind () == framework_error_kind_t::protocol_error)
                co_return;
            throw;
        }
        throw std::runtime_error ("second typed reply consumed the same request token");
    } else {
        std::cerr << "public reply_packet does not accept framework message_t\n";
        throw std::runtime_error ("public reply_packet does not accept framework message_t");
    }
}

class session_t final : public packet_stream_session_t
{
  public:
    task_t<void> on_connected (stream_t &) override { co_return; }
    task_t<void> on_disconnected (stream_t &) override { co_return; }
    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
    task_t<void>
    on_packet (stream_t &stream, const session_message_context_t &, const message_t &) override
    {
        return reply_with_session_info (stream);
    }
};
class encoded_session_t final : public packet_stream_session_t
{
  public:
    explicit encoded_session_t (session_actor_t actor) : _actor (std::move (actor)) {}
    task_t<void> on_connected (stream_t &) override { co_return; }
    task_t<void> on_disconnected (stream_t &) override { co_return; }
    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &,
                            const message_t &payload) override
    {
        if (!payload.encoded () || payload.decode<session_info_t> ().revision != 8)
            throw std::runtime_error ("STREAM input must expose an encoded framework message");
        co_await _actor.relay ("EncodedSend", payload);
        const auto reply = co_await _actor.relay_request (payload).async ();
        if (!reply.encoded ())
            throw std::runtime_error ("relay reply must expose an encoded framework message");
        co_await stream.write_packet (reply).async ();
        co_await stream.reply_packet (reply).async ();
    }

  private:
    session_actor_t _actor;
};

static_assert (std::is_same_v<decltype (std::declval<session_actor_t &> ().relay (
                                std::declval<const message_t &> ())),
                              task_t<void>>);
static_assert (std::is_same_v<decltype (std::declval<session_actor_t &> ()
                                          .relay_request (std::declval<const message_t &> ())
                                          .async ()),
                              request_task_t<message_t>>);
} // namespace

int main ()
{
    detail::configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
    } guard;
    auto builder = test::runtime_failure_builder ();
    builder.stream ("typed-reply").bind ("tcp://127.0.0.1:0").register_session ("session");
    serializer_registry_t serializers;
    detail::bind_stream_serializers (builder, serializers);
    auto runtime = detail::stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("typed-reply");
    session_t session;
    detail::stream_header_t request (detail::stream_message_kind_t::request, stream_codec_t::json,
                                     detail::stream_header_flags_t::has_request_seq, 17,
                                     "SessionInfoRequest", {});
    request.with_correlation_id ("typed-reply-request");
    auto result = runtime.dispatch_packet (session, stream, request,
                                           zlink::message_t::from (std::string ("{}")));
    if (!result) {
        std::cerr << "Session typed reply dispatch failed\n";
        return 1;
    }
    const auto headers = runtime.written_headers (stream);
    const auto payloads = runtime.written_payloads (stream);
    if (headers.size () != 2 || payloads.size () != 2
        || headers[0].kind () != detail::stream_message_kind_t::send
        || headers[0].codec () != stream_codec_t::json
        || headers[1].kind () != detail::stream_message_kind_t::response
        || headers[1].request_seq () != 17 || !headers[1].packet_name ().empty ()
        || headers[1].correlation_id () != "typed-reply-request"
        || headers[1].codec () != stream_codec_t::json)
        return 2;
    const auto pushed = payloads[0].parse_json<session_info_t> ();
    const auto decoded = payloads[1].parse_json<session_info_t> ();
    if (pushed.session_id != stream.session_id () || pushed.revision != 6
        || decoded.session_id != stream.session_id () || decoded.revision != 7)
        return 3;
    auto encoded_stream = runtime.open_session ("typed-reply");
    detail::actor_gateway_runtime_t gateway;
    gateway.bind_serializers (serializers);
    auto actors = gateway.manager ();
    detail::session_actor_manager_access_t::attach (actors, encoded_stream);
    auto actor = actors
                   .bind (detail::actor_ref_access_t::make (
                     node_rid_t::from_string ("encoded-owner"), "encoded-actor", "encoded-id", 1))
                   .async ()
                   .result ()
                   .value ();
    std::vector<std::string> relayed_payloads;
    gateway.on_relay (
      [&] (const actor_ref_t &, actor_context_t, const detail::stream_header_t &,
           const zlink::message_t &payload, std::optional<detail::bound_session_relay_source_t>,
           std::chrono::milliseconds) -> request_task_t<std::optional<zlink::message_t>> {
          relayed_payloads.push_back (payload.to_string ());
          co_await detail::request_admitted_t{};
          co_return std::optional<zlink::message_t> (payload);
      });
    const auto typed_relay =
      actor.relay_request ("TypedRelay", message_t::from (session_info_t{"typed-relay", 9}))
        .async ()
        .result ();
    if (!typed_relay || !typed_relay.value ().encoded ()
        || typed_relay.value ().decode<session_info_t> ().revision != 9) {
        std::cerr << "typed relay failed: "
                  << (typed_relay.error () ? typed_relay.error ()->what ()
                                           : "invalid encoded reply")
                  << '\n';
        return 4;
    }
    encoded_session_t encoded_session (actor);
    const std::string original = " { \"revision\" : 8, \"session_id\" : \"encoded\" } ";
    const auto writes_before_dispatch = runtime.written_payloads (encoded_stream).size ();
    auto encoded_result = runtime.dispatch_packet (encoded_session, encoded_stream, request,
                                                   zlink::message_t::from (original));
    if (!encoded_result) {
        std::cerr << "encoded dispatch failed: " << encoded_result.error ()->what () << '\n';
        return 5;
    }
    const auto encoded_payloads = runtime.written_payloads (encoded_stream);
    const auto encoded_headers = runtime.written_headers (encoded_stream);
    if (encoded_payloads.size () != writes_before_dispatch + 2
        || encoded_headers[writes_before_dispatch].kind () != detail::stream_message_kind_t::send
        || encoded_headers[writes_before_dispatch + 1].kind ()
             != detail::stream_message_kind_t::response
        || encoded_payloads[writes_before_dispatch].to_string () != original
        || encoded_payloads[writes_before_dispatch + 1].to_string () != original
        || relayed_payloads.size () != 3 || relayed_payloads[1] != original
        || relayed_payloads[2] != original) {
        std::cerr << "encoded byte preservation failed: writes=" << encoded_payloads.size ()
                  << " relays=" << relayed_payloads.size () << '\n';
        return 6;
    }
    std::cout << "Session typed encode and encoded relay/send/reply byte preservation: passed\n";
}
