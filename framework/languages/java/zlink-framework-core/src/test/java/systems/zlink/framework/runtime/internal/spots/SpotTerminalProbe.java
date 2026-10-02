package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Records calls at the transport and activation boundaries without inspecting runtime state. */
public final class SpotTerminalProbe
        implements SpotTransportAddressResolver, ZLinkInstanceSpotCallRuntime {
    public final SpotTransportAddress address =
            new SpotTransportAddress(
                    "router", RoutingId.from("owner"), "spot", 1L, ZLinkSpotKind.INSTANCE);
    public final RuntimeException terminal;
    private final boolean missing;
    public int resolves;
    public int invalidations;
    public int activations;
    public int submissions;
    public java.util.function.Function<Message, CompletionStage<Void>> readySend;
    public java.util.function.BiFunction<Message, Duration, CompletionStage<List<Message>>>
            readyRequest;
    public final java.util.ArrayList<UUID> operationIds = new java.util.ArrayList<>();

    public SpotTerminalProbe(RuntimeException terminal, boolean missing) {
        this.terminal = terminal;
        this.missing = missing;
    }

    @Override
    public CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId) {
        resolves++;
        return CompletableFuture.completedFuture(missing ? Optional.empty() : Optional.of(address));
    }

    @Override
    public void invalidate(String spotId) {
        invalidations++;
    }

    @Override
    public CompletionStage<Void> send(
            String spotId,
            String stableType,
            String meshName,
            Message payload,
            Optional<String> packetName,
            String contentType,
            Map<String, String> metadata) {
        if (!missing)
            return java.util.Objects.requireNonNull(readySend, "readySend").apply(payload);
        activations++;
        payload.close();
        return CompletableFuture.failedFuture(terminal);
    }

    @Override
    public CompletionStage<List<Message>> request(
            String spotId,
            String stableType,
            String meshName,
            Message payload,
            Optional<String> packetName,
            String contentType,
            Map<String, String> metadata,
            Duration timeout) {
        if (!missing)
            return java.util.Objects.requireNonNull(readyRequest, "readyRequest")
                    .apply(payload, timeout);
        activations++;
        payload.close();
        return CompletableFuture.failedFuture(terminal);
    }
}
