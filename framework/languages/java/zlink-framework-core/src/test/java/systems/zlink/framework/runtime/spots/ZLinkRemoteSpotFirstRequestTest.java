package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/** #1710: fresh requester READY and Existing lookup are not peer readiness barriers. */
final class ZLinkRemoteSpotFirstRequestTest {
    static final AtomicInteger CALLS = new AtomicInteger();

    @Test
    void newObjectClientRequestsExistingRemoteSpotWithoutPeerWarmup() throws Exception {
        CALLS.set(0);
        var locations = new ZLinkInMemoryLocationStore();
        var monotonicNanos = new AtomicLong();
        var fencingMargin = Duration.ofNanos(1);
        var stores =
                ZLinkRegisteredLocationStores.fromUnified(
                        new ZLinkProviderLocationRepository(
                                locations, monotonicNanos::get, fencingMargin));
        try (var owner = start(options(locations, "owner", true), stores)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(owner)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            monotonicNanos.set(fencingMargin.multipliedBy(3).toNanos());
            assertTrue(
                    ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(owner)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS));
            assertTrue(owner.isReady(), "fixed-RID owner must be serving before remote lookup");
            var created =
                    owner.spotManager()
                            .getOrCreate("first-request-spot", "echo")
                            .inMesh("first-request")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            try (var requester = start(options(locations, "requester", false), stores)) {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(requester)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                monotonicNanos.set(fencingMargin.multipliedBy(6).toNanos());
                assertTrue(
                        ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(requester)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS));
                assertTrue(
                        requester.isReady(),
                        "fixed-RID requester must be serving before the first remote request");
                var existing =
                        requester
                                .spotManager()
                                .getOrCreate("first-request-spot", "echo")
                                .inMesh("first-request")
                                .submit()
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);
                assertEquals(ZLinkSpotCreateState.EXISTING, existing.state());
                assertEquals(created.spot(), existing.spot());
                var terminal =
                        requester
                                .route()
                                .requestToSpot("first-request-spot", new Probe("first"))
                                .inMesh("first-request")
                                .timeout(Duration.ofSeconds(5))
                                .submit(String.class)
                                .handle(Reply::new)
                                .toCompletableFuture()
                                .get(6, TimeUnit.SECONDS);
                assertNull(
                        terminal.failure(), "first remote request failed: " + terminal.failure());
                assertEquals("first", terminal.value());
                assertEquals(1, CALLS.get(), "first logical request reaches the handler once");
            }
        }
    }

    private static DefaultZLinkFrameworkOptions options(
            ZLinkInMemoryLocationStore locations, String rid, boolean server) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addLocationStore(locations);
        var mesh =
                options.addRouteMesh("first-request")
                        .setRoutingId(RoutingId.from(rid))
                        .listen("tcp://127.0.0.1:0");
        if (server)
            mesh.objects()
                    .server()
                    .addSpotFactory("echo", Echo.class, factory -> factory.disableRelocation());
        else mesh.objects().client();
        return options;
    }

    private static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkRegisteredLocationStores stores) {
        return ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                options,
                new systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory(),
                systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                        .reflection(),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction().locationStores(stores));
    }

    private record Reply(String value, Throwable failure) {}

    public record Probe(String value) {}

    public static final class Echo implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Echo(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {
            context.handlers().addHandler(EchoHandler.class);
        }
    }

    public static final class EchoHandler implements ZLinkSpotRequestHandler<Echo, Probe, String> {
        @Override
        public CompletionStage<String> handle(Echo spot, Probe request) {
            CALLS.incrementAndGet();
            return CompletableFuture.completedFuture(request.value());
        }
    }
}
