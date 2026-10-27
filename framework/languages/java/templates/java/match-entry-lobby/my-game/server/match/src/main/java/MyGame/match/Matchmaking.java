package MyGame.match;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class Matchmaking implements ZLinkSpot<Player> {
    private final ZLinkSpotContext context;
    private final ZLinkSpotManager spots;
    private final ZLinkActorClient actors;
    MatchmakingState state = new MatchmakingState();

    public Matchmaking(ZLinkSpotContext context, ZLinkSpotManager spots, ZLinkActorClient actors) {
        this.context = context;
        this.spots = spots;
        this.actors = actors;
    }

    public ZLinkSpotContext context() {
        return context;
    }

    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage payload) {
        enqueue(id, payload.decode(QueueRequest.class));
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    public CompletionStage<Void> onJoinedActor(Player actor) {
        return match();
    }

    public CompletionStage<Void> onLeaveActor(Player actor) {
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Void> queue(Player actor, QueueRequest request) {
        enqueue(actor.context().actorId(), request);
        return match();
    }

    private void enqueue(String id, QueueRequest request) {
        if (!id.equals(request.playerId()))
            throw new IllegalArgumentException("Queue identity mismatch");
        state.enqueue(request);
    }

    private CompletionStage<Void> match() {
        var pair = state.pair();
        if (pair.isEmpty()) return CompletableFuture.completedFuture(null);
        String roomId = state.allocateRoom();
        return spots.getOrCreate(roomId, Names.ROOM)
                .inMesh(Names.MESH)
                .submit()
                .thenCompose(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Room creation rejected");
                            state.remove(pair);
                            CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
                            var assignment = new RoomAssignment(roomId);
                            for (var player : pair)
                                sends =
                                        sends.thenCompose(
                                                ignored ->
                                                        actors.sendToActor(
                                                                        player.playerId(),
                                                                        assignment)
                                                                .submit());
                            return sends;
                        });
    }
}
