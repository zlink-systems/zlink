package MyGame;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final NodeConfig config;
    private EnterField position = new EnterField(0, 0);

    public Player(ZLinkActorContext context, NodeConfig config) {
        this.context = context;
        this.config = config;
    }

    public ZLinkActorContext context() {
        return context;
    }

    public EnterField position() {
        return position;
    }

    public void apply(EnterField position) {
        this.position = position;
    }

    public void checkIdentity(String playerId) {
        if (!context.actorId().equals(playerId))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public void move(EnterField position) {
        context.joinSpot(Rules.fieldId(position.x()), position).defer();
    }

    public CompletionStage<Void> sendState() {
        return context.boundSession()
                .send(
                        new WorldState(
                                Rules.fieldId(position.x()),
                                config.id(),
                                position.x(),
                                position.sequence()))
                .submit();
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (!(completion instanceof ZLinkActorJoinCompletion.Accepted))
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Field join failed: " + completion));
        return context.spotId().equals(Rules.fieldId(position.x()))
                ? sendState()
                : CompletableFuture.completedFuture(null);
    }
}
