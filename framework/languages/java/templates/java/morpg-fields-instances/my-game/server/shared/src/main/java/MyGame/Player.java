package MyGame;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final NodeConfig config;
    private EnterField position = new EnterField(0, 0);
    private String partyId;

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

    public String partyId() {
        return partyId;
    }

    public void setPartyId(String partyId) {
        this.partyId = partyId;
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

    public PlayerSnapshot capture() {
        return new PlayerSnapshot(position.x(), position.sequence(), partyId);
    }

    public void restore(PlayerSnapshot state) {
        apply(new EnterField(state.x(), state.sequence()));
        partyId = state.partyId();
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (!(completion instanceof ZLinkActorJoinCompletion.Accepted))
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Field join failed: " + completion));
        return Rules.fieldId(position.x()).equals(context.spotId().orElse(null))
                ? sendState()
                : CompletableFuture.completedFuture(null);
    }

    public record PlayerSnapshot(double x, long sequence, String partyId) {}
}
