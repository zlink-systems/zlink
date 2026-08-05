/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/stream_connector/contracts/codec_registry.hpp>

#include <google/protobuf/message_lite.h>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink::framework_codecs
{

/* Protobuf codec extension: payload는 protobuf wire 그대로 싣는다. 등록 방법은 두 가지다.
 *
 *  - `register_message_serializer<TMessage>()`: payload 타입 자체가 protoc이 만든 message일 때.
 *  - `register_payload_serializer<TPayload, TMessage>()`: 도메인 타입을 protobuf message로 옮겨
 *    싣을 때. 변환은 ADL로 찾는 `to_protobuf(const TPayload&, TMessage&)`와
 *    `from_protobuf(const TMessage&, TPayload&)`가 맡는다.
 *
 * 두 경우 모두 wire는 진짜 protobuf라서 다른 언어의 같은 `.proto`와 그대로 통한다. */
class protobuf_codec_extension_t
{
  public:
    template <typename TBuilder> void register_framework_codecs (TBuilder &codecs) const
    {
        (void) codecs;
    }

    void register_connector_codecs (zlink::stream_connector::codec_registry_t &codecs) const
    {
        codecs.enable_codec (zlink::stream_connector::codec_t::protobuf)
          .use_default_codec (zlink::stream_connector::codec_t::protobuf);
    }

    static constexpr const char *content_type = "application/x-protobuf";

    template <typename TMessage, typename TBuilder>
    static void register_message_serializer (TBuilder &codecs)
    {
        static_assert (std::is_base_of_v<google::protobuf::MessageLite, TMessage>,
                       "protobuf codec requires a protobuf message type");
        codecs.template add_serializer<TMessage> (
          [] (const TMessage &value) {
              return zlink::framework::detail::encoded_payload_from_raw (
                zlink::message_t::from (serialize (value)));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              TMessage value;
              parse (value,
                     zlink::framework::detail::encoded_payload_to_raw (payload).to_string ());
              return value;
          },
          content_type);
    }

    template <typename TPayload, typename TMessage, typename TBuilder>
    static void register_payload_serializer (TBuilder &codecs)
    {
        static_assert (std::is_base_of_v<google::protobuf::MessageLite, TMessage>,
                       "protobuf codec requires a protobuf message type");
        codecs.template add_serializer<TPayload> (
          [] (const TPayload &value) {
              TMessage message;
              to_protobuf (value, message);
              return zlink::framework::detail::encoded_payload_from_raw (
                zlink::message_t::from (serialize (message)));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              TMessage message;
              parse (message,
                     zlink::framework::detail::encoded_payload_to_raw (payload).to_string ());
              TPayload value;
              from_protobuf (message, value);
              return value;
          },
          content_type);
    }

  private:
    static std::string serialize (const google::protobuf::MessageLite &message)
    {
        std::string bytes;
        if (!message.SerializeToString (&bytes)) {
            throw std::runtime_error ("protobuf codec failed to serialize "
                                      + std::string (message.GetTypeName ()));
        }
        return bytes;
    }

    static void parse (google::protobuf::MessageLite &message, const std::string &bytes)
    {
        if (!message.ParseFromString (bytes)) {
            throw std::runtime_error ("protobuf codec failed to parse "
                                      + std::string (message.GetTypeName ()));
        }
    }
};

inline protobuf_codec_extension_t protobuf ()
{
    return {};
}

} // namespace zlink::framework_codecs
