package systems.zlink.framework.configuration;

// Read-only diagnostics view. Configure via the fluent builder on ZLinkDispatchOptions
// (configureDispatch().messageFlow(...).traceLogFile(...).traceLabel(...)...).
public interface ZLinkDiagnosticsOptions {
    // Configured (static) mode. Use effectiveMessageFlow() for runtime decisions.
    ZLinkMessageFlowLogMode messageFlow();

    double sampleRate();

    boolean includeMessageSizes();

    boolean includeNativeDiagnostics();

    // When set, tracing/error logs go to this dedicated file (separated from app
    // logs). Null = shared app logger.
    String logFile();

    // Node/runtime identity stamped on each trace line. Null = omitted.
    String label();

    // The mode actually in effect: the runtime-mutable live override if installed,
    // else the configured mode (read live on every dispatch).
    ZLinkMessageFlowLogMode effectiveMessageFlow();
}
