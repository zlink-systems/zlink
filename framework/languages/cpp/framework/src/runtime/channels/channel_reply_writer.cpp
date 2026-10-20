/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <utility>

namespace zlink::framework::detail
{

runtime::messaging::envelope_header_t channel_reply_writer_t::create_reply_header (
  runtime::messaging::message_kind_t kind,
  std::string channel_name,
  const runtime::messaging::envelope_header_t &request) const
{
    runtime::messaging::envelope_header_t header;
    header.kind = kind;
    header.channel_name = std::move (channel_name);
    header.message_name = request.message_name;
    header.content_type = request.content_type;
    header.correlation_id = request.correlation_id;
    return header;
}

runtime::messaging::envelope_header_t
channel_reply_writer_t::create_error_header (std::string channel_name,
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
