package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotDiscoveryProbeReq;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotDiscoveryProbeRes;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

/** Reciprocal-discovery probe: answering with this node's own routing id
 * proves the peer edge is admitted in both directions before the join runs. */
public final class UserSpotDiscoveryProbeHandler
    implements ZLinkRouteRequestHandler<UserSpotDiscoveryProbeReq, UserSpotDiscoveryProbeRes> {
    private final NodeIdentity identity;

    public UserSpotDiscoveryProbeHandler(NodeIdentity identity) {
        this.identity = identity;
    }

    @Override
    public CompletionStage<UserSpotDiscoveryProbeRes> handle(
        UserSpotDiscoveryProbeReq request,
        ZLinkRouteMessageContext context) {
        return CompletableFuture.completedFuture(
            new UserSpotDiscoveryProbeRes(request.marker(), identity.nodeRid()));
    }
}
