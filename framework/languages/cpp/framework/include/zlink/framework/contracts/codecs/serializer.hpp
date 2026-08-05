/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/errors/error.hpp>
#include <zlink/framework/codecs/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework
{

class encoded_payload_t;

namespace detail
{
class serializer_registry_state_t;
encoded_payload_t encoded_payload_from_raw (const zlink::message_t &message);
zlink::message_t encoded_payload_to_raw (const encoded_payload_t &payload);

template <typename T, typename = void> struct is_json_serializable_t : std::false_type
{
};

template <typename T>
struct is_json_serializable_t<T, std::void_t<decltype (nlohmann::json (std::declval<T> ()))>>
    : std::true_type
{
};

template <typename T, typename = void> struct is_json_deserializable_t : std::false_type
{
};

template <typename T>
struct is_json_deserializable_t<
  T,
  std::void_t<decltype (std::declval<nlohmann::json> ().template get<T> ())>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_json_serializer_compatible_v =
  is_json_serializable_t<T>::value && is_json_deserializable_t<T>::value;
} // namespace detail

class encoded_payload_t
{
  public:
    encoded_payload_t () = default;

    static encoded_payload_t from_bytes (std::span<const std::byte> bytes)
    {
        encoded_payload_t payload;
        payload._bytes.assign (bytes.begin (), bytes.end ());
        return payload;
    }

    static encoded_payload_t from_bytes (std::span<const std::uint8_t> bytes)
    {
        return from_bytes (std::as_bytes (bytes));
    }

    static encoded_payload_t from_string (const std::string &text)
    {
        return from_bytes (std::as_bytes (std::span<const char> (text.data (), text.size ())));
    }

    std::span<const std::byte> bytes () const noexcept { return _bytes; }
    std::vector<std::uint8_t> to_bytes () const
    {
        std::vector<std::uint8_t> result;
        result.reserve (_bytes.size ());
        for (const auto byte : _bytes) {
            result.push_back (static_cast<std::uint8_t> (byte));
        }
        return result;
    }

    std::string to_string () const
    {
        return std::string (reinterpret_cast<const char *> (_bytes.data ()), _bytes.size ());
    }

    std::size_t size () const noexcept { return _bytes.size (); }
    bool empty () const noexcept { return _bytes.empty (); }

  private:
    friend class serializer_registry_t;
    friend class message_t;
    friend encoded_payload_t detail::encoded_payload_from_raw (const zlink::message_t &message);
    friend zlink::message_t detail::encoded_payload_to_raw (const encoded_payload_t &payload);

    static encoded_payload_t from_raw (const zlink::message_t &message)
    {
        return from_bytes (message.bytes ());
    }

    zlink::message_t to_raw () const { return zlink::message_t::from (bytes ()); }

    std::vector<std::byte> _bytes;
};

namespace detail
{
inline encoded_payload_t encoded_payload_from_raw (const zlink::message_t &message)
{
    return encoded_payload_t::from_raw (message);
}

inline zlink::message_t encoded_payload_to_raw (const encoded_payload_t &payload)
{
    return payload.to_raw ();
}
} // namespace detail

template <typename T> class serializer_t
{
  public:
    using serialize_fn_t = std::function<encoded_payload_t (const T &)>;
    using deserialize_fn_t = std::function<T (const encoded_payload_t &)>;

    serializer_t (serialize_fn_t serialize, deserialize_fn_t deserialize) :
        _state (std::make_shared<const state_t> (
          state_t{std::move (serialize), std::move (deserialize)}))
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
            throw detail::make_origin_exception (
              framework_error_kind_t::protocol_error,
              detail::failure_origin_t::payload_encode,
              "payload serialization failed");
        }
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
            throw detail::make_origin_exception (
              framework_error_kind_t::protocol_error,
              detail::failure_origin_t::payload_decode,
              "payload deserialization failed");
        }
    }

  private:
    struct state_t
    {
        serialize_fn_t serialize;
        deserialize_fn_t deserialize;
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
    /// Normal JSON payloads do not need per-message registration.
    template <typename T>
    serializer_registry_t &add (typename serializer_t<T>::serialize_fn_t serialize,
                                typename serializer_t<T>::deserialize_fn_t deserialize,
                                std::string content_type = "application/octet-stream")
    {
        return add_erased (
          std::type_index (typeid (T)),
          [serialize = std::move (serialize)] (const void *value) {
              return serialize (*static_cast<const T *> (value));
          },
          [deserialize = std::move (deserialize)] (const encoded_payload_t &payload, void *out) {
              *static_cast<T *> (out) = deserialize (payload);
          },
          std::move (content_type));
    }

    /// Gets the serializer for T. If no custom serializer is registered and T can be converted
    /// with nlohmann to_json/from_json, the framework uses the default JSON serializer.
    template <typename T> serializer_t<T> get () const
    {
        const auto type = std::type_index (typeid (T));
        if (const auto cached = cached_serializer (type)) {
            return *std::static_pointer_cast<const serializer_t<T>> (cached);
        }

        serializer_t<T> selected = [&] {
            if (!contains (type)) {
            if constexpr (detail::is_json_serializer_compatible_v<T>) {
                return serializer_t<T> (
                  [] (const T &value) {
                      return encoded_payload_t::from_raw (zlink::message_t::from_json (value));
                  },
                  [] (const encoded_payload_t &payload) {
                      return payload.to_raw ().template parse_json<T> ();
                  });
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
                  });
            }
            }
            return serializer_t<T> (
              [this, type] (const T &value) { return serialize (type, &value); },
              [this, type] (const encoded_payload_t &payload) {
                  T value{};
                  deserialize (type, payload, &value);
                  return value;
              });
        } ();
        auto candidate = std::make_shared<const serializer_t<T>> (
          std::move (selected));
        const auto resolved = cache_serializer (type, candidate);
        return *std::static_pointer_cast<const serializer_t<T>> (resolved);
    }

    encoded_payload_t serialize (std::type_index type, const void *value) const;
    void deserialize (std::type_index type, const encoded_payload_t &payload, void *out) const;
    bool contains (std::type_index type) const;
    std::string content_type (std::type_index type) const;

  private:
    std::shared_ptr<const void>
    cached_serializer (std::type_index type) const noexcept;
    std::shared_ptr<const void>
    cache_serializer (std::type_index type,
                      std::shared_ptr<const void> serializer) const;
    void invalidate_cached_serializer (std::type_index type) noexcept;

    serializer_registry_t &add_erased (std::type_index type,
                                       serialize_any_fn_t serialize,
                                       deserialize_any_fn_t deserialize,
                                       std::string content_type);

    std::unique_ptr<detail::serializer_registry_state_t> _state;
};

} // namespace zlink::framework
