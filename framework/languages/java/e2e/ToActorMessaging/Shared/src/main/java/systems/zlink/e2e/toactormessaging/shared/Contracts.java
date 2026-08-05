package systems.zlink.e2e.toactormessaging.shared;

import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String SPOT_MESH = "to-actor";
    public static final String ACTOR_TYPE = "test-actor";

    private Contracts() {
    }

    @ZLinkPacket("ActorNotify")
    public record ActorNotify(String scenario, String actorId, String value) {
    }

    @ZLinkPacket("ActorAsk")
    public record ActorAsk(String scenario, String actorId, String value) {
    }

    public record ActorReply(String scenario, String actorId, String value) {
    }

    public record ActorEvidence(String scenario, String actorId, String kind, String value) {
    }

    public record ActorCallRequest(String scenario, String actorId, String value) {
    }

    public record ActorRefWire(String nodeRidHex, String actorId, long generation) {
    }

    public record ActorRefCallRequest(String scenario, ActorRefWire actorRef, String value) {
    }

    public record ActorCallResponse(
        String scenario,
        String actorId,
        String result,
        String errorKind) {
        public static ActorCallResponse ok(String scenario, String actorId, String result) {
            return new ActorCallResponse(scenario, actorId, result, null);
        }

        public static ActorCallResponse failed(String scenario, String actorId, String kind) {
            return new ActorCallResponse(scenario, actorId, "failed", kind);
        }
    }

    public record BindActorRequest(ActorRefWire actorRef) {
    }

    public record BindActorReply(String actorId, String nodeRid, long generation) {
    }

    public record BoundPushRequest(String scenario, String actorId, String value) {
    }

    public record BoundPushReply(String actorId, String value, boolean submitted, String errorKind) {
        public static BoundPushReply submitted(String actorId, String value) {
            return new BoundPushReply(actorId, value, true, null);
        }

        public static BoundPushReply failed(String actorId, String value, String errorKind) {
            return new BoundPushReply(actorId, value, false, errorKind);
        }
    }

    public record BoundPushNotify(String scenario, String actorId, String value) {
    }

    public record DestroyActorRequest(String scenario, String actorId) {
    }

    public record DestroyActorReply(String actorId, boolean destroyed) {
    }

    public record UnbindActorRequest(String scenario, String actorId) {
    }

    public record UnbindActorReply(String actorId, boolean unbound) {
    }
}
