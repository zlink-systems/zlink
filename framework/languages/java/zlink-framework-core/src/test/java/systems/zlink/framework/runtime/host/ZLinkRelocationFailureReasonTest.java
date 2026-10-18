package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Host relocation spec 30 §13 (#1882): {@code StoreUnavailable} belongs to a Store read, write or
 * owner-lease failure only. A Store read that succeeds but names an authority the source does not
 * own is not a Store failure.
 */
final class ZLinkRelocationFailureReasonTest {
    private static final String MESH = "failure-reason";
    private static final Duration WAIT = Duration.ofSeconds(15);

    enum Fault {
        /** The provider read fails: a Store failure. */
        STORE_READ_FAILS(ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE),
        STORE_TYPED_UNAVAILABLE_READ_FAILS(ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE),
        /** The provider read succeeds without the source authority: not a Store failure. */
        AUTHORITY_NOT_ON_SOURCE(ZLinkFrameworkRelocationReason.RELOCATION_FAILED);

        final ZLinkFrameworkRelocationReason expected;

        Fault(ZLinkFrameworkRelocationReason expected) {
            this.expected = expected;
        }
    }

    @ParameterizedTest
    @EnumSource(Fault.class)
    void onlyAStoreFailureIsStoreUnavailable(Fault fault) throws Exception {
        var inner = new ZLinkInMemoryLocationStore();
        var inject = new AtomicBoolean();
        var authority = ZLinkAuthorityKeyCodec.decode(ZLinkAuthorityKeyCodec.spot("room"));
        var roomAuthority =
                new ZLinkStoreKey("authority\0" + authority.kind() + "\0" + authority.id());
        var sourceStore =
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, arguments) -> {
                                    if (inject.get()
                                            && method.getName().equals("read")
                                            && roomAuthority.equals(arguments[0])) {
                                        return fault == Fault.STORE_TYPED_UNAVAILABLE_READ_FAILS
                                                ? CompletableFuture.failedFuture(
                                                        new systems.zlink.framework.errors
                                                                .ZLinkFrameworkException(
                                                                systems.zlink.framework.errors
                                                                        .ZLinkFrameworkErrorKind
                                                                        .UNAVAILABLE,
                                                                "injected typed Store outage"))
                                                : fault == Fault.STORE_READ_FAILS
                                                        ? CompletableFuture.failedFuture(
                                                                new IOException(
                                                                        "injected read failure"))
                                                        : CompletableFuture.completedFuture(
                                                                new ZLinkStoreReadMissing(
                                                                        Instant.now()));
                                    }
                                    try {
                                        return method.invoke(inner, arguments);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        var relocations = new InMemoryRelocationStore();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options("source", sourceStore, relocations, null))) {
            source.spotManager()
                    .getOrCreate("room", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            String endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var target =
                            ZLinkFrameworkRuntimeTestAccess.start(
                                    options("target", inner, relocations, endpoint));
                    var sourceReady = new Ready(source.routeMeshRuntime().observe(MESH, 8));
                    var targetReady = new Ready(target.routeMeshRuntime().observe(MESH, 8))) {
                sourceReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                targetReady.result.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                inject.set(true);
                var result =
                        source.relocate(
                                        new ZLinkFrameworkRelocationOptions(
                                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                                null,
                                                WAIT))
                                .toCompletableFuture()
                                .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                inject.set(false);
                assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
                assertEquals(fault.expected, result.reason());
                assertEquals(ZLinkFrameworkRuntimeState.SERVING, source.status().state());
            }
        }
    }

    private static DefaultZLinkFrameworkOptions options(
            String rid,
            ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        var node =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        node.objects().server().addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
        return options;
    }

    private static final class Ready
            implements Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>, AutoCloseable {
        final CompletableFuture<Void> result = new CompletableFuture<>();
        Flow.Subscription subscription;

        Ready(Flow.Publisher<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> publisher) {
            publisher.subscribe(this);
        }

        @Override
        public void onSubscribe(Flow.Subscription subscription) {
            this.subscription = subscription;
            subscription.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> value) {
            if (value.status().isReady() && value.status().readyPeerCount() > 0)
                result.complete(null);
        }

        @Override
        public void onError(Throwable failure) {
            result.completeExceptionally(failure);
        }

        @Override
        public void onComplete() {}

        @Override
        public void close() {
            subscription.cancel();
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void configure() {}
    }
}
