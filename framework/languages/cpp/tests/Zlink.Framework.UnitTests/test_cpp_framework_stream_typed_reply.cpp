/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/framework.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

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
    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &,
                            const zlink::message_t &) override
    {
        return reply_with_session_info (stream);
    }
};
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
    if (headers.size () != 1 || payloads.size () != 1
        || headers[0].kind () != detail::stream_message_kind_t::response
        || headers[0].request_seq () != 17 || !headers[0].packet_name ().empty ()
        || headers[0].correlation_id () != "typed-reply-request"
        || headers[0].codec () != stream_codec_t::json)
        return 2;
    const auto decoded = payloads[0].parse_json<session_info_t> ();
    if (decoded.session_id != stream.session_id () || decoded.revision != 7)
        return 3;
    std::cout << "Session-created typed JSON reply, correlation and one-shot token: passed\n";
}
