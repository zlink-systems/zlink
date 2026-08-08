package systems.zlink.framework.runtime.diagnostics;

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkLogLevel;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

// Message-flow tracer for success transitions and dispatch errors, keyed by
// correlation id. Mirrors the C++ message_flow_tracer / .NET ZLinkMessageFlowTracer.
//
// PERFORMANCE: callers MUST guard event construction with enabled(outcome) so an "off"
// dispatch pays nothing but a volatile mode read:
//     if (tracer.enabled(outcome)) tracer.trace(new ZLinkMessageFlowEvent(...));
public final class ZLinkMessageFlowTracer {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkMessageFlowTracer.class.getName());

    private final ZLinkDispatchOptionsRegistration options;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final AtomicLong tracedCount = new AtomicLong();
    private final AtomicLong providerFailureCount = new AtomicLong();
    private final AtomicLong sampleCounter = new AtomicLong();

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

    // Cheap mode gate (volatile read of the live mode). Build the event only after
    // this returns true.
    public boolean enabled(ZLinkMessageFlowOutcome outcome) {
        return options.diagnostics().effectiveMessageFlow().value() >= requiredMode(outcome).value();
    }

    public void trace(ZLinkMessageFlowEvent flow) {
        if (!enabled(flow.outcome())) {
            return;
        }
        ZLinkMessageFlowEvent tracedFlow = attachFlow(flow);
        if (tracedFlow.outcome() != ZLinkMessageFlowOutcome.DROPPED
            && tracedFlow.outcome() != ZLinkMessageFlowOutcome.ERROR
            && !sample(tracedFlow.flowId())) {
            return;
        }
        tracedCount.incrementAndGet();
        try {
            logDefault(tracedFlow);
        } catch (Throwable ex) {
            reportProviderFailure(ex);
        }
    }

    private ZLinkMessageFlowEvent attachFlow(ZLinkMessageFlowEvent event) {
        if (event.flowId() != null) {
            return event;
        }
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        if (state == null) {
            state = ZLinkFlowContext.create(originFor(event));
        }
        return event.withFlow(state.flowId(), state.origin());
    }

    private static ZLinkFlowOrigin originFor(ZLinkMessageFlowEvent event) {
        return event.outcome() == ZLinkMessageFlowOutcome.RECEIVED
            ? ZLinkFlowOrigin.INBOUND
            : ZLinkFlowOrigin.APPLICATION;
    }

    public long tracedCount() {
        return tracedCount.get();
    }

    public long providerFailureCount() {
        return providerFailureCount.get();
    }

    private static ZLinkMessageFlowLogMode requiredMode(ZLinkMessageFlowOutcome outcome) {
        return outcome == ZLinkMessageFlowOutcome.DROPPED
            || outcome == ZLinkMessageFlowOutcome.ERROR
            ? ZLinkMessageFlowLogMode.ERRORS
            : ZLinkMessageFlowLogMode.NORMAL;
    }

    private boolean sample(String flowId) {
        double rate = options.diagnostics().sampleRate();
        if (rate >= 1.0d) {
            return true;
        }
        if (rate <= 0.0d) {
            return false;
        }
        long stride = Math.max(1L, Math.round(1.0d / rate));
        if (flowId != null) {
            return Math.floorMod(flowId.hashCode(), stride) == 0;
        }
        return sampleCounter.incrementAndGet() % stride == 0;
    }

    private void reportProviderFailure(Throwable error) {
        providerFailureCount.incrementAndGet();
        if (eventDispatcher == null) {
            return;
        }
        eventDispatcher.publishObserverFailure(
            "message-flow",
            "standard-logger",
            error);
    }

    private void logDefault(ZLinkMessageFlowEvent flow) {
        ZLinkDispatchOptionsRegistration.DiagnosticsOptions diagnostics = options.diagnostics();
        Long size = null;
        if (flow.messageSize() != null
            && diagnostics.effectiveMessageFlow().value() >= ZLinkMessageFlowLogMode.DETAILED.value()
            && diagnostics.includeMessageSizes()) {
            size = flow.messageSize();
        }

        String line = ZLinkTraceFormat.flowLine(flow, null, size);
        LOGGER.log(logLevel(flow), line);
    }

    Level logLevel(ZLinkMessageFlowEvent flow) {
        if (flow.outcome() != ZLinkMessageFlowOutcome.ERROR
            && flow.outcome() != ZLinkMessageFlowOutcome.DROPPED) {
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
}
