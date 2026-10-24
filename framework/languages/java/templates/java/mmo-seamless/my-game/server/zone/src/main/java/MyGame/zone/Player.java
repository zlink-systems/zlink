package MyGame.zone;

import MyGame.*;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final NodeConfig config;
    private EnterZone position = new EnterZone(0, 0);

    public Player(ZLinkActorContext context, NodeConfig config) {
        this.context = context;
        this.config = config;
    }

    public ZLinkActorContext context() {
        return context;
    }

    public EnterZone position() {
        return position;
    }

    public void apply(EnterZone position) {
        this.position = position;
    }

    public void checkIdentity(String playerId) {
        if (!context.actorId().equals(playerId))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public void move(EnterZone position) {
        context.joinSpot(Rules.zoneId(position.x()), position).defer();
    }

    public CompletionStage<Void> sendState() {
        return context.boundSession()
                .send(
                        new WorldState(
                                Rules.zoneId(position.x()),
                                config.id(),
                                position.x(),
                                position.sequence()))
                .submit();
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (!(completion instanceof ZLinkActorJoinCompletion.Accepted))
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Zone join failed: " + completion));
        return sendState();
    }
}
