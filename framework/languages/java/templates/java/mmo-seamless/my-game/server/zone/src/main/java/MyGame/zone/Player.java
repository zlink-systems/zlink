package MyGame.zone;

import MyGame.EnterZone;
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

    public void checkIdentity(String playerId) {
        if (!context.actorId().equals(playerId))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public void move(String zoneId, EnterZone arrival) {
        context.joinSpot(zoneId, arrival).defer();
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            if (accepted.reply().isEmpty()) return CompletableFuture.completedFuture(null);
            return context.boundSession().send(accepted.reply().decode(MyGame.WorldState.class)).submit();
        }
        if (completion instanceof ZLinkActorJoinCompletion.Rejected rejected) {
            if (rejected.reply().isEmpty())
                return CompletableFuture.failedFuture(
                        new IllegalStateException("Zone join failed: " + completion));
            return context.boundSession()
                    .send(rejected.reply().decode(MyGame.ActionRejected.class))
                    .submit();
        }
        return CompletableFuture.failedFuture(
                new IllegalStateException("Zone join failed: " + completion));
    }
}
