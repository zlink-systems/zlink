package MyGame.region.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final ZLinkActorClient actors;

    public Player(ZLinkActorContext context, ZLinkActorClient actors) {
        this.context = context;
        this.actors = actors;
    }

    public ZLinkActorContext context() {
        return context;
    }

    public void checkIdentity(String id) {
        if (!context.actorId().equals(id))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        return switch (completion) {
            case ZLinkActorJoinCompletion.Accepted accepted ->
                    CompletableFuture.completedFuture(null);
            // The current Entry owns selection after a capacity rejection.
            case ZLinkActorJoinCompletion.Rejected rejected ->
                    actors.sendToActor(context.actorId(), rejected.reply().decode(RegionFull.class))
                            .submit();
            case ZLinkActorJoinCompletion.Failed failed ->
                    CompletableFuture.failedFuture(
                            new IllegalStateException("Region join failed: " + failed.kind()));
        };
    }
}
