package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

public final class Program {
    private Program() {}

    public static void main(String[] args) {
        RoleConfig config = ServerApplication.readConfig(args);
        switch (config.scenario()) {
            case "channel-echo-only" -> ChannelEchoOnlyScenario.run(config);
            case "s2s-channel-to-spot-request-echo" -> S2sChannelToSpotRequestEchoScenario.run(config);
            case "s2s-channel-to-spot-send-send-echo" -> S2sChannelToSpotSendSendEchoScenario.run(config);
            case "s2s-spot-to-channel-request-echo" -> S2sSpotToChannelRequestEchoTarget.run(config);
            case "s2s-spot-to-channel-send-send-echo" -> S2sSpotToChannelSendSendEchoTarget.run(config);
            default -> throw new IllegalArgumentException("channel-server does not run scenario " + config.scenario() + ".");
        }
    }
}
