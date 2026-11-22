package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.InstanceActivationObserverBackend;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class InstanceSpotPlacementDeadlineTest {
    private static final Duration TIMEOUT = Duration.ofMillis(300);
    private static final String MESH = "mesh";
    private static final String TYPE = "type";
    private static final String SPOT = "placement-deadline";

    @Test
    void sendWaitsForPlacementAndDeliversOriginalActivationDeadline() throws Exception {
        try (var fixture = new Fixture();
                var payload =
                        ZLinkFrameworkMultipartView.snapshot(
                                "{}".getBytes(StandardCharsets.UTF_8))) {
            long activationDeadline = fixture.calls.activationDeadline(MESH);
            var send =
                    fixture.calls
                            .send(
                                    SPOT,
                                    TYPE,
                                    MESH,
                                    payload,
                                    Optional.empty(),
                                    "application/json",
                                    Map.of(),
                                    activationDeadline,
                                    null)
                            .toCompletableFuture();
            assertEquals(
                    2, fixture.placement.lists.get(), "Send must re-read the empty observation");
            var afterDeadline =
                    CompletableFuture.runAsync(
                            () -> {},
                            CompletableFuture.delayedExecutor(
                                    Math.max(
                                            1, activationDeadline - System.currentTimeMillis() + 1),
                                    TimeUnit.MILLISECONDS));
            afterDeadline.get(5, TimeUnit.SECONDS);
            assertTrue(System.currentTimeMillis() >= activationDeadline);
            fixture.releasePlacement();

            send.get(5, TimeUnit.SECONDS);
            var envelope = fixture.received.get(5, TimeUnit.SECONDS);
            assertEquals(activationDeadline, envelope.deadlineUnixMs());
            assertEquals(SPOT, envelope.targetSpotId());
            assertEquals(fixture.target.rid(), envelope.targetNodeRid());
            assertFalse(envelope.request());
        }
    }

    @Test
    void requestDeadlineEndsPlacementWhileStoreRefreshIsPending() throws Exception {
        try (var fixture = new Fixture();
                var payload = Message.from("{}".getBytes(StandardCharsets.UTF_8))) {
            var request =
                    fixture.calls
                            .request(
                                    SPOT,
                                    TYPE,
                                    MESH,
                                    payload,
                                    Optional.empty(),
                                    "application/json",
                                    Map.of(),
                                    TIMEOUT,
                                    null,
                                    ignored -> CompletableFuture.completedFuture(Optional.empty()),
                                    new CompletableFuture<>())
                            .toCompletableFuture();
            assertEquals(
                    2, fixture.placement.lists.get(), "Request must re-read the empty observation");
            var failure =
                    assertThrows(ExecutionException.class, () -> request.get(5, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertFalse(fixture.page.isDone(), "Placement must time out with the Store page held");
            assertFalse(
                    fixture.received.isDone(), "The request must expire before target admission");
            fixture.releasePlacement();
        }
    }

    private static final class Fixture implements AutoCloseable {
        final ZLinkPlacementDiscoveryTest.Fixture placement =
                new ZLinkPlacementDiscoveryTest.Fixture(ZLinkPlacementObjectKind.INSTANCE_SPOT);
        final CompletableFuture<RecoveryEnvelope> received = new CompletableFuture<>();
        final CompletableFuture<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> page =
                new CompletableFuture<>();
        final ZLinkFrameworkRuntime runtime;
        final ZLinkInstanceSpotCallRuntime calls;
        final ZLinkMeshNodeDescriptor target;

        Fixture() throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            options.addRelocationStore(new InMemoryRelocationStore());
            options.addRouteMesh(MESH)
                    .listen("inproc://placement-deadline-" + UUID.randomUUID())
                    .setDefaultRequestTimeout(TIMEOUT)
                    .objects()
                    .server()
                    .addInstanceSpotFactory(
                            TYPE, Instance.class, factory -> factory.disableRelocation());
            runtime =
                    ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                            options,
                            new InstanceActivationObserverBackend(received::complete),
                            ZLinkHandlerActivator.reflection(),
                            null,
                            new ZLinkFrameworkRuntimeTestAccess.Construction()
                                    .spotLocationResolvers(placement.resolver));
            try {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                var spots = ZLinkFrameworkRuntimeTestAccess.spots(runtime);
                var localNode = ZLinkSpotRuntimeTestAccess.routeMeshNodesByName(spots).get(MESH);
                var localRid = localNode.status().routingId();
                target =
                        ZLinkFrameworkRuntimeTestAccess.discovery(runtime)
                                .listLiveMeshNodes(MESH)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS)
                                .stream()
                                .filter(node -> node.rid().equals(localRid))
                                .findFirst()
                                .orElseThrow();
                calls = spots.instanceSpotCalls();
                placement.ownerToken =
                        new ZLinkLocationOwnerToken(target.ownerId(), target.leaseGeneration());
                placement.deleted = true;
                placement.refresh();
                placement.heldPage = page;
            } catch (Exception | Error failure) {
                close();
                throw failure;
            }
        }

        void releasePlacement() {
            page.complete(new ZLinkLocationPage<>(List.of(target), null));
        }

        @Override
        public void close() {
            page.cancel(false);
            try {
                runtime.close();
            } finally {
                placement.close();
            }
        }
    }

    public record Instance(ZLinkInstanceSpotContext context) implements ZLinkInstanceSpot {}
}
