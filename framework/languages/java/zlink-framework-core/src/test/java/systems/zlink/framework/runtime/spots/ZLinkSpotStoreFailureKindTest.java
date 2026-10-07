package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotStoreFailureKindTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void cacheMissSpotOperationClassifiesProviderTimeoutAsUnavailable(boolean request) {
        var reads = new AtomicInteger();
        var provider =
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("read")) {
                                        reads.incrementAndGet();
                                        return CompletableFuture.failedFuture(
                                                new TimeoutException(
                                                        "private Redis command/key details"));
                                    }
                                    throw new AssertionError(
                                            "unexpected SPI operation: " + method.getName());
                                });
        try (var rows =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkProviderLocationRepository(provider)),
                        new ZLinkLocationOptions())) {
            SpotTransportAddressResolver resolver =
                    id -> rows.resolveSpot(id).thenApply(ignored -> Optional.empty());
            var outbound =
                    new DefaultSpotOutbound(
                            null,
                            "mesh",
                            null,
                            new ZLinkJsonMessageSerializer(),
                            ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                            null,
                            null,
                            null,
                            null,
                            false,
                            Duration.ofMinutes(1),
                            () -> resolver,
                            null);
            var pending =
                    request
                            ? outbound.requestToSpot("uncached-spot", new Packet("read"))
                                    .submit(String.class)
                            : outbound.sendToSpot("uncached-spot", new Packet("read")).submit();
            var failure =
                    assertThrows(
                            CompletionException.class, () -> pending.toCompletableFuture().join());
            var error = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind());
            assertInstanceOf(TimeoutException.class, error.getCause());
            assertFalse(error.getMessage().contains("Redis"));
            assertEquals(1, reads.get());
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(strings = {"read", "write", "scan"})
    void repositoryClassifiesEverySpiOperationAndRetainsCause(String operation) {
        var cause = new TimeoutException("private provider details");
        var provider =
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, arguments) ->
                                        method.getName().equals(operation)
                                                ? CompletableFuture.failedFuture(cause)
                                                : CompletableFuture.completedFuture(
                                                        new systems.zlink.framework.locationprovider
                                                                .ZLinkStoreReadMissing(
                                                                java.time.Instant.now())));
        var boundary = new ZLinkProviderLocationRepository(provider);
        var pending =
                switch (operation) {
                    case "read" ->
                            boundary.read(
                                    systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec
                                            .spot("key"),
                                    () -> false);
                    case "write" -> boundary.claimOwnerLease("owner", Duration.ofSeconds(30));
                    case "scan" -> boundary.list("prefix", Optional.empty(), 1, () -> false);
                    default -> throw new AssertionError(operation);
                };
        var terminal =
                assertThrows(CompletionException.class, () -> pending.toCompletableFuture().join());
        var error = assertInstanceOf(ZLinkFrameworkException.class, terminal.getCause());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind());
        assertSame(cause, error.getCause());
        assertFalse(error.getMessage().contains("private"));
    }

    @org.junit.jupiter.api.Test
    void repositoryPreservesTypedValidationAndCancellationFailures() {
        for (var cause :
                List.of(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "typed"),
                        new IllegalArgumentException("validation"),
                        new CancellationException("caller"))) {
            var provider =
                    (ZLinkLocationStore)
                            Proxy.newProxyInstance(
                                    ZLinkLocationStore.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationStore.class},
                                    (proxy, method, arguments) ->
                                            CompletableFuture.failedFuture(cause));
            var pending =
                    new ZLinkProviderLocationRepository(provider)
                            .read(
                                    systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec
                                            .spot("key"),
                                    () -> cause instanceof CancellationException);
            var terminal =
                    assertThrows(
                            CompletionException.class, () -> pending.toCompletableFuture().join());
            assertSame(cause, terminal.getCause());
        }
    }

    private record Packet(String value) {}
}
