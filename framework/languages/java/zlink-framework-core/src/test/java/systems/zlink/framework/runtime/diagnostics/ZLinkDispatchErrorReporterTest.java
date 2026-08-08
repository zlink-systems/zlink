package systems.zlink.framework.runtime.diagnostics;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchFailure;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

class ZLinkDispatchErrorReporterTest {
    @Test
    void offSkipsFlowTracingBeforeConstructingAnEvent() {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        ZLinkDispatchErrorReporter reporter = new ZLinkDispatchErrorReporter(
            options,
            ZLinkHandlerActivator.reflection(),
            Runnable::run);

        reporter.report(failure());

        assertEquals(1L, reporter.reportedCount());
        assertEquals(0L, reporter.flow().tracedCount());
    }

    private static ZLinkDispatchFailure failure() {
        return new ZLinkDispatchFailure(
            ZLinkDispatchErrorSurface.CHANNEL,
            ZLinkDispatchMessageKind.REQUEST,
            ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
            ZLinkDispatchErrorAction.REPLY_ERROR,
            "PlaceOrder",
            "orders",
            null,
            null,
            null,
            null,
            "corr-1",
            IllegalStateException.class.getName(),
            "failed");
    }
}
