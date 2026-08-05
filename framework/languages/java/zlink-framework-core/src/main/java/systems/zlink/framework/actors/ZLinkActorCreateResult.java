package systems.zlink.framework.actors;

import systems.zlink.framework.messaging.ZLinkMessage;

public sealed interface ZLinkActorCreateResult
    permits ZLinkActorCreateResult.Existing,
            ZLinkActorCreateResult.Created,
            ZLinkActorCreateResult.Rejected {
    record Existing(ActorRef actor) implements ZLinkActorCreateResult {
    }

    record Created(ActorRef actor, ZLinkMessage reply) implements ZLinkActorCreateResult {
    }

    record Rejected(ZLinkMessage reply) implements ZLinkActorCreateResult {
    }
}
