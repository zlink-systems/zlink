/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#include <zlink/framework/detail/test_creation_probe.hpp>
#endif

#include <zlink/json_profile.hpp>

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/errors/error.hpp>
#include <zlink/framework/codecs/json.hpp>

#include <exception>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework
{

class encoded_payload_t;
class codec_registration_context_t;

namespace detail
{
struct prepared_header_fragment_t
{
    std::string bytes;
    std::exception_ptr error;
};
struct request_type_plan_t
{
    std::string content_type;
    std::string prefix;
    std::string middle;
    std::exception_ptr error;
};
prepared_header_fragment_t prepare_request_channel (std::string_view channel);
request_type_plan_t prepare_request_type (std::string_view name, std::string content_type);

inline constexpr std::size_t serializer_send_type_cache_capacity = 1024;
class serializer_registry_state_t;
struct serializer_registry_access_t;
struct serializer_registry_test_access_t;
encoded_payload_t encoded_payload_from_raw (const zlink::message_t &message);
encoded_payload_t encoded_payload_from_raw (std::span<const std::byte> bytes);
encoded_payload_t encoded_payload_from_raw (zlink::message_t &&message);
zlink::message_t encoded_payload_to_raw (const encoded_payload_t &payload);
zlink::message_t encoded_payload_to_raw (encoded_payload_t &&payload);

template <typename T> struct service_packet_codec_t;

template <typename T, typename = void> struct extension_serializer_traits_t
{
    static constexpr bool available = false;
};
} // namespace detail

class encoded_payload_t
{
  public:
    encoded_payload_t () = default;

    encoded_payload_t (const encoded_payload_t &other) :
        encoded_payload_t (zlink::message_t::from (other.bytes ()))
    {
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        detail::test_creation_probe::payload_copies.fetch_add (1, std::memory_order_relaxed);
#endif
    }

    encoded_payload_t &operator= (const encoded_payload_t &other)
    {
        if (this == &other)
            return *this;
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        detail::test_creation_probe::payload_copies.fetch_add (1, std::memory_order_relaxed);
#endif
        const auto source = other.bytes ();
        auto owned = zlink::message_t::from (source);
        if (!owned.valid ())
            throw std::bad_alloc ();
        _storage = std::move (owned);
        return *this;
    }

    encoded_payload_t (encoded_payload_t &&) noexcept = default;
    encoded_payload_t &operator= (encoded_payload_t &&) noexcept = default;

    static encoded_payload_t from_bytes (std::span<const std::byte> bytes)
    {
        return encoded_payload_t (zlink::message_t::from (bytes));
    }

    static encoded_payload_t from_bytes (std::span<const std::uint8_t> bytes)
    {
        return from_bytes (std::as_bytes (bytes));
    }

    static encoded_payload_t from_string (const std::string &text)
    {
        return from_bytes (std::as_bytes (std::span<const char> (text.data (), text.size ())));
    }

    std::span<const std::byte> bytes () const noexcept
    {
        if (const auto *message = std::get_if<zlink::message_t> (&_storage))
            return message->bytes ();
        return std::get<std::span<const std::byte>> (_storage);
    }
    std::vector<std::uint8_t> to_bytes () const
    {
        const auto source = bytes ();
        std::vector<std::uint8_t> result;
        result.reserve (source.size ());
        for (const auto byte : source) {
            result.push_back (static_cast<std::uint8_t> (byte));
        }
        return result;
    }

    std::string to_string () const
    {
        const auto source = bytes ();
        if (source.empty ())
            return {};
        return std::string (reinterpret_cast<const char *> (source.data ()), source.size ());
    }

    std::size_t size () const noexcept { return bytes ().size (); }
    bool empty () const noexcept { return bytes ().empty (); }

  private:
    friend class serializer_registry_t;
    friend class message_t;
    friend encoded_payload_t detail::encoded_payload_from_raw (const zlink::message_t &message);
    friend encoded_payload_t detail::encoded_payload_from_raw (std::span<const std::byte> bytes);
    friend encoded_payload_t detail::encoded_payload_from_raw (zlink::message_t &&message);
    friend zlink::message_t detail::encoded_payload_to_raw (const encoded_payload_t &payload);
    friend zlink::message_t detail::encoded_payload_to_raw (encoded_payload_t &&payload);

    explicit encoded_payload_t (zlink::message_t &&message) : _storage (std::move (message))
    {
        if (!std::get<zlink::message_t> (_storage).valid ())
            throw std::bad_alloc ();
    }

    static encoded_payload_t from_text (std::string text)
    {
        auto storage = std::make_unique<std::string> (std::move (text));
        const auto data =
          std::as_writable_bytes (std::span<char> (storage->data (), storage->size ()));
        auto message = zlink::advanced::external_message_t::from (
          data, [] (void *, void *hint) { delete static_cast<std::string *> (hint); },
          storage.get ());
        if (!message.valid ())
            throw std::bad_alloc ();
        storage.release ();
        return encoded_payload_t (std::move (message));
    }

    zlink::message_t to_raw () const
    {
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
        detail::test_creation_probe::payload_copies.fetch_add (1, std::memory_order_relaxed);
#endif
        if (const auto *message = std::get_if<zlink::message_t> (&_storage))
            return message->copy ();
        return zlink::message_t::from (bytes ());
    }

    // Only the private raw-message bridge creates a borrowed view. Public
    // factories and copies keep value semantics by owning their bytes.
    std::variant<std::span<const std::byte>, zlink::message_t> _storage;
};

/// Encoded bytes and the media type chosen by the same typed serializer.
/// Keeping these values together prevents an erased type lookup from selecting
/// different wire metadata after serialization.
struct serialized_payload_t
{
    encoded_payload_t payload;
    std::string content_type;
};

namespace detail
{
inline encoded_payload_t encoded_payload_from_raw (std::span<const std::byte> bytes)
{
    encoded_payload_t payload;
    payload._storage = bytes;
    return payload;
}

inline encoded_payload_t encoded_payload_from_raw (const zlink::message_t &message)
{
    return encoded_payload_from_raw (message.bytes ());
}

inline encoded_payload_t encoded_payload_from_raw (zlink::message_t &&message)
{
    return encoded_payload_t (std::move (message));
}

inline zlink::message_t encoded_payload_to_raw (const encoded_payload_t &payload)
{
    return payload.to_raw ();
}

inline zlink::message_t encoded_payload_to_raw (encoded_payload_t &&payload)
{
    if (auto *message = std::get_if<zlink::message_t> (&payload._storage))
        return std::move (*message);
    return payload.to_raw ();
}
} // namespace detail

template <typename T> class serializer_t
{
  public:
    using serialize_fn_t = std::function<encoded_payload_t (const T &)>;
    using deserialize_fn_t = std::function<T (const encoded_payload_t &)>;

    serializer_t (serialize_fn_t serialize,
                  deserialize_fn_t deserialize,
                  std::string content_type) :
        _state (std::make_shared<const state_t> (state_t{
          std::move (serialize), std::move (deserialize),
          detail::prepare_request_type (detail::message_name<T> (), std::move (content_type))}))
    {
    }

    encoded_payload_t serialize (const T &value) const
    {
        try {
            return _state->serialize (value);
        }
        catch (const framework_exception_t &) {
            throw;
        }
        catch (...) {
            throw detail::make_origin_exception (framework_error_kind_t::protocol_error,
                                                 detail::failure_origin_t::payload_encode,
                                                 "payload serialization failed");
        }
    }

    serialized_payload_t serialize_with_content_type (const T &value) const
    {
        return serialized_payload_t{serialize (value), _state->plan.content_type};
    }

    T deserialize (const encoded_payload_t &payload) const
    {
        try {
            return _state->deserialize (payload);
        }
        catch (const framework_exception_t &) {
            throw;
        }
        catch (...) {
            std::throw_with_nested (detail::make_origin_exception (
              framework_error_kind_t::protocol_error, detail::failure_origin_t::payload_decode,
              "payload deserialization failed"));
        }
    }

    const std::string &content_type () const noexcept { return _state->plan.content_type; }

  private:
    friend struct detail::serializer_registry_access_t;
    struct state_t
    {
        serialize_fn_t serialize;
        deserialize_fn_t deserialize;
        detail::request_type_plan_t plan;
    };

    std::shared_ptr<const state_t> _state;
};

class serializer_registry_t
{
  public:
    using serialize_any_fn_t = std::function<encoded_payload_t (const void *)>;
    using deserialize_any_fn_t = std::function<void (const encoded_payload_t &, void *)>;

    serializer_registry_t ();
    ~serializer_registry_t ();

    serializer_registry_t (serializer_registry_t &&) noexcept;
    serializer_registry_t &operator= (serializer_registry_t &&) noexcept;
    serializer_registry_t (const serializer_registry_t &) = delete;
    serializer_registry_t &operator= (const serializer_registry_t &) = delete;

    /// Registers a serializer for a payload that cannot use the default JSON path.
    /// The content type is normalized to a parameter-free lowercase ASCII media type.
    /// A later registration replaces an earlier registration with the same normalized
    /// content type. Normal JSON payloads do not need per-message registration.
    template <typename T>
    serializer_registry_t &add (typename serializer_t<T>::serialize_fn_t serialize,
                                typename serializer_t<T>::deserialize_fn_t deserialize,
                                std::string content_type = "application/octet-stream")
    {
        return add_for_registration<T> (std::move (serialize), std::move (deserialize),
                                        std::move (content_type), std::nullopt);
    }

    /// Gets the serializer for T. The default JSON serializer reads dto_traits and enum_traits.
    template <typename T> serializer_t<T> get () const
    {
        const auto type = std::type_index (typeid (T));
        if (const auto cached = cached_serializer (type)) {
            return *std::static_pointer_cast<const serializer_t<T>> (cached);
        }

        auto selected = make_serializer<T> ();
        auto candidate = std::make_shared<const serializer_t<T>> (std::move (selected));
        const auto resolved = cache_serializer (type, candidate);
        return *std::static_pointer_cast<const serializer_t<T>> (resolved);
    }

    encoded_payload_t serialize (std::type_index type, const void *value) const;
    void deserialize (std::type_index type, const encoded_payload_t &payload, void *out) const;
    bool contains (std::type_index type) const;
    std::string content_type (std::type_index type) const;

  private:
    friend struct detail::serializer_registry_access_t;
    friend struct detail::serializer_registry_test_access_t;
    friend class codec_registration_context_t;

    template <typename T> serializer_t<T> make_serializer () const
    {
        const auto type = std::type_index (typeid (T));
        return [&] {
            auto registered = erased_serializer (type);
            if (!registered) {
                if constexpr (requires { detail::service_packet_codec_t<T>::make_serializer (); }) {
                    return detail::service_packet_codec_t<T>::make_serializer ();
                } else {
                    if constexpr (detail::extension_serializer_traits_t<T>::available) {
                        using traits_t = detail::extension_serializer_traits_t<T>;
                        const auto extension = erased_serializer (
                          std::type_index (typeid (typename traits_t::registration_key_type)));
                        if (extension) {
                            return traits_t::make_serializer ();
                        }
                    }
                    if constexpr (zlink::detail::json_profile::is_json_candidate<T> ()) {
                        static_assert (zlink::detail::json_profile::validate_type<T> ());
                        return serializer_t<T> (
                          [] (const T &value) {
                              return encoded_payload_t::from_text (
                                zlink::detail::json_profile::encode (value));
                          },
                          [] (const encoded_payload_t &payload) {
                              const auto bytes = payload.bytes ();
                              const auto *begin = reinterpret_cast<const char *> (bytes.data ());
                              const auto *end = begin == nullptr ? begin : begin + bytes.size ();
                              return zlink::detail::json_profile::decode<T> (begin, end);
                          },
                          zlink::detail::json_profile::content_type);
                    } else {
                        return serializer_t<T> (
                          [] (const T &) -> encoded_payload_t {
                              throw framework_exception_t (
                                framework_error_kind_t::protocol_error,
                                "No serializer is registered for this payload type");
                          },
                          [] (const encoded_payload_t &) -> T {
                              throw framework_exception_t (
                                framework_error_kind_t::protocol_error,
                                "No serializer is registered for this payload type");
                          },
                          zlink::detail::json_profile::content_type);
                    }
                }
            }
            auto serialize = std::move (registered->serialize);
            auto deserialize = std::move (registered->deserialize);
            return serializer_t<T> (
              [serialize = std::move (serialize)] (const T &value) { return serialize (&value); },
              [deserialize = std::move (deserialize)] (const encoded_payload_t &payload) {
                  T value{};
                  deserialize (payload, &value);
                  return value;
              },
              std::move (registered->content_type));
        }();
    }

    explicit serializer_registry_t (std::shared_ptr<detail::serializer_registry_state_t> state) :
        _state (std::move (state))
    {
    }

    template <typename T>
    serializer_registry_t &
    add_for_registration (typename serializer_t<T>::serialize_fn_t serialize,
                          typename serializer_t<T>::deserialize_fn_t deserialize,
                          std::string content_type,
                          std::optional<std::size_t> registration)
    {
        return add_erased (
          std::type_index (typeid (T)),
          [serialize = std::move (serialize)] (const void *value) {
              return serialize (*static_cast<const T *> (value));
          },
          [deserialize = std::move (deserialize)] (const encoded_payload_t &payload, void *out) {
              *static_cast<T *> (out) = deserialize (payload);
          },
          std::move (content_type), registration);
    }

    struct erased_serializer_t
    {
        serialize_any_fn_t serialize;
        deserialize_any_fn_t deserialize;
        std::string content_type;
    };

    std::optional<erased_serializer_t> erased_serializer (std::type_index type) const;
    std::shared_ptr<const void> cached_serializer (std::type_index type) const noexcept;
    std::shared_ptr<const void> cache_serializer (std::type_index type,
                                                  std::shared_ptr<const void> serializer) const;
    void invalidate_cached_serializer (std::type_index type) noexcept;
    void set_resolved_serializer_cache_capacity_for_tests (std::size_t capacity) noexcept;
    void freeze () noexcept;
    std::size_t begin_registration ();

    serializer_registry_t &add_erased (std::type_index type,
                                       serialize_any_fn_t serialize,
                                       deserialize_any_fn_t deserialize,
                                       std::string content_type,
                                       std::optional<std::size_t> registration);

    std::shared_ptr<detail::serializer_registry_state_t> _state;
};

namespace detail
{

struct serializer_registry_access_t
{
    template <typename T>
    static std::shared_ptr<const request_type_plan_t>
    request_plan (const serializer_t<T> &serializer) noexcept
    {
        return {serializer._state, &serializer._state->plan};
    }
    static serializer_registry_t retain (const serializer_registry_t &registry)
    {
        return serializer_registry_t (registry._state);
    }

    static void freeze (serializer_registry_t &registry) noexcept { registry.freeze (); }
    static bool frozen (const serializer_registry_t &registry) noexcept;
    template <typename T> static serializer_t<T> bind (const serializer_registry_t &registry)
    {
        return registry.make_serializer<T> ();
    }
};

template <typename TPayload>
TPayload deserialize_typed_payload (const serializer_t<TPayload> &serializer,
                                    const zlink::message_t &message,
                                    std::string_view content_type)
{
    if (!content_type.empty () && content_type != serializer.content_type ())
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "inbound content type does not match the typed handler codec");
    return serializer.deserialize (encoded_payload_from_raw (message));
}

// A handler's immutable construction plan retains descriptors from its registry.
// Standalone invocations before host startup retain the ordinary registry path.
template <typename TRequest, typename TReply = void> struct handler_codec_plan_t
{
    std::optional<serializer_t<TRequest>> request;
    using reply_t =
      std::conditional_t<std::is_void_v<TReply>, std::monostate, serializer_t<TReply>>;
    std::optional<reply_t> reply;
    void bind (serializer_registry_t &serializers)
    {
        if (!serializer_registry_access_t::frozen (serializers))
            return;
        request = serializer_registry_access_t::bind<TRequest> (serializers);
        if constexpr (!std::is_void_v<TReply>)
            reply = serializer_registry_access_t::bind<TReply> (serializers);
    }
    TRequest deserialize (serializer_registry_t &serializers,
                          const zlink::message_t &message,
                          std::string_view wire_type) const
    {
        return request
                 ? deserialize_typed_payload (*request, message, wire_type)
                 : deserialize_typed_payload (serializers.get<TRequest> (), message, wire_type);
    }
    template <typename TValue>
    encoded_payload_t serialize (serializer_registry_t &serializers, const TValue &value) const
    {
        return reply ? reply->serialize (value) : serializers.get<TValue> ().serialize (value);
    }
};

template <typename TPayload>
TPayload deserialize_typed_payload (serializer_registry_t &serializers,
                                    const zlink::message_t &message,
                                    std::string_view content_type)
{
    return deserialize_typed_payload (serializers.get<TPayload> (), message, content_type);
}

} // namespace detail

} // namespace zlink::framework
