package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final PlayerData data;
    private final GoldBalance gold;

    public Player(ZLinkActorContext context, PlayerData data, long gold) {
        this.context = context;
        this.data = data;
        this.gold = new GoldBalance(gold);
    }

    public ZLinkActorContext context() {
        return context;
    }

    public long gold() {
        return gold.gold();
    }

    public GoldBalance goldBalance() {
        return gold;
    }

    public CompletionStage<ActionRejected> earn() {
        var decision = gold.previewEarn();
        if (!decision.accepted())
            return CompletableFuture.completedFuture(decision.rejection());
        return data.save(context.actorId(), decision.gold())
                .thenApply(
                        ignored -> {
                            gold.commit(decision);
                            return null;
                        });
    }

    public void restore(GoldBalance.Snapshot snapshot) {
        gold.restore(snapshot);
    }

    public void checkIdentity(String id) {
        if (!context.actorId().equals(id))
            throw new IllegalArgumentException("Player identity mismatch");
    }

    public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
        if (!(completion instanceof ZLinkActorJoinCompletion.Accepted))
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Zone join failed: " + completion));
        return CompletableFuture.completedFuture(null);
    }
}
