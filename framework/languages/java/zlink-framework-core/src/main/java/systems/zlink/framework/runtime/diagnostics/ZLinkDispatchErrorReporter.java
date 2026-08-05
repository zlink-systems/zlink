package systems.zlink.framework.runtime.diagnostics;

import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

public final class ZLinkDispatchErrorReporter {
    private final AtomicLong reportedCount = new AtomicLong();
    // Success-path tracer companion: every surface already receives a reporter, so
    // exposing the flow tracer here wires all dispatch sites without threading a new
    // parameter. Shares the same options (live mode), factory and executor.
    private final ZLinkMessageFlowTracer flow;

    public ZLinkDispatchErrorReporter(
        ZLinkDispatchOptionsRegistration options,
        ZLinkHandlerActivator handlerFactory,
        Executor executor) {
        this(options, handlerFactory, executor, null);
    }

    public ZLinkDispatchErrorReporter(
        ZLinkDispatchOptionsRegistration options,
        ZLinkHandlerActivator handlerFactory,
        Executor executor,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        this.flow = new ZLinkMessageFlowTracer(options, handlerFactory, executor, eventDispatcher);
    }

    public ZLinkMessageFlowTracer flow() {
        return flow;
    }

    public void report(ZLinkDispatchFailure error) {
        reportedCount.incrementAndGet();
        flow.trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.ERROR,
            error.surface(),
            error.messageKind(),
            error.packetName(),
            error.channelName(),
            error.topic(),
            error.correlationId(),
            error.sourceRid(),
            error.spotId(),
            error.actorId(),
            null,
            error.reason(),
            error.action(),
            error.errorType(),
            error.errorMessage()));
    }

    public long reportedCount() {
        return reportedCount.get();
    }

    public long observerFailureCount() {
        return flow.observerFailureCount();
    }
}
