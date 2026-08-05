package systems.zlink.framework.actors;

import systems.zlink.contracts.core.RoutingId;

public record ActorRefSnapshot(
    String actorId,
    long objectGeneration,
    String meshName,
    RoutingId nodeRid) {
    public static ActorRefSnapshot from(ActorRef actorRef) {
        return new ActorRefSnapshot(
            actorRef.actorId(),
            actorRef.objectGeneration(),
            actorRef.meshName(),
            actorRef.nodeRid());
    }

    public ActorRef toActorRef() {
        return new ActorRef(actorId, objectGeneration, meshName, nodeRid);
    }
}
