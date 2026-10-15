package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.spots.SpotTerminalProbe;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executors;

final class ZLinkChannelSpotTerminalTest {
    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void sendPreservesFirstTerminal(boolean instance, boolean stale) {
        verifyTerminal(false, instance, stale, false);
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void requestDoesNotCreateAnotherOperation(boolean instance, boolean stale) {
        verifyTerminal(true, instance, stale, false);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void missingOwnerActivatesBeforeFirstSubmission(boolean request) {
        verifyTerminal(request, true, true, true);
    }

    private void verifyTerminal(boolean request, boolean instance, boolean stale, boolean missing) {
        var terminal =
                stale
                        ? ZLinkFrameworkErrorOrigin.framework(
                                ZLinkFrameworkErrorKind.NOT_FOUND, "first operation failed")
                        : new systems.zlink.framework.errors.ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE, "application failure");
        verifyTerminal(request, instance, stale, missing, terminal);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void ownerFenceRefusalInvalidatesWithoutResubmitting(boolean request) {
        verifyTerminal(
                request,
                true,
                true,
                false,
                ZLinkFrameworkErrorOrigin.ownerFenceRefusal("retired owner"));
    }

    @ParameterizedTest
    @CsvSource({"false,NOT_FOUND", "true,NOT_FOUND", "false,UNAVAILABLE", "true,UNAVAILABLE"})
    void applicationFailureKeepsTheReadyRoute(boolean request, ZLinkFrameworkErrorKind kind) {
        verifyTerminal(
                request,
                true,
                false,
                false,
                new ZLinkFrameworkException(kind, "application failure"));
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void frameworkUnavailableInvalidatesTheReadyRoute(boolean request) {
        verifyTerminal(
                request,
                true,
                true,
                false,
                ZLinkFrameworkErrorOrigin.framework(
                        ZLinkFrameworkErrorKind.UNAVAILABLE, "owner unavailable"));
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void receivedUnavailableWithoutControlCodeKeepsTheReadyRoute(boolean request) {
        verifyTerminal(
                request,
                true,
                false,
                false,
                systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping
                        .receivedFailure(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "wire application failure",
                                (int)
                                        systems.zlink.framework.runtime.protocol
                                                .ServiceWireConstants
                                                .FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED,
                                java.util.Map.of()));
    }

    private void verifyTerminal(
            boolean request,
            boolean instance,
            boolean stale,
            boolean missing,
            RuntimeException terminal) {
        var probe = new SpotTerminalProbe(terminal, missing);
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        var runtime =
                new ZLinkChannelCallRuntime(
                        new ZLinkMessageFlowTracer(
                                new ZLinkDispatchOptionsRegistration(), null, Runnable::run),
                        scheduler,
                        null,
                        (channel, node, spot, generation, authority, lease, parts) -> {
                            probe.submissions++;
                            return CompletableFuture.failedFuture(terminal);
                        },
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
                                requestAdmission) -> {
                            probe.submissions++;
                            probe.operationIds.add(operationId);
                            return registry.submit(
                                    operationId,
                                    timeout,
                                    () -> CompletableFuture.failedFuture(terminal),
                                    replies -> replies.forEach(Message::close));
                        });
        probe.readySend =
                payload ->
                        runtime.sendToSpot(
                                probe.address.routerChannelId(),
                                probe.address.targetNodeRid(),
                                probe.address.spotId(),
                                probe.address.spotGeneration(),
                                probe.address.authorityOwnerGeneration(),
                                probe.address.ownerLeaseGeneration(),
                                java.util.List.of(payload));
        probe.readyRequest =
                (payload, timeout) ->
                        runtime.requestToSpot(
                                probe.address.routerChannelId(),
                                probe.address.targetNodeRid(),
                                probe.address.spotId(),
                                probe.address.spotGeneration(),
                                probe.address.authorityOwnerGeneration(),
                                probe.address.ownerLeaseGeneration(),
                                java.util.List.of(payload),
                                timeout,
                                null);
        try (Message payload = Message.from(new byte[] {1})) {
            java.util.concurrent.CompletionStage<?> result;
            if (request) {
                var call =
                        new RouteSpotRequestCall(
                                runtime,
                                "channel",
                                "mesh",
                                probe,
                                () -> probe,
                                "spot",
                                payload,
                                Optional.of("Request"),
                                Duration.ofSeconds(1));
                result = (instance ? call.instanceSpot() : call).submit(String.class);
            } else {
                var call =
                        new RouteSpotSendCall(
                                runtime,
                                "channel",
                                probe,
                                () -> probe,
                                "spot",
                                payload,
                                Optional.of("Send"));
                result = (instance ? call.instanceSpot() : call).submit();
            }
            var failure =
                    assertThrows(
                            CompletionException.class, () -> result.toCompletableFuture().join());
            assertSame(terminal, failure.getCause());
            assertEquals(
                    missing ? 0 : 1,
                    probe.submissions,
                    "the first terminal must not cause resubmission");
            assertEquals(
                    request && !missing ? 1 : 0,
                    probe.operationIds.size(),
                    "no second operation ID");
            assertEquals(1, probe.resolves, "Ready routes use the resolver before activation");
            assertEquals(missing ? 1 : 0, probe.coordinatorCalls);
            assertEquals(stale ? 1 : 0, probe.invalidations);
            assertEquals(missing ? 1 : 0, probe.activations);
        } finally {
            runtime.beginClose();
            scheduler.shutdownNow();
        }
    }
}
