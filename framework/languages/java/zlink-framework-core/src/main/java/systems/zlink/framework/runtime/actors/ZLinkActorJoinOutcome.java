package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.messaging.ZLinkMessage;

sealed interface ZLinkActorJoinOutcome
    permits ZLinkActorJoinOutcome.Accepted, ZLinkActorJoinOutcome.Rejected {

    record Accepted(
        ActorRef actor,
        ZLinkMessage reply) implements ZLinkActorJoinOutcome {
    }

    record Rejected(
        ZLinkMessage reply) implements ZLinkActorJoinOutcome {
    }
}
