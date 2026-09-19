package systems.zlink.tutorial.server.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.tutorial.shared.Contracts;

// --8<-- [start:instance-spot-handler]
// Handlers are written the same way as room
// handlers: the first type argument is the
// queue, the other two are request and reply.
public final class JoinMatchQueueHandler
    implements ZLinkSpotRequestHandler<
        MatchQueue,
        Contracts.JoinMatchQueue,
        Contracts.MatchQueueStatus> {

    @Override
    public CompletionStage<
            Contracts.MatchQueueStatus> handle(
        MatchQueue queue,
        Contracts.JoinMatchQueue request) {
        queue.enqueue(request.playerId());
        var status =
            new Contracts.MatchQueueStatus(
                queue.waiting());
        return CompletableFuture
            .completedFuture(status);
    }
}
// --8<-- [end:instance-spot-handler]
