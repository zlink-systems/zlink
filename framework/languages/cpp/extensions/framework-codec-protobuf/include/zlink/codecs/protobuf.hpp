/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector/contracts/codec_registry.hpp>

#include <google/protobuf/message_lite.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink::framework_codecs
{

namespace detail
{
inline zlink::framework::encoded_payload_t serialize_protobuf (
  const google::protobuf::MessageLite &value)
{
    const auto size = value.ByteSizeLong ();
    if (size > static_cast<std::size_t> (std::numeric_limits<int>::max ()))
        throw std::length_error ("protobuf payload exceeds serializer size limit");
    auto message = zlink::message_t::allocate (size);
    if (!message.valid ())
        throw std::bad_alloc ();
    if (!value.SerializeToArray (message.data (), static_cast<int> (size)))
        throw std::runtime_error ("protobuf codec failed to serialize "
                                  + std::string (value.GetTypeName ()));
    return zlink::framework::detail::encoded_payload_from_raw (std::move (message));
}

inline void parse_protobuf (google::protobuf::MessageLite &value,
                            const zlink::framework::encoded_payload_t &payload)
{
    const auto bytes = payload.bytes ();
    if (bytes.size () > static_cast<std::size_t> (std::numeric_limits<int>::max ())
        || !value.ParseFromArray (bytes.data (), static_cast<int> (bytes.size ())))
        throw std::runtime_error ("protobuf codec failed to parse "
                                  + std::string (value.GetTypeName ()));
}
} // namespace detail

struct protobuf_registration_key_t
{
};

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
        codecs.template add_serializer<protobuf_registration_key_t> (
          [] (const protobuf_registration_key_t &) {
              return zlink::framework::encoded_payload_t{};
          },
          [] (const zlink::framework::encoded_payload_t &) {
              return protobuf_registration_key_t{};
          },
          content_type);
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
              return detail::serialize_protobuf (value);
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              TMessage value;
              detail::parse_protobuf (value, payload);
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
              return detail::serialize_protobuf (message);
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              TMessage message;
              detail::parse_protobuf (message, payload);
              TPayload value;
              from_protobuf (message, value);
              return value;
          },
          content_type);
    }

};

inline protobuf_codec_extension_t protobuf ()
{
    return {};
}

} // namespace zlink::framework_codecs

namespace zlink::framework::detail
{

template <typename T>
struct extension_serializer_traits_t<
  T,
  std::enable_if_t<std::is_base_of_v<google::protobuf::MessageLite, T>>>
{
    static constexpr bool available = true;
    using registration_key_type = framework_codecs::protobuf_registration_key_t;

    static serializer_t<T> make_serializer ()
    {
        return serializer_t<T> (
          [] (const T &value) {
              return framework_codecs::detail::serialize_protobuf (value);
          },
          [] (const encoded_payload_t &payload) {
              T value;
              framework_codecs::detail::parse_protobuf (value, payload);
              return value;
          },
          framework_codecs::protobuf_codec_extension_t::content_type);
    }
};

} // namespace zlink::framework::detail

namespace zlink::stream_connector::codecs
{

template <typename T>
requires std::is_base_of_v<google::protobuf::MessageLite, T> struct codec_traits<T>
{
    static constexpr codec_t codec = codec_t::protobuf;

    static zlink::message_t encode (const T &value)
    {
        std::string bytes;
        if (!value.SerializeToString (&bytes)) {
            throw std::runtime_error ("protobuf codec failed to serialize "
                                      + std::string (value.GetTypeName ()));
        }
        return zlink::message_t::from (bytes);
    }

    static T decode (const zlink::message_t &payload)
    {
        T value;
        if (!value.ParseFromString (payload.to_string ())) {
            throw std::runtime_error ("protobuf codec failed to parse "
                                      + std::string (value.GetTypeName ()));
        }
        return value;
    }

    static T decode_message_pack (const zlink::message_t &)
    {
        throw std::runtime_error ("protobuf payload cannot be decoded as MessagePack");
    }
};

} // namespace zlink::stream_connector::codecs
