package systems.zlink.framework.actors;

import java.util.Objects;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.messaging.ZLinkMessage;

public sealed interface ZLinkActorJoinCompletion
    permits ZLinkActorJoinCompletion.Accepted,
            ZLinkActorJoinCompletion.Rejected,
            ZLinkActorJoinCompletion.Failed {

    record Accepted(
        ZLinkActorJoinOperationId operationId,
        ActorRef actor,
        ZLinkMessage reply) implements ZLinkActorJoinCompletion {
        public Accepted {
            Objects.requireNonNull(operationId, "operationId");
            Objects.requireNonNull(actor, "actor");
            reply = reply == null ? ZLinkMessage.empty() : reply;
        }
    }

    record Rejected(
        ZLinkActorJoinOperationId operationId,
        ZLinkMessage reply) implements ZLinkActorJoinCompletion {
        public Rejected {
            Objects.requireNonNull(operationId, "operationId");
            reply = reply == null ? ZLinkMessage.empty() : reply;
        }
    }

    record Failed(
        ZLinkActorJoinOperationId operationId,
        ZLinkFrameworkErrorKind kind) implements ZLinkActorJoinCompletion {
        public Failed {
            Objects.requireNonNull(operationId, "operationId");
            Objects.requireNonNull(kind, "kind");
        }
    }
}
