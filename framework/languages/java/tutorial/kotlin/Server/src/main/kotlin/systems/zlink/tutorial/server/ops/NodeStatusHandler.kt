package systems.zlink.tutorial.server.ops

import java.time.Duration
import java.time.Instant
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRouteRequestHandler
import systems.zlink.tutorial.shared.GetNodeStatus
import systems.zlink.tutorial.shared.NodeStatus

// --8<-- [start:node-direct-handler]
// A node-direct handler, not a channel handler. It answers only when a caller
// names this node's routing id, so it reports on this one process.
//
// No group annotation here either: ZLinkMeshNodeBuilder has no addHandlerGroup,
// so a node-direct handler is always registered by type.
class NodeStatusHandler : ZLinkSuspendingRouteRequestHandler<GetNodeStatus, NodeStatus> {

    override suspend fun handle(
        request: GetNodeStatus,
        context: ZLinkRouteMessageContext,
    ): NodeStatus {
        val process = ProcessHandle.current()
        // Process start, not first use of this handler, so the number means what
        // an operator expects it to mean.
        val startedAt = process.info().startInstant().orElse(Instant.now())
        val uptime = Duration.between(startedAt, Instant.now())

        return NodeStatus(
            meshName = context.meshName().orElse("(none)"),
            // "(none)" here proves the point: no channel was involved in the
            // routing. A channel handler would find its channel name here.
            channelName = context.channelName().orElse("(none)"),
            // Node-direct context also carries the caller's routing id.
            calledBy = context.sourceNodeRid().toString(),
            uptime = "${uptime.toSeconds()}s",
            processId = process.pid().toInt(),
        )
    }
}
// --8<-- [end:node-direct-handler]
