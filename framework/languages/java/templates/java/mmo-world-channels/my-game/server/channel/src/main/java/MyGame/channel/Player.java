package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class Player implements ZLinkActor {
    private final ZLinkActorContext context;
    private final PlayerData data;
    private long gold;

    public Player(ZLinkActorContext context, PlayerData data, long gold) {
        this.context = context;
        this.data = data;
        restore(gold);
    }

    public ZLinkActorContext context() {
        return context;
    }

    public long gold() {
        return gold;
    }

    public void restore(long value) {
        if (value < 0 || value > Names.MAX_GOLD) throw new IllegalArgumentException("Invalid gold");
        gold = value;
    }

    public CompletionStage<Void> earn(long amount) {
        if (amount <= 0 || amount > Names.MAX_GOLD - gold)
            throw new IllegalArgumentException("Invalid earning amount");
        long next = gold + amount;
        return data.save(context.actorId(), next).thenRun(() -> gold = next);
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
