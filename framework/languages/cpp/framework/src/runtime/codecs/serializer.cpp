/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/codecs/serializer.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace zlink::framework::detail
{

struct serializer_descriptor_t
{
    serializer_registry_t::serialize_any_fn_t serialize;
    serializer_registry_t::deserialize_any_fn_t deserialize;
    std::string content_type;
};

class serializer_registry_state_t
{
  public:
    using resolved_serializer_cache_t =
      std::map<std::type_index, std::shared_ptr<const void>>;

    std::map<std::type_index, serializer_descriptor_t> serializers;
    std::map<std::string, std::type_index> type_by_content_type;
    std::shared_ptr<const resolved_serializer_cache_t> resolved_serializers =
      std::make_shared<const resolved_serializer_cache_t> ();
    std::mutex resolved_serializers_mutex;
    std::atomic_bool frozen{false};
};

} // namespace zlink::framework::detail

namespace zlink::framework
{

namespace
{

bool is_media_type_token_character (unsigned char value) noexcept
{
    if ((value >= '0' && value <= '9')
        || (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z'))
        return true;
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.find (static_cast<char> (value))
           != std::string_view::npos;
}

std::string normalize_content_type (std::string_view input)
{
    while (!input.empty ()
           && (input.front () == ' ' || input.front () == '\t'))
        input.remove_prefix (1);
    while (!input.empty ()
           && (input.back () == ' ' || input.back () == '\t'))
        input.remove_suffix (1);

    const auto slash = input.find ('/');
    if (slash == std::string_view::npos || slash == 0
        || slash + 1 == input.size ()
        || input.find ('/', slash + 1) != std::string_view::npos) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "codec content type must be a parameter-free ASCII type/subtype");
    }

    std::string normalized;
    normalized.reserve (input.size ());
    for (std::size_t index = 0; index < input.size (); ++index) {
        const auto value = static_cast<unsigned char> (input[index]);
        if (index == slash) {
            normalized.push_back ('/');
            continue;
        }
        if (!is_media_type_token_character (value)) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "codec content type must be a parameter-free ASCII type/subtype");
        }
        normalized.push_back (
          value >= 'A' && value <= 'Z'
            ? static_cast<char> (value + ('a' - 'A'))
            : static_cast<char> (value));
    }
    return normalized;
}

} // namespace

serializer_registry_t::serializer_registry_t () :
    _state (std::make_unique<detail::serializer_registry_state_t> ())
{
}

serializer_registry_t::~serializer_registry_t () = default;

serializer_registry_t::serializer_registry_t (serializer_registry_t &&) noexcept = default;

serializer_registry_t &
serializer_registry_t::operator= (serializer_registry_t &&) noexcept = default;

serializer_registry_t &serializer_registry_t::add_erased (std::type_index type,
                                                          serialize_any_fn_t serialize,
                                                          deserialize_any_fn_t deserialize,
                                                          std::string content_type)
{
    if (_state->frozen.load (std::memory_order_acquire)) {
        throw framework_exception_t (
          framework_error_kind_t::invalid_operation,
          "codec registry is immutable after runtime startup");
    }
    auto normalized = normalize_content_type (content_type);

    if (const auto existing = _state->serializers.find (type);
        existing != _state->serializers.end ()) {
        const auto indexed =
          _state->type_by_content_type.find (existing->second.content_type);
        if (indexed != _state->type_by_content_type.end ()
            && indexed->second == type)
            _state->type_by_content_type.erase (indexed);
    }

    if (const auto existing =
          _state->type_by_content_type.find (normalized);
        existing != _state->type_by_content_type.end ()
        && existing->second != type) {
        const auto replaced_type = existing->second;
        _state->serializers.erase (replaced_type);
        invalidate_cached_serializer (replaced_type);
    }

    _state->serializers.insert_or_assign (
      type,
      detail::serializer_descriptor_t{
        std::move (serialize), std::move (deserialize), normalized});
    _state->type_by_content_type.insert_or_assign (
      std::move (normalized), type);
    invalidate_cached_serializer (type);
    return *this;
}

std::shared_ptr<const void>
serializer_registry_t::cached_serializer (std::type_index type) const noexcept
{
    const auto cache = std::atomic_load_explicit (
      &_state->resolved_serializers, std::memory_order_acquire);
    const auto found = cache->find (type);
    return found == cache->end () ? nullptr : found->second;
}

std::shared_ptr<const void>
serializer_registry_t::cache_serializer (
  std::type_index type,
  std::shared_ptr<const void> serializer) const
{
    std::lock_guard lock (_state->resolved_serializers_mutex);
    const auto current = std::atomic_load_explicit (
      &_state->resolved_serializers, std::memory_order_acquire);
    if (const auto found = current->find (type); found != current->end ())
        return found->second;

    if (current->size () >= detail::serializer_send_type_cache_capacity)
        return serializer;

    auto next = std::make_shared<detail::serializer_registry_state_t::
                                    resolved_serializer_cache_t> (*current);
    next->emplace (type, std::move (serializer));
    const auto resolved = next->at (type);
    std::shared_ptr<const detail::serializer_registry_state_t::
                      resolved_serializer_cache_t> published = std::move (next);
    std::atomic_store_explicit (&_state->resolved_serializers,
                                published,
                                std::memory_order_release);
    return resolved;
}

void serializer_registry_t::invalidate_cached_serializer (
  std::type_index type) noexcept
{
    std::lock_guard lock (_state->resolved_serializers_mutex);
    const auto current = std::atomic_load_explicit (
      &_state->resolved_serializers, std::memory_order_acquire);
    if (current->find (type) == current->end ())
        return;
    auto next = std::make_shared<detail::serializer_registry_state_t::
                                    resolved_serializer_cache_t> (*current);
    next->erase (type);
    std::shared_ptr<const detail::serializer_registry_state_t::
                      resolved_serializer_cache_t> published = std::move (next);
    std::atomic_store_explicit (&_state->resolved_serializers,
                                published,
                                std::memory_order_release);
}

void serializer_registry_t::freeze () noexcept
{
    _state->frozen.store (true, std::memory_order_release);
}

std::optional<serializer_registry_t::erased_serializer_t>
serializer_registry_t::erased_serializer (std::type_index type) const
{
    const auto found = _state->serializers.find (type);
    if (found == _state->serializers.end ())
        return std::nullopt;
    return erased_serializer_t{
      found->second.serialize, found->second.deserialize,
      found->second.content_type};
}

encoded_payload_t serializer_registry_t::serialize (std::type_index type, const void *value) const
{
    const auto found = _state->serializers.find (type);
    if (found == _state->serializers.end ()) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          std::string ("erased serializer is not registered: ") + type.name ()
            + ". Normal typed JSON payloads use serializer_registry_t::get<T>(); "
              "avoid wrapping them in framework::message_t unless the erased type is registered.");
    }
    try {
        return found->second.serialize (value);
    }
    catch (const framework_exception_t &) {
        throw;
    }
    catch (...) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "payload serialization failed");
    }
}

void serializer_registry_t::deserialize (std::type_index type,
                                         const encoded_payload_t &payload,
                                         void *out) const
{
    const auto found = _state->serializers.find (type);
    if (found == _state->serializers.end ()) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          std::string ("erased serializer is not registered: ") + type.name ()
            + ". Normal typed JSON payloads use serializer_registry_t::get<T>(); "
              "avoid wrapping them in framework::message_t unless the erased type is registered.");
    }
    try {
        found->second.deserialize (payload, out);
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

bool serializer_registry_t::contains (std::type_index type) const
{
    return _state->serializers.find (type) != _state->serializers.end ();
}

std::string serializer_registry_t::content_type (std::type_index type) const
{
    const auto found = _state->serializers.find (type);
    if (found == _state->serializers.end ()) {
        return "application/json";
    }
    if (found->second.content_type.empty ()) {
        return "application/octet-stream";
    }
    return found->second.content_type;
}

} // namespace zlink::framework
