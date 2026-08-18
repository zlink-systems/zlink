package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteFailRequest;
import systems.zlink.crosslanguage.host.SpotRouteContracts.TestHostSpotRouteReply;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Application-origin failure with a typed kind: the wire must preserve
 * "rejected" and must NOT carry the zlink.origin=framework marker. */
public final class SpotRouteFailRequestHandler
    implements ZLinkRouteRequestHandler<TestHostSpotRouteFailRequest, TestHostSpotRouteReply> {
    @Override
    public CompletionStage<TestHostSpotRouteReply> handle(
        TestHostSpotRouteFailRequest request,
        ZLinkRouteMessageContext context) {
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.REJECTED, "application spot route failure");
    }
}
