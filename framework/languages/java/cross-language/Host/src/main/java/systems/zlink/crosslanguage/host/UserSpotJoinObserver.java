package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;

/** Signals the target-side runner that the foreign actor finished joining the
 * fixed User Spot, mirroring the .NET TestHost's UserSpotJoinObserver. */
public final class UserSpotJoinObserver {
    private final CompletableFuture<Void> joined = new CompletableFuture<>();

    public CompletableFuture<Void> joined() {
        return joined;
    }

    public void complete() {
        joined.complete(null);
    }
}
