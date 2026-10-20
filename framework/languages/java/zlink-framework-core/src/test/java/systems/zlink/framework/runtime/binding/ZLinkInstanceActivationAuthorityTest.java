package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.service.*;

import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BiFunction;

final class ZLinkInstanceActivationAuthorityTest {
    @ParameterizedTest
    @CsvSource({"false,0", "false,7", "false,8", "true,0", "true,7", "true,8"})
    @SuppressWarnings("unchecked")
    void activationKeepsReservedFenceAfterCloseCleanup(boolean recovery, long successorGeneration)
            throws Exception {
        boolean replacement = successorGeneration != 0;
        try (var context = Zlink.createContext();
                var owner = new ZLinkJavaRawMeshNode(context, "activation-authority")) {
            owner.setRoutingId(RoutingId.from("activation-authority"));
            var target = (ZLinkJavaRawSpotNode) owner.spotNode();
            var route = fence(owner, 7, 11);
            var successor = fence(owner, replacement ? successorGeneration : 8, 12);
            var observed = new CompletableFuture<ZLinkServiceM6BWireCodec.InstanceRouteFence>();
            var delivered = new CompletableFuture<Void>();
            target.setInstanceSpotActivationValidator(
                    envelope ->
                            CompletableFuture.completedFuture(
                                    new ZLinkAuthorityMissing(Instant.now())));
            target.registerInstanceSpotType(
                    "room",
                    new ZLinkInternalMeshNode.InstanceSpotActivationHandler() {
                        public CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                                ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                                systems.zlink.framework.runtime.internal.locations
                                                .ZLinkAuthorityReadResult
                                        authority) {
                            return CompletableFuture.completedFuture(route);
                        }

                        public CompletionStage<Void> activate(
                                String type,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence reserved,
                                ZLinkBackendSpot spot) {
                            observed.complete(reserved);
                            spot.onDispatchEvent(
                                    new ZLinkInternalAsyncSpotDispatchHandler() {
                                        public CompletionStage<Void> handleAsync(
                                                ZLinkBackendSpotDispatchInfo info) {
                                            return CompletableFuture.completedFuture(null);
                                        }

                                        public CompletionStage<Void> handleRoute(
                                                ZLinkBackendReceived received,
                                                CompletableFuture<Void> admission) {
                                            if (admission != null) admission.complete(null);
                                            received.close();
                                            delivered.complete(null);
                                            return CompletableFuture.completedFuture(null);
                                        }
                                    });
                            return CompletableFuture.completedFuture(null);
                        }
                    });
            // Run the existing close cleanup after reservation and before the activation hook.
            // The factory still creates the real backend Spot on the normal activation path.
            var registryField = ZLinkJavaRawSpotNode.class.getDeclaredField("instanceSpots");
            registryField.setAccessible(true);
            var registry = registryField.get(target);
            var factoriesField = ZLinkJavaInstanceSpotRegistry.class.getDeclaredField("factories");
            factoriesField.setAccessible(true);
            var factories = (Map<String, Object>) factoriesField.get(registry);
            var registration = factories.get("room");
            var components = registration.getClass().getRecordComponents();
            var constructor =
                    registration
                            .getClass()
                            .getDeclaredConstructor(
                                    java.util.Arrays.stream(components)
                                            .map(java.lang.reflect.RecordComponent::getType)
                                            .toArray(Class<?>[]::new));
            constructor.setAccessible(true);
            Object[] values = new Object[components.length];
            for (int index = 0; index < components.length; index++) {
                var accessor = components[index].getAccessor();
                accessor.setAccessible(true);
                values[index] = accessor.invoke(registration);
                if (components[index].getName().equals("factory")) {
                    var factory = (BiFunction<String, Long, ZLinkBackendSpot>) values[index];
                    values[index] =
                            (BiFunction<String, Long, ZLinkBackendSpot>)
                                    (id, generation) -> {
                                        var spot = factory.apply(id, generation);
                                        if (replacement)
                                            owner.registerInstanceIntent("room", successor);
                                        owner.forgetInstanceIntent(route);
                                        return spot;
                                    };
                }
            }
            factories.put("room", constructor.newInstance(values));
            var failure = new CompletableFuture<Throwable>();
            try (var message = Message.from(new byte[] {1})) {
                if (recovery) {
                    var envelope =
                            new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                                    "spot",
                                    "room",
                                    owner.name(),
                                    owner.routingId(),
                                    1,
                                    "1",
                                    owner.routingId(),
                                    1,
                                    Optional.empty(),
                                    false,
                                    1,
                                    2,
                                    null,
                                    Long.MAX_VALUE,
                                    new byte[0],
                                    ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(
                                            List.of(message)));
                    target.recoverInstanceSpot(envelope, route)
                            .whenComplete(
                                    (ignored, error) -> {
                                        if (error != null) failure.complete(error);
                                    });
                } else {
                    var header =
                            new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                                    0,
                                    new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                                            owner.routingId(),
                                            1,
                                            "spot",
                                            owner.name(),
                                            "room",
                                            "1",
                                            Long.MAX_VALUE),
                                    true,
                                    1,
                                    owner.routingId(),
                                    null,
                                    false,
                                    1,
                                    2,
                                    null);
                    assertTrue(
                            target.enqueueRemoteInstanceSpot(
                                    owner.routingId(),
                                    header,
                                    new byte[0],
                                    List.of(Message.from(new byte[] {1})),
                                    null,
                                    reply -> reply.forEach(Message::close),
                                    failure::complete));
                }
                CompletableFuture.anyOf(observed, failure).get(5, TimeUnit.SECONDS);
                assertFalse(failure.isDone(), () -> "Activation failed: " + failure.join());
                assertSame(route, observed.join());
                delivered.get(5, TimeUnit.SECONDS);
                var authoritiesField =
                        ZLinkJavaRawSpotNode.class.getDeclaredField("instanceAuthorities");
                authoritiesField.setAccessible(true);
                var authorities = (Map<?, ?>) authoritiesField.get(target);
                assertEquals(replacement ? 1 : 0, authorities.size());
                if (replacement) {
                    target.forgetInstanceSpotAuthority(successor);
                    assertTrue(
                            authorities.isEmpty(),
                            "old close cleanup replaced the next reservation");
                }
            }
        }
    }

    private static ZLinkServiceM6BWireCodec.InstanceRouteFence fence(
            ZLinkJavaRawMeshNode owner, long generation, long authorityGeneration) {
        return new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                owner.routingId(),
                1,
                "spot",
                generation,
                "owner",
                authorityGeneration,
                1,
                "version");
    }
}
