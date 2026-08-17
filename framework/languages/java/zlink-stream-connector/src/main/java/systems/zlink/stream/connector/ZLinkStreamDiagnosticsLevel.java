package systems.zlink.stream.connector;

/**
 * Message-flow diagnostics level of the standalone STREAM connector
 * (spec 26 §4 values). The default is {@link #ERRORS}, which preserves the
 * connector's flow wire behavior. At {@link #OFF} the connector applies the
 * spec 27 §4 omission rules: outbound frames carry no {@code flow_id} /
 * {@code flow_origin} pair (header flag 0x10 stays clear), inbound flow
 * fields are neither validated nor installed as flow context (structural
 * length checks remain), and no trace-only flow work runs.
 */
public enum ZLinkStreamDiagnosticsLevel {
    OFF,
    ERRORS,
    NORMAL,
    DETAILED
}
