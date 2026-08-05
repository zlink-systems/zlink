/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"

#include <zlink/framework/contracts/detail/message_name.hpp>

#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <typeindex>

namespace zlink::framework::runtime::messaging
{

class client_call_codec_t
{
  public:
    envelope_header_t
    create_envelope (message_kind_t kind,
                     std::string channel_name,
                     std::string message_name,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds (0),
                     std::optional<std::string> topic = std::nullopt,
                     std::optional<std::string> source = std::nullopt) const;

    template <typename TMessage>
    message_parts_t encode_envelope_parts (const envelope_header_t &header,
                                           const TMessage &message,
                                           const serializer_registry_t &serializers) const
    {
        return _codec.encode_parts (header, std::type_index (typeid (TMessage)), &message,
                                    serializers);
    }

    template <typename TReply>
    result_t<TReply> decode_envelope_reply (const message_parts_t &reply,
                                            const serializer_registry_t &serializers,
                                            const std::string &empty_message,
                                            const std::string &error_message,
                                            const std::string &operation_name) const
    {
        if (reply.size () == 0) {
            return result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                              empty_message);
        }
        auto header = _codec.decode_header (reply);
        if (!header) {
            return result_t<TReply>::failure (
              header.error_kind (), header.error () ? header.error ()->what () : error_message);
        }
        if (header.value ().kind == message_kind_t::error) {
            const auto error = _failure_mapper.error_header_exception (
              header.value ().error_code.value_or ("request_failed"),
              header.value ().error_message.value_or (error_message), operation_name);
            return detail::result_access_t::failure<TReply> (error);
        }
        auto body = _codec.decode_body (reply);
        if (!body) {
            return result_t<TReply>::failure (body.error_kind (), body.error ()
                                                                    ? body.error ()->what ()
                                                                    : "Reply body is missing.");
        }
        try {
            return result_t<TReply>::success (serializers.get<TReply> ().deserialize (
              detail::encoded_payload_from_raw (body.value ())));
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<TReply> (error);
        }
    }

  private:
    envelope_codec_t _codec;
    request_failure_mapper_t _failure_mapper;
};

} // namespace zlink::framework::runtime::messaging
