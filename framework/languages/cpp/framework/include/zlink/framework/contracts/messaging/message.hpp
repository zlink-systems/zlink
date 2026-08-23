/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <type_traits>
#include <utility>

namespace zlink::framework
{

namespace runtime
{
class actor_client_impl_t;
class mesh_node_host_service_t;
} // namespace runtime

class actor_context_t;
class app_t;
class bound_session_t;
class message_t;
class session_actor_t;
class spot_actor_join_result_t;
class spot_create_response_t;
class spot_handler_registry_t;
class spot_node_builder_t;
class spot_manager_t;
template <typename TActor> class spot_t;

namespace detail
{
class actor_gateway_runtime_t;
class channel_runtime_t;
class mesh_node_runtime_t;
class spot_node_runtime_t;
class stream_runtime_t;
zlink::message_t message_to_raw (const message_t &message,
                                 const serializer_registry_t &serializers);
} // namespace detail

class message_t
{
  public:
    message_t () = default;

    /// Wraps a typed value for APIs that accept message_t. The value is encoded later through the
    /// serializer selected for TValue, so normal JSON payloads do not need per-message registration.
    template <typename TValue> static message_t from (TValue value)
    {
        using value_type = std::remove_cvref_t<TValue>;
        message_t wrapped;
        wrapped._type = std::type_index (typeid (value_type));
        wrapped._packet_name = detail::message_name<value_type> ();
        wrapped._value = std::make_shared<value_type> (std::move (value));
        auto typed_value = std::static_pointer_cast<const value_type> (wrapped._value);
        wrapped._encoder = [typed_value] (const serializer_registry_t &serializers) {
            return serializers.template get<value_type> ().serialize (*typed_value);
        };
        wrapped._decode = std::make_shared<decode_state_t> ();
        return wrapped;
    }

    template <typename TValue> TValue decode () const
    {
        using value_type = std::remove_cvref_t<TValue>;
        if (_value && _type == std::type_index (typeid (value_type))) {
            return *static_cast<const value_type *> (_value.get ());
        }
        const auto &serializers = require_serializers ();
        return decode_encoded<value_type> (serializers);
    }

    template <typename TValue> TValue decode (const serializer_registry_t &serializers) const
    {
        using value_type = std::remove_cvref_t<TValue>;
        if (_value && _type == std::type_index (typeid (value_type))) {
            return *static_cast<const value_type *> (_value.get ());
        }
        return decode_encoded<value_type> (serializers);
    }

    bool encoded () const noexcept { return static_cast<bool> (_raw_payload); }
    bool empty () const noexcept
    {
        if (_raw_payload) {
            return _raw_payload->encoded.empty ();
        }
        return !_value;
    }

  private:
    friend class detail::actor_gateway_runtime_t;
    friend class detail::channel_runtime_t;
    friend class detail::mesh_node_runtime_t;
    friend class detail::spot_node_runtime_t;
    friend class detail::stream_runtime_t;
    friend class runtime::actor_client_impl_t;
    friend class runtime::mesh_node_host_service_t;
    friend class app_t;
    friend zlink::message_t detail::message_to_raw (const message_t &message,
                                                    const serializer_registry_t &serializers);
    friend class actor_context_t;
    friend class bound_session_t;
    friend class session_actor_t;
    friend class spot_actor_join_result_t;
    friend class spot_create_response_t;
    friend class spot_handler_registry_t;
    friend class spot_node_builder_t;
    friend class spot_manager_t;
    template <typename TActor> friend class spot_t;

    static message_t from_raw (zlink::message_t message,
                               const serializer_registry_t *serializers = nullptr)
    {
        message_t wrapped;
        auto raw_payload = std::make_shared<raw_payload_state_t> ();
        raw_payload->message = std::move (message);
        raw_payload->encoded = encoded_payload_t::from_raw (
          raw_payload->message);
        wrapped._raw_payload = std::move (raw_payload);
        wrapped._decode = std::make_shared<decode_state_t> ();
        wrapped._serializers = serializers;
        return wrapped;
    }

    struct raw_payload_state_t
    {
        zlink::message_t message;
        encoded_payload_t encoded;
    };

    struct decode_state_t
    {
        std::mutex gate;
        bool attempted = false;
        std::type_index type{typeid (void)};
        std::shared_ptr<const void> value;
        std::exception_ptr failure;
    };

    template <typename TValue>
    TValue decode_encoded (const serializer_registry_t &serializers) const
    {
        using value_type = std::remove_cvref_t<TValue>;
        if (!_decode) {
            return with_encoded_payload (
              serializers,
              [&] (const encoded_payload_t &payload) {
                  return serializers.template get<value_type> ().deserialize (
                    payload);
              });
        }

        std::lock_guard lock (_decode->gate);
        if (_decode->attempted) {
            if (_decode->failure)
                std::rethrow_exception (_decode->failure);
            if (_decode->type != std::type_index (typeid (value_type))) {
                throw framework_exception_t (
                  framework_error_kind_t::protocol_error,
                  "framework message was already decoded as another payload type");
            }
            if constexpr (std::is_copy_constructible_v<value_type>) {
                return *static_cast<const value_type *> (
                  _decode->value.get ());
            }
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "decoded framework message payload is not copy constructible");
        }

        _decode->attempted = true;
        _decode->type = std::type_index (typeid (value_type));
        try {
            auto decoded = with_encoded_payload (
              serializers,
              [&] (const encoded_payload_t &payload) {
                  return serializers.template get<value_type> ().deserialize (
                    payload);
              });
            if constexpr (std::is_copy_constructible_v<value_type>) {
                _decode->value = std::make_shared<const value_type> (decoded);
            }
            return decoded;
        }
        catch (...) {
            _decode->value.reset ();
            _decode->failure = std::current_exception ();
            throw;
        }
    }

    template <typename TVisitor>
    auto with_encoded_payload (const serializer_registry_t &serializers,
                               TVisitor &&visitor) const
      -> std::invoke_result_t<TVisitor, const encoded_payload_t &>
    {
        using result_type =
          std::invoke_result_t<TVisitor, const encoded_payload_t &>;
        static_assert (!std::is_reference_v<result_type>,
                       "encoded payload visitor result must own its value");
        if (_raw_payload) {
            return std::forward<TVisitor> (visitor) (
              _raw_payload->encoded);
        }
        encoded_payload_t encoded;
        if (!_value) {
            return std::forward<TVisitor> (visitor) (encoded);
        }
        if (_encoder) {
            encoded = _encoder (serializers);
        }
        else {
            encoded = serializers.serialize (_type, _value.get ());
        }
        return std::forward<TVisitor> (visitor) (encoded);
    }

    zlink::message_t to_raw () const
    {
        const auto &serializers = require_serializers ();
        return with_encoded_payload (
          serializers,
          [] (const encoded_payload_t &payload) { return payload.to_raw (); });
    }

    zlink::message_t to_raw (const serializer_registry_t &serializers) const
    {
        return with_encoded_payload (
          serializers,
          [] (const encoded_payload_t &payload) { return payload.to_raw (); });
    }

    std::shared_ptr<raw_payload_state_t> _raw_payload;
    std::shared_ptr<decode_state_t> _decode;
    std::shared_ptr<const void> _value;
    std::type_index _type = std::type_index (typeid (void));
    std::string _packet_name;
    std::function<encoded_payload_t (const serializer_registry_t &)> _encoder;
    const serializer_registry_t *_serializers = nullptr;

    const serializer_registry_t &require_serializers () const
    {
        if (_serializers == nullptr) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "framework message has no serializer registry");
        }
        return *_serializers;
    }
};

namespace detail
{
inline zlink::message_t message_to_raw (const message_t &message,
                                        const serializer_registry_t &serializers)
{
    return message.to_raw (serializers);
}
} // namespace detail

} // namespace zlink::framework
