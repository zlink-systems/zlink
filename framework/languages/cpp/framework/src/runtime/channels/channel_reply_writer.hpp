/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <string>

namespace zlink::framework::detail
{

class channel_reply_writer_t
{
  public:
    runtime::messaging::envelope_header_t
    create_reply_header (runtime::messaging::message_kind_t kind,
                         std::string channel_name,
                         const runtime::messaging::envelope_header_t &request) const;

    runtime::messaging::envelope_header_t
    create_error_header (std::string channel_name,
                         const runtime::messaging::envelope_header_t &request,
                         const framework_exception_t &error) const;

    runtime::messaging::message_parts_t
    reply_raw_envelope (const runtime::messaging::envelope_header_t &header,
                        zlink::message_t body) const;
};

} // namespace zlink::framework::detail
