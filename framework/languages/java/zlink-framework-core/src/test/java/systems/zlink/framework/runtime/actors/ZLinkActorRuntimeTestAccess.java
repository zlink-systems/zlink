package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;

/** Test-only access to package-owned Message Follow retention. */
public final class ZLinkActorRuntimeTestAccess {
    private ZLinkActorRuntimeTestAccess() {}

    public static void retainMessageFollowSource(
            ZLinkActorRuntime runtime,
            ZLinkActor actor,
            ZLinkBackendActorRef sourceActorRef,
            ZLinkBackendActorRef targetActorRef,
            SpotTransportAddress targetAddress,
            ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute) {
        runtime.retainMessageFollowSource(
                actor, sourceActorRef, targetActorRef, targetAddress, targetRoute);
    }

    public static ZLinkServiceMessageFollowWireCodec.ActorRoute messageFollowSourceRoute(
            ZLinkActorRuntime runtime, String actorId) {
        return runtime.handoff.messageFollowSource(actorId).orElseThrow().sourceRoute();
    }

    public static ZLinkServiceM6BWireCodec.ActorRouteFence messageFollowTargetRoute(
            ZLinkActorRuntime runtime, String actorId) {
        return runtime.handoff.messageFollowSource(actorId).orElseThrow().rawTargetRoute();
    }

    public static void removeMessageFollowSource(
            ZLinkActorRuntime runtime, ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute) {
        var source = runtime.handoff.messageFollowSource(sourceRoute).orElseThrow();
        runtime.removeMessageFollowSource(source);
    }

    public static ZLinkStateLane dispatchLane(ZLinkActorRuntime runtime) {
        return runtime.dispatches.stateLane;
    }
}
