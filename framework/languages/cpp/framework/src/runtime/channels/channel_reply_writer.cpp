/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/admission_queue_messages.hpp"

#include <utility>

namespace zlink::framework::detail
{

runtime::messaging::envelope_header_t channel_reply_writer_t::create_reply_header (
  runtime::messaging::message_kind_t kind,
  std::string_view channel_name,
  const runtime::messaging::envelope_header_t &request) const
{
    auto name = runtime::messaging::header_text_t::view (request.message_name);
    if (kind == runtime::messaging::message_kind_t::response) {
        if (auto queue_reply = runtime::admission_queue::reply_packet_name (request.message_name))
            name = std::move (*queue_reply);
    }
    return {.kind = kind,
            .channel_name = runtime::messaging::header_text_t::view (channel_name),
            .message_name = std::move (name),
            .content_type = runtime::messaging::header_text_t::view (request.content_type),
            .correlation_id = runtime::messaging::header_text_t::view (request.correlation_id)};
}

runtime::messaging::envelope_header_t
channel_reply_writer_t::create_error_header (std::string_view channel_name,
                                             const runtime::messaging::envelope_header_t &request,
                                             const framework_exception_t &error) const
{
    auto header = create_reply_header (runtime::messaging::message_kind_t::error,
                                       std::move (channel_name), request);
    header.error_code = stream_error_code (error.kind ());
    header.error_message = error.what ();
    runtime::messaging::write_failure_origin (header, error);
    /* zlink.origin=framework marker for framework-generated errors only
     * (stale-route contract); application handler errors stay unmarked. */
    runtime::messaging::write_error_origin (header, error);
    return header;
}

runtime::messaging::message_parts_t
channel_reply_writer_t::reply_raw_envelope (const runtime::messaging::envelope_header_t &header,
                                            zlink::message_t body) const
{
    return runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, std::move (body));
}

} // namespace zlink::framework::detail
