package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Level;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

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
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS_ONLY));
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.DROPPED));
    }

    @Test
    void keyTransitionsEmitsLifecycle() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.KEY_TRANSITIONS));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.REPLIED));
    }

    @Test
    void liveModeOverridesAndTogglesAtRuntime() {
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.OFF);
        AtomicReference<ZLinkMessageFlowLogMode> cell =
            new AtomicReference<>(ZLinkMessageFlowLogMode.OFF);
        options.diagnostics().installLiveMode(cell);
        ZLinkMessageFlowTracer tracer = tracer(options);

        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.KEY_TRANSITIONS);
        assertTrue(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
        cell.set(ZLinkMessageFlowLogMode.OFF);
        assertFalse(tracer.enabled(ZLinkMessageFlowOutcome.RECEIVED));
    }

    @Test
    void writesStructuredLineToSeparatedFile() throws Exception {
        Path file = Files.createTempFile("zlink-flow", ".log");
        Files.deleteIfExists(file);
        ZLinkDispatchOptionsRegistration options = options(ZLinkMessageFlowLogMode.KEY_TRANSITIONS);
        options.traceLogFile(file.toString()).traceLabel("api");
        ZLinkMessageFlowTracer tracer = tracer(options);

        tracer.trace(flow(ZLinkMessageFlowOutcome.RECEIVED));
        tracer.trace(flow(ZLinkMessageFlowOutcome.REPLIED));

        String content = Files.readString(file);
        assertTrue(content.contains("outcome=RECEIVED"), content);
        assertTrue(content.contains("outcome=REPLIED"), content);
        assertTrue(content.contains("corr=corr-1"), content);
        assertTrue(content.contains("label=api"), content);
        Files.deleteIfExists(file);
        assertEquals(2L, tracer.tracedCount());
    }

    @Test
    void dispatchErrorsUseContractLogLevels() {
        ZLinkMessageFlowTracer tracer = tracer(options(ZLinkMessageFlowLogMode.ERRORS_ONLY));

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
        return new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.ERROR,
            ZLinkDispatchErrorSurface.CHANNEL,
            kind,
            "PlaceOrder",
            "orders",
            null,
            "corr-1",
            null,
            null,
            null,
            null,
            reason,
            ZLinkDispatchErrorAction.DROP,
            null,
            null);
    }
}
