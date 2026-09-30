package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        if (!"spot".equals(config.role())) {
            throw new IllegalArgumentException("spot-server runs the spot role.");
        }
        switch (config.scenario()) {
            case "s2s-channel-to-spot-request-echo", "s2s-channel-to-spot-send-send-echo" -> S2sChannelToSpotEchoTarget.run(config);
            case "s2s-spot-to-channel-request-echo" -> S2sSpotToChannelRequestEchoScenario.run(config);
            case "s2s-spot-to-channel-send-send-echo" -> S2sSpotToChannelSendSendEchoScenario.run(config);
            case "spot-no-await-echo" -> SpotNoAwaitEchoScenario.run(config);
            case "spot-worker-offload-echo" -> SpotWorkerOffloadEchoScenario.run(config);
            default -> throw new IllegalArgumentException("spot-server does not run scenario " + config.scenario() + ".");
        }
    }
}
