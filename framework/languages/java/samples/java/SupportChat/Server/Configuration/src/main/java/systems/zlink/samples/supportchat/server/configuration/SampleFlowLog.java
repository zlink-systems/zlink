package systems.zlink.samples.supportchat.server.configuration;

public final class SampleFlowLog {
    private SampleFlowLog() {
    }

    public static String path(SampleTopology topology, String role) {
        return topology.requiredLogDirectory() + "/flow-" + role + ".log";
    }
}
