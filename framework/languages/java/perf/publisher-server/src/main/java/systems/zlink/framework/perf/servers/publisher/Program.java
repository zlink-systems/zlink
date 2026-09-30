package systems.zlink.framework.perf.servers.publisher;

import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"pubsub-fanout-echo".equals(config.scenario()) || !"publisher".equals(config.role()) || !config.source()) {
            throw new IllegalArgumentException("publisher-server runs the publisher role of pubsub-fanout-echo.");
        }
        PubSubFanoutEchoScenario.run(config, ServerApplication.cellDirectory(args));
    }
}
