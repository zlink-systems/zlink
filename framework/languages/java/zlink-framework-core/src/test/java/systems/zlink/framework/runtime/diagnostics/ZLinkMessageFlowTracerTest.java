package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Arrays;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Level;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkActivationState;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkChannelRouteKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowResult;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkTraceEventId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

// MFLOW-001/002/003/011: mode gating + runtime live toggle + structured output.
class ZLinkMessageFlowTracerTest {
    private static ZLinkDispatchOptionsRegistration options(ZLinkMessageFlowLogMode mode) {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(mode);
        return options;
    }

    private static ZLinkMessageFlowTracer tracer(ZLinkDispatchOptionsRegistration options) {
        return new ZLinkMessageFlowTracer(options, ZLinkHandlerActivator.reflection(), Runnable::run);
    }

    private static ZLinkMessageFlowEvent flow(ZLinkMessageFlowOutcome phase) {
        return new ZLinkMessageFlowEvent(
            phase,
            ZLinkDispatchErrorSurface.CHANNEL,
            ZLinkDispatchMessageKind.REQUEST,
            "PlaceOrder", "orders", null, "corr-1", null, null, null, null);
    }

    @Test
    void offSuppressesAllTransitions() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.OFF));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.DROPPED));
    }

    @Test
    void errorsOnlyEmitsDroppedNotReceived() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.DROPPED));
    }

    @Test
    void keyTransitionsEmitsLifecycle() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.REPLIED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.ADMITTED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.COMPLETED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.BACKPRESSURED));
    }

    @Test
    void liveModeOverridesAndTogglesAtRuntime() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.OFF);
        AtomicReference<ZLinkMessageFlowLogMode> cell =
            new AtomicReference<>(ZLinkMessageFlowLogMode.OFF);
        options.diagnostics().installLiveMode(cell);
        ZLinkMessageFlowTracer tracer = tracer(options);

        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.NORMAL);
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.OFF);
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
    }

    @Test
    void tracesStructuredTransitions() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkMessageFlowTracer tracer = tracer(options);

        tracer.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));
        tracer.trace(flow(ZLinkMessageFlowOutcome.REPLIED));

        assertEquals(2L, tracer.tracedCount());
    }

    @Test
    void processingPointUsesTheSingleLiveLevelReadCapturedByBegin() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkMessageFlowTracer tracer = tracer(options);

        ZLinkMessageFlowTracer.TracePoint point =
            tracer.begin(ZLinkMessageFlowOutcome.RECEIVED);
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        point.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));

        assertEquals(1L, tracer.tracedCount());
    }

    @Test
    void spec26PhasesAndOutcomesAreSeparateClosedVocabularies() {
        assertEquals(
            Set.of("RECEIVED", "ADMITTED", "DISPATCHED", "COMPLETED", "REPLIED",
                "SENT", "REPLY_RECEIVED", "BACKPRESSURED", "DROPPED"),
            Set.copyOf(Arrays.stream(ZLinkMessageFlowOutcome.values())
                .map(Enum::name).toList()));
        assertEquals(
            Set.of("succeeded", "failed", "backpressured", "dropped", "cancelled",
                "shutdown"),
            Set.copyOf(Arrays.stream(ZLinkMessageFlowResult.values())
                .map(ZLinkMessageFlowResult::traceName).toList()));
        assertEquals(
            Set.of("node", "channel", "spot", "instance_spot", "actor", "stream",
                "actor_relocation", "classic_fanout"),
            Set.copyOf(Arrays.stream(ZLinkDispatchErrorSurface.values())
                .map(ZLinkDispatchErrorSurface::traceName).toList()));
        assertEquals(
            Set.of("send", "request", "response", "error", "control"),
            Set.copyOf(Arrays.stream(ZLinkDispatchMessageKind.values())
                .map(ZLinkDispatchMessageKind::traceName).toList()));
        assertEquals(
            Set.of("reply_error", "fail_caller", "drop"),
            Set.copyOf(Arrays.stream(ZLinkDispatchErrorAction.values())
                .map(ZLinkDispatchErrorAction::traceName).toList()));
        assertEquals(
            Set.of("no_handler", "decode_error", "handler_exception", "invalid_frame",
                "reply_path_missing", "unexpected_reply", "backpressure", "stale_target",
                "shutdown", "target_closed", "location_unavailable",
                "activation_rejected", "activation_timeout"),
            Set.copyOf(Arrays.stream(ZLinkDispatchErrorReason.values())
                .map(ZLinkDispatchErrorReason::traceName).toList()));
    }

    @Test
    void flowlessSamplingDoesNotCreateFlowAndFailuresBypassSampling() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.NORMAL);
        options.traceSampleRate(0.0d);
        ZLinkMessageFlowTracer tracer = tracer(options);

        assertNull(systems.zlink.framework.runtime.internal.diagnostics
            .ZLinkFlowContext.current());
        tracer.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));
        tracer.trace(flow(ZLinkMessageFlowOutcome.BACKPRESSURED));

        assertEquals(1L, tracer.tracedCount());
        assertNull(systems.zlink.framework.runtime.internal.diagnostics
            .ZLinkFlowContext.current());
    }

    @Test
    void requestTerminalClassifiesEveryTerminalAndCompletionOwnerEmitsOnce() {
        assertEquals(ZLinkMessageFlowResult.SUCCEEDED,
            ZLinkMessageFlowTracer.requestTerminalResult(null, false));
        assertEquals(ZLinkMessageFlowResult.FAILED,
            ZLinkMessageFlowTracer.requestTerminalResult(
                new TimeoutException("deadline"), false));
        assertEquals(ZLinkMessageFlowResult.CANCELLED,
            ZLinkMessageFlowTracer.requestTerminalResult(
                new CancellationException("cancelled"), true));
        assertEquals(ZLinkMessageFlowResult.SHUTDOWN,
            ZLinkMessageFlowTracer.requestTerminalResult(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN, "shutdown"), false));
        assertEquals(ZLinkMessageFlowResult.BACKPRESSURED,
            ZLinkMessageFlowTracer.requestTerminalResult(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, "full"), false));

        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.NORMAL));
        CompletableFuture<String> request = new CompletableFuture<>();
        request.whenComplete((ignored, error) -> {
            ZLinkMessageFlowTracer.TerminalTracePoint terminal =
                tracer.beginRequestTerminal(error, request);
            if (terminal != null) {
                terminal.trace(flow(ZLinkMessageFlowOutcome.REPLY_RECEIVED));
            }
        });

        assertTrue(request.complete("ok"));
        assertFalse(request.completeExceptionally(new IllegalStateException("late")));
        assertFalse(request.cancel(false));
        assertEquals(1L, tracer.tracedCount());
    }

    @Test
    void formatterUsesNormativeEventIdsValuesPrefixAndExactStructuredKeys() {
        ZLinkMessageFlowEvent event = new ZLinkMessageFlowEvent(
            ZLinkTraceEventId.MESSAGE_FLOW,
            ZLinkMessageFlowOutcome.COMPLETED,
            ZLinkMessageFlowResult.SUCCEEDED,
            ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
            ZLinkDispatchMessageKind.ACTOR_SEND,
            "PlaceOrder", "orders", ZLinkChannelRouteKind.ROUTE_MESH, "mesh-a",
            "topic-a", "corr-1", "source-1", "target-1", "server-1", "spot-1",
            "CartSpot", ZLinkActivationState.READY, "actor-1", 42L, 0.125d,
            null, null, null, null, null, null, null, null, 17L);

        String line = ZLinkTraceFormat.flowLine(event, 42L);
        assertTrue(line.startsWith("zlink flow: event_id=zlink.message_flow"));
        assertTrue(line.contains(" phase=completed"));
        assertTrue(line.contains(" surface=channel"));
        assertTrue(line.contains(" kind=send"));
        assertTrue(line.contains(" outcome=succeeded"));
        Set<String> expected = Set.of(
            "event_id", "phase", "surface", "kind", "mesh", "channel",
            "channel_route", "source_rid", "target_rid", "server_rid", "packet",
            "topic", "spot", "instance_type", "activation_state", "actor", "corr",
            "outcome", "size");
        assertEquals(expected, Set.copyOf(
            Arrays.stream(line.substring("zlink flow: ".length()).split(" "))
                .map(field -> field.substring(0, field.indexOf('='))).toList()));
        assertFalse(line.contains("duration_seconds="));

        ZLinkMessageFlowEvent dispatchError = ZLinkMessageFlowEvent.dispatchError(
            ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
            ZLinkDispatchMessageKind.SEND,
            "Notice", "events", "topic-a", null, null, null, null,
            ZLinkDispatchErrorReason.HANDLER_MISSING,
            ZLinkDispatchErrorAction.DROP,
            null, null);
        String errorLine = ZLinkTraceFormat.flowLine(dispatchError, null);
        assertTrue(errorLine.contains("event_id=zlink.dispatch_error"));
        assertTrue(errorLine.contains("surface=classic_fanout"));
        assertTrue(errorLine.contains("reason=no_handler"));
        assertTrue(errorLine.contains("outcome=failed"));
        assertFalse(errorLine.contains(" phase="));
        assertEquals(ZLinkDispatchErrorAction.DROP, dispatchError.errorAction());
        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkMessageFlowEvent(
                ZLinkTraceEventId.DISPATCH_ERROR, null, ZLinkMessageFlowResult.FAILED,
                ZLinkDispatchErrorSurface.CHANNEL, ZLinkDispatchMessageKind.REQUEST,
                null, null, ZLinkChannelRouteKind.CLIENT_SERVER, null, null, null,
                null, null, null, null, null, null, null, null, null, null, null,
                null, null, null, null, null, null, null));
        assertThrows(IllegalArgumentException.class, () ->
            new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.RECEIVED,
                ZLinkDispatchErrorSurface.CLASSIC_FANOUT,
                ZLinkDispatchMessageKind.PUBLISH,
                "Notice", "events", "topic-a", null, null, null, null, null));
    }

    @Test
    void dispatchErrorsUseContractLogLevels() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS));

        assertEquals(Level.SEVERE, tracer.logLevel(error(
            ZLinkDispatchMessageKind.SEND, ZLinkDispatchErrorReason.HANDLER_EXCEPTION)));
        assertEquals(Level.SEVERE, tracer.logLevel(error(
            ZLinkDispatchMessageKind.PUBLISH, ZLinkDispatchErrorReason.HANDLER_EXCEPTION)));
        assertEquals(Level.WARNING, tracer.logLevel(error(
            ZLinkDispatchMessageKind.SEND, ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED)));
        assertEquals(Level.WARNING, tracer.logLevel(error(
            ZLinkDispatchMessageKind.ACTOR_SEND, ZLinkDispatchErrorReason.INVALID_FRAME)));
        assertEquals(Level.FINE, tracer.logLevel(error(
            ZLinkDispatchMessageKind.PUBLISH, ZLinkDispatchErrorReason.HANDLER_MISSING)));
        assertEquals(Level.SEVERE, tracer.logLevel(error(
            ZLinkDispatchMessageKind.REQUEST, ZLinkDispatchErrorReason.HANDLER_MISSING)));
    }

    private static ZLinkMessageFlowEvent error(
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorReason reason) {
        return ZLinkMessageFlowEvent.dispatchError(
            ZLinkDispatchErrorSurface.CHANNEL,
            kind,
            "PlaceOrder",
            "orders",
            null,
            "corr-1",
            null,
            null,
            null,
            reason,
            ZLinkDispatchErrorAction.DROP,
            null,
            null);
    }
}
