package systems.zlink.e2e.registrymessaging.provider.Infrastructure;

import java.util.concurrent.CompletableFuture;

/** Publicly controlled barrier for the RM-B3 in-flight request fixture. */
public final class ProfileGate {
    private volatile CompletableFuture<Void> release = new CompletableFuture<>();

    public void reset() {
        release = new CompletableFuture<>();
    }

    public void open() {
        release.complete(null);
    }

    public void await() {
        release.join();
    }
}
