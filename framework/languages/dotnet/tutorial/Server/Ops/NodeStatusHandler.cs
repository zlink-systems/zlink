using System.Diagnostics;
using Tutorial.Shared;
using Zlink.Framework.Contracts.Channels;

namespace Tutorial.Server.Ops;

// --8<-- [start:node-direct-handler]
// A node-direct handler, not a channel handler. It answers only when a caller
// names this node's routing id, so it reports on this one process.
public sealed class NodeStatusHandler : IZLinkRouteRequestHandler<GetNodeStatus, NodeStatus>
{
    public ValueTask<NodeStatus> HandleAsync(
        GetNodeStatus request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken
    )
    {
        // Process start, not first use of this handler, so the number means
        // what an operator expects it to mean.
        var uptime = DateTime.Now - Process.GetCurrentProcess().StartTime;

        return ValueTask.FromResult(
            new NodeStatus(
                MeshName: context.MeshName ?? "(none)",
                // Null here proves the point: no channel was involved in the routing.
                // A channel handler would find its channel name in this property.
                ChannelName: context.ChannelName ?? "(none)",
                // Node-direct context also carries the caller's routing id.
                CalledBy: context.SourceNodeRid.ToString(),
                Uptime: $"{uptime.TotalSeconds:F0}s",
                ProcessId: Environment.ProcessId
            )
        );
    }
}
// --8<-- [end:node-direct-handler]
