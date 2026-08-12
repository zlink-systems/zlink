package systems.zlink.e2e.toactormessaging.shared;

import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String SPOT_MESH = "to-actor";
    public static final String ACTOR_TYPE = "test-actor";

    private Contracts() {
    }

    @ZLinkPacket("ActorMsg")
    public record ActorMsg(String scenario, String actorId, String value) {
    }

    @ZLinkPacket("ActorReq")
    public record ActorReq(String scenario, String actorId, String value) {
    }

    public record ActorRes(String scenario, String actorId, String value) {
    }

    public record ActorEvidence(String scenario, String actorId, String kind, String value) {
    }

    public record ActorCallReq(String scenario, String actorId, String value) {
    }

    public record ActorCreateReq(String reason) {
    }

    public record ActorRefWire(String nodeRidHex, String actorId, long generation) {
    }

    public record ActorCallRes(
        String scenario,
        String actorId,
        String result,
        String errorKind) {
        public static ActorCallRes ok(String scenario, String actorId, String result) {
            return new ActorCallRes(scenario, actorId, result, null);
        }

        public static ActorCallRes failed(String scenario, String actorId, String kind) {
            return new ActorCallRes(scenario, actorId, "failed", kind);
        }
    }

    public record RouteStatusRes(boolean ready, String targetRid) {
    }

    public record BindActorReq(ActorRefWire actorRef) {
    }

    public record BindActorRes(String actorId, String nodeRid, long generation) {
    }

    public record BoundPushReq(String scenario, String actorId, String value) {
    }

    public record BoundPushRes(String actorId, String value, boolean submitted, String errorKind) {
        public static BoundPushRes submitted(String actorId, String value) {
            return new BoundPushRes(actorId, value, true, null);
        }

        public static BoundPushRes failed(String actorId, String value, String errorKind) {
            return new BoundPushRes(actorId, value, false, errorKind);
        }
    }

    public record BoundPushNotify(String scenario, String actorId, String value) {
    }

    public record DestroyActorReq(String scenario, String actorId) {
    }

    public record DestroyActorRes(String actorId, boolean destroyed) {
    }

    public record DestroyActorRefReq(String scenario, ActorRefWire actorRef) {
    }

    public record DestroyActorRefRes(String actorId, boolean destroyed, String errorKind) {
        public static DestroyActorRefRes completed(String actorId, boolean destroyed) {
            return new DestroyActorRefRes(actorId, destroyed, null);
        }

        public static DestroyActorRefRes failed(String actorId, String errorKind) {
            return new DestroyActorRefRes(actorId, false, errorKind);
        }
    }

    public record UnbindActorReq(String scenario, String actorId) {
    }

    public record UnbindActorRes(String actorId, boolean unbound) {
    }
}
