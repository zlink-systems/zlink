package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;

// The Channel echo target of §10.5: an automatic RouteMesh node (run Docker Redis) that serves this cell's ChannelName with
// the typed request handler. The measured operation lives in the Spot process; this side echoes.
public final class S2sSpotToChannelRequestEchoTarget {
    private S2sSpotToChannelRequestEchoTarget() {}

    public static void run(RoleConfig config) {
        if (!"channel".equals(config.role()) || config.source()) {
            throw new IllegalArgumentException("The Channel role is the echo target of this scenario.");
        }
        ServerApplication.create(config).configure(options -> ServerApplication.routeMesh(options, config, "perf-channel")
                .channelName(config.channelName()).server()
                .addRequestHandler(ChannelEchoHandler.class, PerfEchoRequest.class, PerfEchoReply.class)).start();
    }
}
