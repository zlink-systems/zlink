/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"

#include <utility>
#include <cstring>

namespace zlink::framework::detail
{

route_handler_registry_t &
route_handler_registry_t::add_handler (route_handler_descriptor_t descriptor,
                                       invoker_t invoker,
                                       std::function<void (serializer_registry_t &)> bind_codecs)
{
    // Published tables remain immutable while a prepared entry retains them.
    if (!_handlers.unique ())
        _handlers = std::make_shared<handlers_t> (*_handlers);
    key_t key{descriptor.router_channel_id, descriptor.kind, descriptor.packet_name};
    const auto [_, inserted] =
      _handlers->emplace (std::move (key), entry_t{std::move (descriptor), std::move (invoker),
                                                   std::move (bind_codecs)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate routed handler registration");
    }
    return *this;
}

void route_handler_registry_t::bind_codecs (serializer_registry_t &serializers)
{
    for (auto &[_, entry] : *_handlers)
        if (entry.bind_codecs)
            entry.bind_codecs (serializers);
}

const route_handler_descriptor_t *
route_handler_registry_t::find (std::string_view router_channel_id,
                                runtime::messaging::message_kind_t kind,
                                std::string_view packet_name) const
{
    const auto *entry = resolve (router_channel_id, kind, packet_name);
    return entry ? &entry->descriptor : nullptr;
}

const route_handler_registry_t::entry_t *
route_handler_registry_t::resolve (std::string_view router_channel_id,
                                   runtime::messaging::message_kind_t kind,
                                   std::string_view packet_name) const
{
    const auto found = _handlers->find (key_view_t{router_channel_id, kind, packet_name});
    return found == _handlers->end () ? nullptr : &found->second;
}

result_t<route_handler_registry_t::prepared_header_t> route_handler_registry_t::prepare_header (
  const zlink::message_t &message,
  std::string_view channel,
  bool capture_flow,
  std::optional<runtime::messaging::envelope_header_t> *rejection_context) const
{
    // Same bounded, byte-compared, insertion-order cache as the .NET codec.
    // Worker-local storage needs no publication lock. Entries retain a scope-free
    // registry table, never an invocation provider or a scoped instance.
    static thread_local std::vector<std::shared_ptr<const cached_header_t>> cache;
    const auto bytes = message.bytes ();
    if (rejection_context)
        rejection_context->reset ();
    for (const auto &entry : cache) {
        if (entry->registration.get () == _handlers.get () && entry->channel == channel
            && entry->bytes.size () == bytes.size ()
            && std::memcmp (entry->bytes.data (), bytes.data (), bytes.size ()) == 0)
            return result_t<prepared_header_t>::success (prepared_header_t (entry));
    }
    bool repeatable = false;
    auto decoded = runtime::messaging::envelope_codec_t{}.decode_header_view (
      message, capture_flow, rejection_context, &repeatable);
    if (!decoded)
        return propagate_failure<prepared_header_t> (decoded,
                                                     "route envelope header decode failed");
    const auto &header = decoded.value ();
    const auto *handler =
      resolve (channel.empty () ? std::string_view (header.channel_name) : channel, header.kind,
               header.message_name);
    const auto packet_class = spot_route_internal_dispatcher_t::classify (header.message_name);
    if (!repeatable || bytes.size () > 1024)
        return result_t<prepared_header_t>::success (
          prepared_header_t (std::move (decoded.value ()), handler, packet_class));
    auto entry = std::make_shared<const cached_header_t> (
      cached_header_t{std::string (reinterpret_cast<const char *> (bytes.data ()), bytes.size ()),
                      std::string (channel), header, _handlers, handler, packet_class});
    if (cache.size () == 64)
        cache.erase (cache.begin ());
    cache.push_back (entry);
    return result_t<prepared_header_t>::success (prepared_header_t (std::move (entry)));
}

} // namespace zlink::framework::detail
