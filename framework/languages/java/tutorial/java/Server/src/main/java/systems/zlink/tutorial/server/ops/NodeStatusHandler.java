package systems.zlink.tutorial.server.ops;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:node-direct-handler]
// A node-direct handler, not a channel handler. It answers only when a caller
// names this node's routing id, so it reports on this one process.
public final class NodeStatusHandler
    implements ZLinkRouteRequestHandler<Contracts.GetNodeStatus, Contracts.NodeStatus> {

    @Override
    public CompletionStage<Contracts.NodeStatus> handle(
        Contracts.GetNodeStatus request,
        ZLinkRouteMessageContext context) {
        // Process start, not first use of this handler, so the number means
        // what an operator expects it to mean.
        ProcessHandle process = ProcessHandle.current();
        Duration uptime = Duration.between(
            process.info().startInstant().orElse(Instant.now()),
            Instant.now());

        return CompletableFuture.completedFuture(new Contracts.NodeStatus(
            context.meshName().orElse("(none)"),
            // Empty here proves the point: no channel was involved in the routing.
            // A channel handler would find its channel name in this property.
            context.channelName().orElse("(none)"),
            // Node-direct context also carries the caller's routing id.
            context.sourceNodeRid().toString(),
            uptime.toSeconds() + "s",
            (int) process.pid()));
    }
}
// --8<-- [end:node-direct-handler]
