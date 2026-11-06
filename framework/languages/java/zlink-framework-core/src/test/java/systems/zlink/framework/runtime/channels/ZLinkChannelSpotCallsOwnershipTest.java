package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkChannelSpotCallsOwnershipTest {
    @Test
    void spotSendClosesRetainedPayloadAndEnvelopeOnSuccessOneHundredTimes() {
        assertSpotSendCloses(true);
    }

    @Test
    void spotSendClosesRetainedPayloadAndEnvelopeOnFailureOneHundredTimes() {
        assertSpotSendCloses(false);
    }

    @Test
    void spotRequestFailureClosesRetainedPayloadAndEnvelopeOneHundredTimes() {
        for (int iteration = 0; iteration < 100; iteration++) {
            AtomicReference<List<Message>> submitted = new AtomicReference<>();
            ZLinkChannelCallRuntime runtime =
                    new ZLinkChannelCallRuntime(
                            flow(),
                            null,
                            null,
                            (channel,
                                    node,
                                    spot,
                                    generation,
                                    authority,
                                    lease,
                                    parts,
                                    timeout,
                                    registry,
                                    operationId,
                                    admission) -> {
                                submitted.set(parts);
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("request failure"));
                            });
            SpotTransportAddressResolver resolver = resolver();
            ZLinkFrameworkMultipartView payload =
                    ZLinkFrameworkMultipartView.snapshot(new byte[] {1, 2, 3});
            RouteSpotRequestCall call =
                    new RouteSpotRequestCall(
                            runtime,
                            "channel",
                            "mesh",
                            resolver,
                            null,
                            "spot",
                            payload,
                            Optional.of("Packet"),
                            Duration.ofSeconds(1),
                            "application/json",
                            null,
                            null,
                            false,
                            null,
                            null,
                            systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata
                                    .empty());

            try {
                assertThrows(
                        CompletionException.class,
                        () -> call.submit(String.class).toCompletableFuture().join());
                assertClosed(payload, submitted.get());
            } finally {
                if (submitted.get() != null) {
                    ZLinkFrameworkMultipartView.closeAll(submitted.get());
                }
                payload.close();
                runtime.beginClose();
            }
        }
    }

    private static void assertSpotSendCloses(boolean success) {
        for (int iteration = 0; iteration < 100; iteration++) {
            AtomicReference<List<Message>> submitted = new AtomicReference<>();
            CompletionStage<Void> terminal =
                    success
                            ? CompletableFuture.completedFuture(null)
                            : CompletableFuture.failedFuture(
                                    new IllegalStateException("send failure"));
            ZLinkChannelCallRuntime runtime =
                    new ZLinkChannelCallRuntime(
                            flow(),
                            null,
                            (channel, node, spot, generation, authority, lease, parts) -> {
                                submitted.set(parts);
                                return terminal;
                            },
                            null);
            ZLinkFrameworkMultipartView payload =
                    ZLinkFrameworkMultipartView.snapshot(new byte[] {1, 2, 3});
            RouteSpotSendCall call =
                    new RouteSpotSendCall(
                            runtime,
                            "channel",
                            resolver(),
                            null,
                            "spot",
                            payload,
                            Optional.of("Packet"));

            try {
                if (success) {
                    call.submit().toCompletableFuture().join();
                } else {
                    assertThrows(
                            CompletionException.class,
                            () -> call.submit().toCompletableFuture().join());
                }
                assertClosed(payload, submitted.get());
            } finally {
                if (submitted.get() != null) {
                    ZLinkFrameworkMultipartView.closeAll(submitted.get());
                }
                payload.close();
                runtime.beginClose();
            }
        }
    }

    private static void assertClosed(ZLinkFrameworkMultipartView payload, List<Message> envelope) {
        assertTrue(isClosed(payload), "retained payload owner remains open");
        assertTrue(isClosed(envelope), "retained envelope remains open");
    }

    private static boolean isClosed(Object view) {
        try {
            return view instanceof ZLinkFrameworkMultipartView multipartView
                    && multipartView.getFirst() == null;
        } catch (IllegalStateException expected) {
            return true;
        }
    }

    private static SpotTransportAddressResolver resolver() {
        SpotTransportAddress address =
                new SpotTransportAddress(
                        "route", RoutingId.from("target"), "spot", 1, ZLinkSpotKind.USER);
        return ignored -> CompletableFuture.completedFuture(Optional.of(address));
    }

    private static ZLinkMessageFlowTracer flow() {
        return new ZLinkMessageFlowTracer(
                new ZLinkDispatchOptionsRegistration(),
                ZLinkHandlerActivator.reflection(),
                Runnable::run);
    }
}
