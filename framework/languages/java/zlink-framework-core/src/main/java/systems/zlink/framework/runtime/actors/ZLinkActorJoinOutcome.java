package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.messaging.ZLinkMessage;

sealed interface ZLinkActorJoinOutcome
        permits ZLinkActorJoinOutcome.Accepted,
                ZLinkActorJoinOutcome.Rejected,
                ZLinkActorJoinOutcome.Relayed {

    record Accepted(ActorRef actor, ZLinkMessage reply) implements ZLinkActorJoinOutcome {}

    record Relayed(java.util.concurrent.CompletionStage<Void> sourceCleanup)
            implements ZLinkActorJoinOutcome {}

    record Rejected(ZLinkMessage reply) implements ZLinkActorJoinOutcome {}
}
