package MyGame.table;

import MyGame.*;

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

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        return switch (completion) {
            case ZLinkActorJoinCompletion.Accepted accepted ->
                    CompletableFuture.completedFuture(null);
            case ZLinkActorJoinCompletion.Rejected rejected ->
                    context.boundSession()
                            .send(rejected.reply().decode(ActionRejected.class))
                            .submit();
            case ZLinkActorJoinCompletion.Failed failed ->
                    CompletableFuture.failedFuture(
                            new IllegalStateException("Join failed: " + failed.kind()));
        };
    }
}
