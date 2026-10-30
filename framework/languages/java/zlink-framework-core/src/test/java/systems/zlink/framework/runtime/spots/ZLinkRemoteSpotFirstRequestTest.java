package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** #1710: fresh requester READY and Existing lookup are not peer readiness barriers. */
final class ZLinkRemoteSpotFirstRequestTest {
    static final AtomicInteger CALLS = new AtomicInteger();

    @Test
    void newObjectClientRequestsExistingRemoteSpotWithoutPeerWarmup() throws Exception {
        CALLS.set(0);
        var locations = new ZLinkInMemoryLocationStore();
        try (var owner = ZLinkFrameworkRuntimeTestAccess.start(options(locations, "owner", true))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(owner)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var created =
                    owner.spotManager()
                            .getOrCreate("first-request-spot", "echo")
                            .inMesh("first-request")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            try (var requester =
                    ZLinkFrameworkRuntimeTestAccess.start(options(locations, "requester", false))) {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(requester)
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
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
