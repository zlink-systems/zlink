package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkActorCreateBeforeOwnerLeaseAdmissionTest {
    // Location runtime spec §5, lines 740–766: closed admission rejects new operations.
    @Test
    void actorCreateBeforeInitialOwnerLeaseIsRecordedIsRejectedUntilStartupCompletes()
            throws Exception {
        var store = new InitialOwnerLeaseGate();
        var options = optionsWithSpotNodeAndActorFactory(store);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var startup =
                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                            .toCompletableFuture();
            try {
                store.claimReached.get(2, TimeUnit.SECONDS);
                assertFalse(startup.isDone());
                assertFalse(runtime.isReady());
                assertFalse(ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(runtime));

                var error =
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () -> runtime.actorManager().create("player-1", "player").submit());
                assertTrue(ZLinkLocationRuntime.isExecutionPaused(error));
                assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, error.kind());
                assertEquals("The owner lease admission deadline has expired.", error.getMessage());
                assertFalse(startup.isDone());
            } finally {
                store.releaseClaim.complete(null);
            }

            startup.get(10, TimeUnit.SECONDS);
            assertTrue(runtime.isReady());
            assertTrue(ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(runtime));
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            runtime.actorManager()
                                    .create("player-1", "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(10, TimeUnit.SECONDS));
            assertEquals("player-1", created.actor().actorId());
        }
    }

    @Test
    void hostWaitsForInitialOwnerLeaseBeforeActorCreation() throws Exception {
        var store = new InitialOwnerLeaseGate();
        var options = optionsWithSpotNodeAndActorFactory(store);
        var host =
                CompletableFuture.supplyAsync(
                        () ->
                                ZLinkFrameworkRuntimeTestAccess.startHost(
                                        options, new ZLinkJavaBackendAdapterFactory()));
        try {
            store.claimReached.get(2, TimeUnit.SECONDS);
            assertFalse(host.isDone());
        } finally {
            store.releaseClaim.complete(null);
        }
        try (var runtime = host.get(10, TimeUnit.SECONDS)) {
            assertTrue(runtime.isReady());
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    runtime.actorManager()
                            .create("player-1", "player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
        }
    }

    private static final class InitialOwnerLeaseGate implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final CompletableFuture<Void> claimReached = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseClaim = new CompletableFuture<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (request.mutations().getFirst() instanceof ZLinkStorePut put
                    && put.key().value().startsWith("owner-lease\0")
                    && claimReached.complete(null)) {
                return releaseClaim.thenCompose(ignored -> inner.write(request, cancellation));
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    private static DefaultZLinkFrameworkOptions optionsWithSpotNodeAndActorFactory() {
        return optionsWithSpotNodeAndActorFactory(new ZLinkInMemoryLocationStore());
    }

    private static DefaultZLinkFrameworkOptions optionsWithSpotNodeAndActorFactory(
            ZLinkLocationStore store) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh =
                options.addRouteMesh("game")
                        .listen("inproc://play-router")
                        .setRoutingIdPrefix("play-node");
        mesh.channelName("game").server();
        mesh.objects()
                .server()
                .addSpotFactory(
                        "game",
                        NodesAndServicesTest.GameSpot.class,
                        factory -> factory.disableRelocation())
                .addEntrySpot(NodesAndServicesTest.ActorCreationEntry.class)
                .addActorFactory(
                        "player",
                        NodesAndServicesTest.PlayerActor.class,
                        NodesAndServicesTest.PlayerActorFactory.class,
                        factory -> factory.disableRelocation());
        return options;
    }
}
