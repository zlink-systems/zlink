/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_reply_writer.hpp"

#include <utility>

namespace zlink::framework::detail
{

namespace
{

std::string error_code_name (framework_error_kind_t kind)
{
    switch (kind) {
        case framework_error_kind_t::not_found:
            return "not_found";
        case framework_error_kind_t::already_exists:
            return "already_exists";
        case framework_error_kind_t::type_mismatch:
            return "type_mismatch";
        case framework_error_kind_t::not_configured:
            return "not_configured";
        case framework_error_kind_t::rejected:
            return "rejected";
        case framework_error_kind_t::unavailable:
            return "unavailable";
        case framework_error_kind_t::capacity_exceeded:
            return "capacity_exceeded";
        case framework_error_kind_t::deadline_exceeded:
            return "deadline_exceeded";
        case framework_error_kind_t::shutting_down:
            return "shutting_down";
        case framework_error_kind_t::protocol_error:
            return "protocol_error";
        case framework_error_kind_t::invalid_operation:
            return "invalid_operation";
        case framework_error_kind_t::data_lost:
            return "data_lost";
        case framework_error_kind_t::internal_failure:
            return "internal_failure";
    }
    return "internal_failure";
}

} // namespace

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
    header.error_code = error_code_name (error.kind ());
    header.error_message = error.what ();
    return header;
}

runtime::messaging::message_parts_t
channel_reply_writer_t::reply_raw_envelope (const runtime::messaging::envelope_header_t &header,
                                            zlink::message_t body) const
{
    return runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, std::move (body));
}

} // namespace zlink::framework::detail
