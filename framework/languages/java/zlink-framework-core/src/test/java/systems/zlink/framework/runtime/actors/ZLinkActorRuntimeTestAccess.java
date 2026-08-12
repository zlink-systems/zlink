package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;

/** Test-only access to package-owned Message Follow retention. */
public final class ZLinkActorRuntimeTestAccess {
    private ZLinkActorRuntimeTestAccess() {
    }

    public static void retainMessageFollowSource(
        ZLinkActorRuntime runtime,
        ZLinkActor actor,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute) {
        runtime.retainMessageFollowSource(
            actor,
            sourceActorRef,
            targetActorRef,
            targetAddress,
            targetRoute);
    }
}
