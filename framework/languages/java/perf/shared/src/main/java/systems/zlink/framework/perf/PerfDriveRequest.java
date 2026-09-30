package systems.zlink.framework.perf;

// §10.5-§10.6: the local public Spot driver asks the Spot handler to run one measured operation.
public record PerfDriveRequest(PerfEchoRequest echo) {}
