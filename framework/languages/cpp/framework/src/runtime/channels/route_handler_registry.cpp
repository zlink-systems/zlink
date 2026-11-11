/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_handler_registry.hpp"

#include <algorithm>
#include <utility>

namespace zlink::framework::detail
{

route_handler_registry_t &
route_handler_registry_t::add_handler (route_handler_descriptor_t descriptor,
                                       invoker_t invoker,
                                       std::function<void (serializer_registry_t &)> bind_codecs)
{
    key_t key{descriptor.router_channel_id, descriptor.kind, descriptor.packet_name};
    const auto [_, inserted] =
      _handlers.emplace (std::move (key), entry_t{std::move (descriptor), std::move (invoker),
                                                  std::move (bind_codecs)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate routed handler registration");
    }
    return *this;
}

void route_handler_registry_t::bind_codecs (serializer_registry_t &serializers)
{
    for (auto &[_, entry] : _handlers)
        if (entry.bind_codecs)
            entry.bind_codecs (serializers);
}

std::vector<std::string> route_handler_registry_t::router_channel_ids () const
{
    std::vector<std::string> channels;
    channels.reserve (_handlers.size ());
    for (const auto &[key, _] : _handlers) {
        if (std::find (channels.begin (), channels.end (), key.router_channel_id)
            == channels.end ())
            channels.push_back (key.router_channel_id);
    }
    return channels;
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
    const auto found = _handlers.find (key_view_t{router_channel_id, kind, packet_name});
    return found == _handlers.end () ? nullptr : &found->second;
}

} // namespace zlink::framework::detail
