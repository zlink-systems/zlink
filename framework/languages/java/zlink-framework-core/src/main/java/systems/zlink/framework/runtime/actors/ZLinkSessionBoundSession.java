package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkBoundSession;

import java.util.concurrent.CompletionStage;

/** Internal identity carried by a bound Session runtime. */
interface ZLinkSessionBoundSession extends ZLinkBoundSession {
    RoutingId sessionRid();

    CompletionStage<Void> disconnectForDestroy();
}
