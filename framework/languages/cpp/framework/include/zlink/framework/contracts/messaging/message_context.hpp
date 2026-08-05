/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/routing_id.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework
{

/// Read-only projection of the application metadata the framework forwarded for one inbound
/// message. Handlers ask questions of the metadata instead of binding to the map type, so the
/// transport frame layout never reaches the public handler surface.
class message_metadata_t
{
  public:
    message_metadata_t () = default;
    explicit message_metadata_t (std::map<std::string, std::string> values) :
        _values (std::move (values))
    {
    }

    std::optional<std::string_view> find (std::string_view key) const
    {
        const auto iterator = lower_bound (key);
        if (iterator == _values.end () || iterator->first != key) {
            return std::nullopt;
        }
        return std::string_view (iterator->second);
    }

    bool contains (std::string_view key) const
    {
        const auto iterator = lower_bound (key);
        return iterator != _values.end () && iterator->first == key;
    }

    bool empty () const noexcept { return _values.empty (); }

    const std::map<std::string, std::string> &values () const noexcept { return _values; }

  private:
    std::map<std::string, std::string>::const_iterator
    lower_bound (std::string_view key) const
    {
        return std::lower_bound (
          _values.begin (), _values.end (), key,
          [] (const auto &entry, std::string_view value) {
              return std::string_view (entry.first) < value;
          });
    }

    std::map<std::string, std::string> _values;
};

/// Universal inbound message context handed to request, send, Spot packet and Spot Actor handlers.
/// It carries inbound message information only; reply-wait cancellation belongs to the call object
/// and STREAM session termination belongs to the session lifecycle.
struct message_context_t
{
    std::optional<std::string> mesh_name;
    std::optional<std::string> channel_name;
    std::string packet_name;
    std::optional<std::string> content_type;
    message_metadata_t metadata;
    std::optional<std::string> correlation_id;
};

/// Inbound context for event and Spot subscription handlers.
struct publish_message_context_t : message_context_t
{
    std::string topic;
    std::optional<std::string> source;
};

/// Inbound context for RouteMesh node-direct handlers. The universal fields keep the same meaning
/// and only the source node RoutingId is added.
struct route_message_context_t : message_context_t
{
    zlink::routing_id_t source_node_rid;
};

/// Identifies the public dispatch surface currently invoking a root handler filter.
enum class handler_dispatch_kind_t
{
    node_direct_send = 0,
    node_direct_request = 1,
    channel_send = 2,
    channel_request = 3,
    classic_fanout = 4
};

/// Context supplied to root handler filters. Filters inspect the dispatch surface and the same
/// inbound message information that the handler receives; they cannot replace a request reply.
struct handler_filter_context_t : message_context_t
{
    handler_dispatch_kind_t dispatch_kind = handler_dispatch_kind_t::channel_send;
};

namespace detail
{

/// Runtime-private carrier for one inbound message. Dispatch fills it once from the wire envelope
/// and each typed invoker projects the exact public context its handler declared, so no public
/// context type has to be downcast.
struct inbound_message_context_t
{
    message_context_t message;
    std::string topic;
    std::optional<std::string> source;

    publish_message_context_t as_publish_context () const
    {
        publish_message_context_t context{message};
        context.topic = topic;
        context.source = source;
        return context;
    }
};

} // namespace detail

} // namespace zlink::framework
