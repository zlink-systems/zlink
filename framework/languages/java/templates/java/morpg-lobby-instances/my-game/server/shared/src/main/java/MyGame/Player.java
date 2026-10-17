package MyGame;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;

    public Player(ZLinkActorContext context) {
        this.context = context;
    }

    public ZLinkActorContext context() {
        return context;
    }

    public CompletionStage<Void> deliver(String sourceSpotId, Object message) {
        if (!sourceSpotId.equals(context.spotId().orElse(null)))
            return CompletableFuture.completedFuture(null);
        return context.boundSession().send(message).submit();
    }

    public void checkIdentity(String playerId) {
        if (!context.actorId().equals(playerId))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        return switch (completion) {
            case ZLinkActorJoinCompletion.Accepted accepted ->
                    CompletableFuture.completedFuture(null);
            case ZLinkActorJoinCompletion.Rejected rejected ->
                    context.boundSession().send(new ActionRejected(Rules.JOIN_REJECTED)).submit();
            case ZLinkActorJoinCompletion.Failed failed ->
                    CompletableFuture.failedFuture(
                            new IllegalStateException("Join failed: " + failed.kind()));
        };
    }
}
