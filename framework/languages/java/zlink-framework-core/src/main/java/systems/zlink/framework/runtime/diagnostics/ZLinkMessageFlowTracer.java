package systems.zlink.framework.runtime.diagnostics;

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

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Internal Spec 26 message-flow tracer. */
public final class ZLinkMessageFlowTracer {
    private static final int FNV_OFFSET_BASIS = 0x811c9dc5;
    private static final int FNV_PRIME = 0x01000193;
    private static final double UNSIGNED_HASH_RANGE = (double) (1L << Integer.SIZE);
    private static final int UTF8_ASCII_LIMIT = 0x80;
    private static final int UTF8_TWO_BYTE_LIMIT = 0x800;
    private static final int UTF8_TWO_BYTE_PREFIX = 0xc0;
    private static final int UTF8_THREE_BYTE_PREFIX = 0xe0;
    private static final int UTF8_FOUR_BYTE_PREFIX = 0xf0;
    private static final int UTF8_CONTINUATION_PREFIX = 0x80;
    private static final int UTF8_CONTINUATION_PAYLOAD_BITS = 6;
    private static final int UTF8_CONTINUATION_PAYLOAD_MASK =
            (1 << UTF8_CONTINUATION_PAYLOAD_BITS) - 1;
    private static final Logger LOGGER = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
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

    /**
     * Spec 27 §4 capture gate: flow context is created, installed and copied forward at every level
     * except Off. At Off the processing point must not validate, install or propagate flow state
     * (correlation_id is unaffected).
     */
    public boolean captureEnabled() {
        return options.diagnostics().effectiveMessageFlow() != ZLinkMessageFlowLogMode.OFF;
    }

    public TracePoint begin(ZLinkMessageFlowOutcome phase) {
        return begin(phase, defaultResult(phase));
    }

    public TracePoint begin(ZLinkMessageFlowOutcome phase, ZLinkMessageFlowResult result) {
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
     * Captures the live level before doing terminal-only classification work. The returned point
     * owns the request's one terminal message-flow record.
     */
    public TerminalTracePoint beginRequestTerminal(
            Throwable failure, java.util.concurrent.Future<?> request) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (mode == ZLinkMessageFlowLogMode.OFF) {
            return null;
        }
        return beginRequestTerminalAtMode(failure, request != null && request.isCancelled(), mode);
    }

    private TerminalTracePoint beginRequestTerminalAtMode(
            Throwable failure, boolean cancelled, ZLinkMessageFlowLogMode mode) {
        ZLinkMessageFlowResult result = requestTerminalResult(failure, cancelled);
        if (!accepts(mode, result)) {
            return null;
        }
        return event -> traceAtMode(event.withOutcome(result), mode);
    }

    public void trace(ZLinkMessageFlowEvent flow) {
        ZLinkMessageFlowLogMode mode = options.diagnostics().effectiveMessageFlow();
        if (!accepts(mode, flow.outcome())) {
            return;
        }
        traceAtMode(flow, mode);
    }

    private void traceAtMode(ZLinkMessageFlowEvent event, ZLinkMessageFlowLogMode mode) {
        ZLinkMessageFlowEvent tracedFlow = attachAmbientFlow(event);
        if (sampled(tracedFlow)
                && !sample(tracedFlow.flowId(), tracedFlow.sourceMeshGeneration())) {
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
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        if (state == null) {
            return event;
        }
        ZLinkMessageFlowEvent traced =
                event.flowId() == null ? event.withFlow(state.flowId(), state.origin()) : event;
        return traced.streamSessionId() == null && state.streamSessionId() != null
                ? traced.withStreamSessionId(state.streamSessionId())
                : traced;
    }

    public long tracedCount() {
        return tracedCount.get();
    }

    public long providerFailureCount() {
        return providerFailureCount.get();
    }

    private static boolean accepts(ZLinkMessageFlowLogMode mode, ZLinkMessageFlowResult result) {
        ZLinkMessageFlowLogMode required =
                result == ZLinkMessageFlowResult.SUCCEEDED
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

    static ZLinkMessageFlowResult requestTerminalResult(Throwable failure, boolean cancelled) {
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
        }
        return actual == null ? ZLinkMessageFlowResult.SUCCEEDED : ZLinkMessageFlowResult.FAILED;
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
            long generation =
                    eventSourceGeneration == null
                            ? sourceMeshGeneration
                            : eventSourceGeneration.longValue();
            samplingKey = generation + ":" + localSamplingSequence.incrementAndGet();
        }
        long unsignedHash = Integer.toUnsignedLong(fnv1a(samplingKey));
        return unsignedHash / UNSIGNED_HASH_RANGE < rate;
    }

    static int fnv1a(String value) {
        int hash = FNV_OFFSET_BASIS;
        for (int index = 0; index < value.length(); index++) {
            char current = value.charAt(index);
            if (current < UTF8_ASCII_LIMIT) {
                hash = fnv1aByte(hash, current);
            } else if (current < UTF8_TWO_BYTE_LIMIT) {
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_TWO_BYTE_PREFIX | current >>> UTF8_CONTINUATION_PAYLOAD_BITS);
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | current & UTF8_CONTINUATION_PAYLOAD_MASK);
            } else if (Character.isHighSurrogate(current)
                    && index + 1 < value.length()
                    && Character.isLowSurrogate(value.charAt(index + 1))) {
                int codePoint = Character.toCodePoint(current, value.charAt(++index));
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_FOUR_BYTE_PREFIX
                                        | codePoint >>> (3 * UTF8_CONTINUATION_PAYLOAD_BITS));
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | codePoint >>> (2 * UTF8_CONTINUATION_PAYLOAD_BITS)
                                                & UTF8_CONTINUATION_PAYLOAD_MASK);
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | codePoint >>> UTF8_CONTINUATION_PAYLOAD_BITS
                                                & UTF8_CONTINUATION_PAYLOAD_MASK);
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | codePoint & UTF8_CONTINUATION_PAYLOAD_MASK);
            } else if (Character.isSurrogate(current)) {
                hash = fnv1aByte(hash, '?');
            } else {
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_THREE_BYTE_PREFIX
                                        | current >>> (2 * UTF8_CONTINUATION_PAYLOAD_BITS));
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | current >>> UTF8_CONTINUATION_PAYLOAD_BITS
                                                & UTF8_CONTINUATION_PAYLOAD_MASK);
                hash =
                        fnv1aByte(
                                hash,
                                UTF8_CONTINUATION_PREFIX
                                        | current & UTF8_CONTINUATION_PAYLOAD_MASK);
            }
        }
        return hash;
    }

    private static int fnv1aByte(int hash, int value) {
        return (hash ^ value) * FNV_PRIME;
    }

    private void reportProviderFailure(Throwable error) {
        providerFailureCount.incrementAndGet();
        if (eventDispatcher == null) {
            return;
        }
        eventDispatcher.publishObserverFailure("message-flow", "standard-logger", error);
    }

    private void logDefault(ZLinkMessageFlowEvent flow, ZLinkMessageFlowLogMode mode) {
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
