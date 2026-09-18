#pragma once

#include "../../Shared/contracts.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <string>

namespace fw = zlink::framework;

// Set while the process starts, before main() runs. The Framework builds a
// handler per call, so a member of the handler would only ever measure zero.
inline const auto process_started_at = std::chrono::steady_clock::now ();

// --8<-- [start:node-direct-handler]
// A node-direct handler, not a channel handler. It answers only when a caller
// names this node's routing id, so it reports on this one process.
class node_status_handler_t
{
  public:
    using request_type = get_node_status_t;
    using reply_type = node_status_t;

    // Asking for the context is what gives a handler the routing facts below.
    // A handler that does not need them declares handle(const request_type &).
    reply_type handle (const request_type &, const fw::route_message_context_t &context)
    {
        const auto uptime = std::chrono::steady_clock::now () - process_started_at;

        return node_status_t{
          context.mesh_name.value_or ("(none)"),
          // Empty here proves the point: no channel was involved in the routing.
          // A channel handler would find its channel name in this field.
          context.channel_name.value_or (""),
          // Node-direct context also carries the caller's routing id.
          context.source_node_rid.to_string (),
          std::to_string (std::chrono::duration_cast<std::chrono::seconds> (uptime).count ())
            + "s"};
    }
};
// --8<-- [end:node-direct-handler]
