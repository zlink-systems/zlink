package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

// The Channel target of §10.6 (Object Client): the Channel send handler receives the Spot's send and answers with a second
// one-way send to the SpotId that the DTO names in returnSpotId. The measured operation lives in the Spot process; this side
// does not assume the Channel context carries the source SpotId.
public final class S2sSpotToChannelSendSendEchoTarget {
    private S2sSpotToChannelSendSendEchoTarget() {}

    public static void run(RoleConfig config) {
        if (!"channel".equals(config.role()) || config.source()) {
            throw new IllegalArgumentException("The Channel role is the echo target of this scenario.");
        }
        ServerApplication.create(config).configure(options -> {
            var mesh = ServerApplication.routeMesh(options, config, "perf-channel");
            mesh.objects().client();
            mesh.channelName(config.channelName()).server().addSendHandler(S2sReturnToSpotHandler.class, PerfEchoRequest.class);
        }).start();
    }
}
