package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.spots.ZLinkRemoteSpotRequestFixture.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.UnaryOperator;

final class ZLinkDelayedSpotOwnerDiscoveryTest {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration TERMINAL_LIMIT = Duration.ofSeconds(6);

    private final AtomicBoolean discoveryReleased = new AtomicBoolean();
    private final AtomicBoolean requesterHiddenFromOwner = new AtomicBoolean();

    @Test
    void firstRequestTerminatesWhileOwnerDiscoveryIsDelayedAndNextRequestSucceeds()
            throws Exception {
        try (var fixture =
                new ZLinkRemoteSpotRequestFixture(
                        this::delayOwnerDiscovery, UnaryOperator.identity())) {
            long started = System.nanoTime();
            var terminal =
                    fixture.request("first", REQUEST_TIMEOUT)
                            .handle(
                                    (reply, failure) ->
                                            new Terminal(
                                                    reply, failure, System.nanoTime() - started))
                            .toCompletableFuture();
            var first =
                    terminal.get(
                            TERMINAL_LIMIT.toNanos() - (System.nanoTime() - started),
                            TimeUnit.NANOSECONDS);
            assertTrue(first.elapsedNanos() < TERMINAL_LIMIT.toNanos());
            if (first.failure() == null) {
                assertTrue(first.elapsedNanos() < REQUEST_TIMEOUT.toNanos());
                assertEquals("first", first.reply());
                assertEquals(1, ZLinkRemoteSpotFirstRequestTest.CALLS.get());
            } else {
                Throwable failure = first.failure();
                while (failure instanceof CompletionException && failure.getCause() != null) {
                    failure = failure.getCause();
                }
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
                assertEquals(0, ZLinkRemoteSpotFirstRequestTest.CALLS.get());
            }
            assertTrue(requesterHiddenFromOwner.get());
            int firstCalls = ZLinkRemoteSpotFirstRequestTest.CALLS.get();
            discoveryReleased.set(true);
            assertEquals("next", await(fixture.request("next", REQUEST_TIMEOUT)));
            assertEquals(firstCalls + 1, ZLinkRemoteSpotFirstRequestTest.CALLS.get());
        }
    }

    private ZLinkLocationRepository delayOwnerDiscovery(ZLinkLocationRepository store) {
        return intercept(
                store,
                "listMeshNodes",
                args ->
                        discoveryReleased.get() || !MESH.equals(args[0])
                                ? null
                                : listWithoutRequester(store, (ZLinkPageRequest) args[1]));
    }

    private CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listWithoutRequester(
            ZLinkLocationRepository store, ZLinkPageRequest request) {
        return store.listMeshNodes(MESH, request)
                .thenApply(
                        page -> {
                            var visible =
                                    page.items().stream()
                                            .filter(node -> !node.rid().equals(REQUESTER_RID))
                                            .toList();
                            if (visible.size() != page.items().size())
                                requesterHiddenFromOwner.set(true);
                            return new ZLinkLocationPage<>(visible, page.continuationToken());
                        });
    }

    private record Terminal(String reply, Throwable failure, long elapsedNanos) {}
}
