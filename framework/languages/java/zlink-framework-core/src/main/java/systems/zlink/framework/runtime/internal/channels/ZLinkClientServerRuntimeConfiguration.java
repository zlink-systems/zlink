package systems.zlink.framework.runtime.internal.channels;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;

/**
 * Internal host-to-channel dependency for the ClientServer location runtime.
 */
public final class ZLinkClientServerRuntimeConfiguration {
    private final ZLinkLocationRepository store;
    private final ZLinkLocationOptions options;
    private volatile ZLinkLocationOwnerToken owner;
    private Lifecycle lifecycle;

    public ZLinkClientServerRuntimeConfiguration(
        ZLinkLocationRepository store,
        ZLinkLocationOptions options) {
        this.store = store;
        this.options = Objects.requireNonNull(options, "options");
    }

    public ZLinkLocationRepository store() {
        return store;
    }

    public Supplier<ZLinkLocationOwnerToken> owner() {
        return () -> {
            ZLinkLocationOwnerToken current = owner;
            if (current == null) {
                throw new IllegalStateException(
                    "ClientServer owner lease is not ready");
            }
            return current;
        };
    }

    public void setOwner(ZLinkLocationOwnerToken value) {
        owner = Objects.requireNonNull(value, "value");
    }

    public ZLinkLocationOptions options() {
        return options;
    }

    public synchronized void install(Lifecycle value) {
        if (lifecycle != null) {
            throw new IllegalStateException(
                "ClientServer runtime lifecycle is already installed");
        }
        lifecycle = Objects.requireNonNull(value, "value");
    }

    public CompletionStage<Void> start() {
        Lifecycle current = lifecycle;
        return current == null
            ? CompletableFuture.completedFuture(null)
            : current.start();
    }

    public CompletionStage<Void> markDraining() {
        Lifecycle current = lifecycle;
        return current == null
            ? CompletableFuture.completedFuture(null)
            : current.markDraining();
    }

    public CompletionStage<Void> stop() {
        Lifecycle current = lifecycle;
        return current == null
            ? CompletableFuture.completedFuture(null)
            : current.stop();
    }

    public interface Lifecycle {
        CompletionStage<Void> start();

        CompletionStage<Void> markDraining();

        CompletionStage<Void> stop();
    }
}
