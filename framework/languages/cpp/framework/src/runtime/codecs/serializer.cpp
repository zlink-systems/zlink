/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/codecs/serializer.hpp>

#include <atomic>
#include <map>
#include <mutex>
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
    std::shared_ptr<const resolved_serializer_cache_t> resolved_serializers =
      std::make_shared<const resolved_serializer_cache_t> ();
    std::mutex resolved_serializers_mutex;
};

} // namespace zlink::framework::detail

namespace zlink::framework
{

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
    const auto [_, inserted] = _state->serializers.emplace (
      type, detail::serializer_descriptor_t{std::move (serialize), std::move (deserialize),
                                            std::move (content_type)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate serializer registration");
    }
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
