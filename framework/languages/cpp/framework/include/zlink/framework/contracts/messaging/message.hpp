/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>

#include <functional>
#include <memory>
#include <optional>
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
        wrapped._value = std::make_shared<value_type> (std::move (value));
        auto typed_value = std::static_pointer_cast<const value_type> (wrapped._value);
        wrapped._encoder = [typed_value] (const serializer_registry_t &serializers) {
            return serializers.template get<value_type> ().serialize (*typed_value);
        };
        return wrapped;
    }

    template <typename TValue> TValue decode () const
    {
        using value_type = std::remove_cvref_t<TValue>;
        if (_value && _type == std::type_index (typeid (value_type))) {
            return *static_cast<const value_type *> (_value.get ());
        }
        return require_serializers ().template get<value_type> ().deserialize (to_encoded ());
    }

    template <typename TValue> TValue decode (const serializer_registry_t &serializers) const
    {
        using value_type = std::remove_cvref_t<TValue>;
        if (_value && _type == std::type_index (typeid (value_type))) {
            return *static_cast<const value_type *> (_value.get ());
        }
        return serializers.template get<value_type> ().deserialize (to_encoded (serializers));
    }

    bool encoded () const noexcept { return _encoded.has_value (); }
    bool empty () const noexcept
    {
        if (_encoded) {
            return _encoded->empty ();
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
        wrapped._encoded = encoded_payload_t::from_raw (message);
        wrapped._serializers = serializers;
        return wrapped;
    }

    zlink::message_t to_raw () const { return to_encoded ().to_raw (); }

    zlink::message_t to_raw (const serializer_registry_t &serializers) const
    {
        return to_encoded (serializers).to_raw ();
    }

    encoded_payload_t to_encoded () const
    {
        if (_encoded) {
            return *_encoded;
        }
        return to_encoded (require_serializers ());
    }

    encoded_payload_t to_encoded (const serializer_registry_t &serializers) const
    {
        if (_encoded) {
            return *_encoded;
        }
        if (!_value) {
            return {};
        }
        if (_encoder) {
            return _encoder (serializers);
        }
        return serializers.serialize (_type, _value.get ());
    }

    std::optional<encoded_payload_t> _encoded;
    std::shared_ptr<const void> _value;
    std::type_index _type = std::type_index (typeid (void));
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
