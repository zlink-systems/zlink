package systems.zlink.framework.execution;

/**
 * The domain state a serial execution lane is permitted to carry.
 *
 * <p>The variants are deliberately closed.  In particular, a session lane
 * cannot be constructed with Spot relocation state, and an Actor delivery
 * lane cannot be constructed with either lifecycle state.</p>
 */
public sealed interface ZLinkExecutionLanePolicy permits
    ZLinkExecutionLanePolicy.Spot,
    ZLinkExecutionLanePolicy.Session,
    ZLinkExecutionLanePolicy.ActorDelivery,
    ZLinkExecutionLanePolicy.Generic {

    /** Spot lanes alone may resume a turn after its gate has been returned. */
    boolean releasesGateOnIncompleteStage();

    static Spot spot() {
        return Spot.ACTIVE;
    }

    static Spot spotReturningGate() {
        return Spot.RETURN_PENDING;
    }

    static Session session() {
        return Session.OPEN;
    }

    static ActorDelivery actorDelivery() {
        return ActorDelivery.ACTIVE;
    }

    static Generic generic() {
        return Generic.ACTIVE;
    }

    /** A Spot lane may carry return-pending and relocation-seal state. */
    enum Spot implements ZLinkExecutionLanePolicy {
        ACTIVE(false),
        RETURN_PENDING(true),
        RELOCATION_SEALED(false),
        RETURN_PENDING_RELOCATION_SEALED(true);

        private final boolean releasesGateOnIncompleteStage;

        Spot(boolean releasesGateOnIncompleteStage) {
            this.releasesGateOnIncompleteStage = releasesGateOnIncompleteStage;
        }

        @Override
        public boolean releasesGateOnIncompleteStage() {
            return releasesGateOnIncompleteStage;
        }
    }

    /** A session lane may carry connection-closed state, but no Spot state. */
    enum Session implements ZLinkExecutionLanePolicy {
        OPEN,
        CONNECTION_CLOSED;

        @Override
        public boolean releasesGateOnIncompleteStage() {
            return false;
        }
    }

    /** Actor delivery has none of the Spot or session lifecycle state. */
    enum ActorDelivery implements ZLinkExecutionLanePolicy {
        ACTIVE;

        @Override
        public boolean releasesGateOnIncompleteStage() {
            return false;
        }
    }

    /** Other serial owners have none of the three lane-specific states. */
    enum Generic implements ZLinkExecutionLanePolicy {
        ACTIVE;

        @Override
        public boolean releasesGateOnIncompleteStage() {
            return false;
        }
    }
}
