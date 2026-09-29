package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkRelocationHandOffTest {
    @Test
    void sourcePreserveSettlesRetainedWorkWhileRelayAdmissionIsPending() {
        var relayAdmission = new CompletableFuture<Void>();
        var settleCalls = new AtomicInteger();
        var result =
                ZLinkRelocationHandOff.run(
                        client(settleCalls),
                        request(),
                        () -> relayAdmission,
                        Duration.ofSeconds(1),
                        Instant.now());

        assertEquals(1, settleCalls.get());
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                result.toCompletableFuture().getNow(null));
    }

    @Test
    void sourcePreserveSettlesRetainedWorkWhenRelayAdmissionThrows() {
        var settleCalls = new AtomicInteger();
        var result =
                ZLinkRelocationHandOff.run(
                        client(settleCalls),
                        request(),
                        () -> {
                            throw new IllegalStateException("relay admission rejected");
                        },
                        Duration.ofSeconds(1),
                        Instant.now());

        assertEquals(1, settleCalls.get());
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                result.toCompletableFuture().getNow(null));
    }

    private static ZLinkSpotRetireControl.StageRequest request() {
        RoutingId source = RoutingId.from("source");
        RoutingId target = RoutingId.from("target");
        var fence = new ZLinkSpotRetireControl.Fence(UUID.randomUUID(), 1);
        return new ZLinkSpotRetireControl.StageRequest(
                fence,
                source,
                1,
                "source-owner",
                1,
                target,
                1,
                "target-owner",
                1,
                "mesh",
                "actor",
                "player",
                false,
                true,
                new byte[] {1},
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                "actor", 1, "actor", "player", true, 1, 1)));
    }

    private static ZLinkRelocationTransitionClient client(AtomicInteger settleCalls) {
        return new ZLinkRelocationTransitionClient() {
            @Override
            public CompletionStage<Void> stage(
                    RoutingId rid, ZLinkSpotRetireControl.StageRequest value, Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> relay(
                    RoutingId rid,
                    ZLinkSpotRetireControl.Fence value,
                    byte[] record,
                    Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> publish(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Settlement> settle(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Instant preserveAt) {
                settleCalls.incrementAndGet();
                return CompletableFuture.completedFuture(Settlement.SOURCE_PRESERVED);
            }

            @Override
            public CompletionStage<Void> abort(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }
        };
    }
}
