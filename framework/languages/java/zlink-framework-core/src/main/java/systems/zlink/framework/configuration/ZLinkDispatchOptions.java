package systems.zlink.framework.configuration;

public interface ZLinkDispatchOptions {
    ZLinkUnhandledDispatchOptions unhandled();

    ZLinkDiagnosticsOptions diagnostics();

    ZLinkDispatchOptions setMessageFlowObserver(
        Class<? extends ZLinkMessageFlowObserver> observerType);

    ZLinkDispatchOptions setMessageFlowObserver(
        ZLinkMessageFlowObserver observer);

    // Fluent diagnostics/tracing config (builder-chain only; the diagnostics fields
    // are read-only, configure them through these).
    ZLinkDispatchOptions messageFlow(ZLinkMessageFlowLogMode mode);

    ZLinkDispatchOptions traceSampleRate(double rate);

    ZLinkDispatchOptions includeMessageSizes(boolean include);

    // Send tracing/error logs to a dedicated file (separated from app logs).
    ZLinkDispatchOptions traceLogFile(String path);

    // Label stamped on every trace line (label=) for cross-node aggregation.
    ZLinkDispatchOptions traceLabel(String label);
}
