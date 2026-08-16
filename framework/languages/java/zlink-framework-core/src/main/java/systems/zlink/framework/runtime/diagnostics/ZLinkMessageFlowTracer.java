package systems.zlink.framework.runtime.diagnostics;

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.framework.configuration.ZLinkLogLevel;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowResult;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkTraceEventId;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

/** Internal Spec 26 message-flow tracer. */
public final class ZLinkMessageFlowTracer {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
    private static final AtomicLong NEXT_SOURCE_GENERATION = new AtomicLong();

    private final ZLinkDispatchOptionsRegistration options;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final long sourceMeshGeneration = NEXT_SOURCE_GENERATION.incrementAndGet();
    private final AtomicLong localSamplingSequence = new AtomicLong();
    private final AtomicLong tracedCount = new AtomicLong();
    private final AtomicLong providerFailureCount = new AtomicLong();

    public ZLinkMessageFlowTracer(
        ZLinkDispatchOptionsRegistration options,
        ZLinkHandlerActivator handlerFactory,
        Executor executor) {
        this(options, handlerFactory, executor, null);
    }

    public ZLinkMessageFlowTracer(
        ZLinkDispatchOptionsRegistration options,
        ZLinkHandlerActivator handlerFactory,
        Executor executor,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this.options = options;
        this.eventDispatcher = eventDispatcher;
    }

    /** Compatibility query. Processing points should use begin so mode is read once. */
    public boolean enabled(ZLinkMessageFlowOutcome phase) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        return accepts(mode, defaultResult(phase));
    }

    public TracePoint begin(ZLinkMessageFlowOutcome phase) {
        return begin(phase, defaultResult(phase));
    }

    public TracePoint begin(
        ZLinkMessageFlowOutcome phase,
        ZLinkMessageFlowResult result) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (!accepts(mode, result)) {
            return null;
        }
        return event -> traceAtMode(event, mode);
    }

    public TracePoint beginDispatchError() {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (!accepts(mode, ZLinkMessageFlowResult.FAILED)) {
            return null;
        }
        return event -> traceAtMode(event, mode);
    }

    /**
     * Captures the live level before doing terminal-only classification work.
     * The returned point owns the request's one terminal message-flow record.
     */
    public TerminalTracePoint beginRequestTerminal(
        Throwable failure,
        java.util.concurrent.Future<?> request) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (mode == ZLinkMessageFlowLogMode.OFF) {
            return null;
        }
        return beginRequestTerminalAtMode(
            failure, request != null && request.isCancelled(), mode);
    }

    private TerminalTracePoint beginRequestTerminalAtMode(
        Throwable failure,
        boolean cancelled,
        ZLinkMessageFlowLogMode mode) {
        ZLinkMessageFlowResult result = requestTerminalResult(failure, cancelled);
        if (!accepts(mode, result)) {
            return null;
        }
        return event -> traceAtMode(event.withOutcome(result), mode);
    }

    public void traceLazy(
        ZLinkMessageFlowOutcome phase,
        java.util.function.Supplier<ZLinkMessageFlowEvent> flow) {
        TracePoint point = begin(phase);
        if (point != null) {
            point.trace(flow.get());
        }
    }

    public void trace(ZLinkMessageFlowEvent flow) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (!accepts(mode, flow.outcome())) {
            return;
        }
        traceAtMode(flow, mode);
    }

    private void traceAtMode(
        ZLinkMessageFlowEvent event,
        ZLinkMessageFlowLogMode mode) {
        ZLinkMessageFlowEvent tracedFlow = attachAmbientFlow(event);
        if (sampled(tracedFlow) && !sample(
            tracedFlow.flowId(), tracedFlow.sourceMeshGeneration())) {
            return;
        }
        tracedCount.incrementAndGet();
        try {
            logDefault(tracedFlow, mode);
        } catch (Throwable ex) {
            reportProviderFailure(ex);
        }
    }

    private static ZLinkMessageFlowEvent attachAmbientFlow(ZLinkMessageFlowEvent event) {
        if (event.flowId() != null) {
            return event;
        }
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        return state == null ? event : event.withFlow(state.flowId(), state.origin());
    }

    public long tracedCount() {
        return tracedCount.get();
    }

    public long providerFailureCount() {
        return providerFailureCount.get();
    }

    private static boolean accepts(
        ZLinkMessageFlowLogMode mode,
        ZLinkMessageFlowResult result) {
        ZLinkMessageFlowLogMode required = result == ZLinkMessageFlowResult.SUCCEEDED
            ? ZLinkMessageFlowLogMode.NORMAL
            : ZLinkMessageFlowLogMode.ERRORS;
        return mode.value() >= required.value();
    }

    private static ZLinkMessageFlowResult defaultResult(ZLinkMessageFlowOutcome phase) {
        if (phase == ZLinkMessageFlowOutcome.BACKPRESSURED) {
            return ZLinkMessageFlowResult.BACKPRESSURED;
        }
        if (phase == ZLinkMessageFlowOutcome.DROPPED) {
            return ZLinkMessageFlowResult.DROPPED;
        }
        return ZLinkMessageFlowResult.SUCCEEDED;
    }

    static ZLinkMessageFlowResult requestTerminalResult(
        Throwable failure,
        boolean cancelled) {
        if (cancelled || failure instanceof java.util.concurrent.CancellationException) {
            return ZLinkMessageFlowResult.CANCELLED;
        }
        Throwable actual = failure;
        while (actual instanceof java.util.concurrent.CompletionException
            && actual.getCause() != null) {
            actual = actual.getCause();
        }
        if (actual instanceof ZLinkFrameworkException frameworkFailure) {
            if (frameworkFailure.kind() == ZLinkFrameworkErrorKind.SHUTTING_DOWN) {
                return ZLinkMessageFlowResult.SHUTDOWN;
            }
            if (frameworkFailure.kind() == ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED) {
                return ZLinkMessageFlowResult.BACKPRESSURED;
            }
        }
        return actual == null
            ? ZLinkMessageFlowResult.SUCCEEDED
            : ZLinkMessageFlowResult.FAILED;
    }

    private static boolean sampled(ZLinkMessageFlowEvent flow) {
        return flow.eventId() == ZLinkTraceEventId.MESSAGE_FLOW
            && flow.outcome() == ZLinkMessageFlowResult.SUCCEEDED;
    }

    private boolean sample(String flowId, Long eventSourceGeneration) {
        double rate = options.diagnostics().sampleRate();
        if (rate >= 1.0d) {
            return true;
        }
        if (rate <= 0.0d) {
            return false;
        }
        String samplingKey = flowId;
        if (samplingKey == null) {
            long generation = eventSourceGeneration == null
                ? sourceMeshGeneration : eventSourceGeneration.longValue();
            samplingKey = generation + ":" + localSamplingSequence.incrementAndGet();
        }
        long unsignedHash = Integer.toUnsignedLong(fnv1a(samplingKey));
        return unsignedHash / 4294967296.0d < rate;
    }

    private static int fnv1a(String value) {
        int hash = 0x811c9dc5;
        byte[] bytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        for (byte current : bytes) {
            hash ^= current & 0xff;
            hash *= 0x01000193;
        }
        return hash;
    }

    private void reportProviderFailure(Throwable error) {
        providerFailureCount.incrementAndGet();
        if (eventDispatcher == null) {
            return;
        }
        eventDispatcher.publishObserverFailure(
            "message-flow", "standard-logger", error);
    }

    private void logDefault(
        ZLinkMessageFlowEvent flow,
        ZLinkMessageFlowLogMode mode) {
        Long size = null;
        if (flow.messageSize() != null
            && mode.value() >= ZLinkMessageFlowLogMode.DETAILED.value()
            && options.diagnostics().includeMessageSizes()) {
            size = flow.messageSize();
        }
        LOGGER.log(logLevel(flow), ZLinkTraceFormat.flowLine(flow, size));
    }

    Level logLevel(ZLinkMessageFlowEvent flow) {
        if (flow.eventId() == ZLinkTraceEventId.MESSAGE_FLOW
            && flow.outcome() == ZLinkMessageFlowResult.SUCCEEDED) {
            return Level.INFO;
        }
        if (flow.errorReason() == ZLinkDispatchErrorReason.HANDLER_EXCEPTION) {
            return Level.SEVERE;
        }
        if (flow.messageKind() == ZLinkDispatchMessageKind.PUBLISH) {
            return julLevel(options.unhandled().publishLogLevel());
        }
        if (flow.messageKind() == ZLinkDispatchMessageKind.SEND
            || flow.messageKind() == ZLinkDispatchMessageKind.ACTOR_SEND) {
            return julLevel(options.unhandled().sendLogLevel());
        }
        return Level.SEVERE;
    }

    private static Level julLevel(ZLinkLogLevel level) {
        return switch (level) {
            case TRACE -> Level.FINEST;
            case DEBUG -> Level.FINE;
            case INFO -> Level.INFO;
            case WARN -> Level.WARNING;
            case ERROR -> Level.SEVERE;
        };
    }

    @FunctionalInterface
    public interface TracePoint {
        void trace(ZLinkMessageFlowEvent event);
    }

    @FunctionalInterface
    public interface TerminalTracePoint {
        void trace(ZLinkMessageFlowEvent event);
    }
}
