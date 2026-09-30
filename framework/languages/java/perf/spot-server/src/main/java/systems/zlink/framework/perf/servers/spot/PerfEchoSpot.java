package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.spots.ZLinkSpotContext;

// The User Spot that answers a typed PerfEchoRequest with the typed echo at once: the target of §10.3 and the local echo
// Spot of §10.7. The typed request handler is the only application code on the Spot.
public final class PerfEchoSpot extends PerfSpot {
    public PerfEchoSpot(ZLinkSpotContext context) {
        super(context);
        context.handlers().addHandler(PerfEchoRequestHandler.class);
    }
}
