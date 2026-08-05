package systems.zlink.samples.shoppingmall.server.configuration;

public final class SampleFlowLog {
    private SampleFlowLog() {
    }

    public static String path(String directory, String role) {
        return directory + "/flow-" + role + ".log";
    }
}
