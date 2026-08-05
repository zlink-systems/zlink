package systems.zlink.framework.runtime.actors;

import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.messaging.ZLinkMessage;

final class ZLinkActorJoinResults {
    private ZLinkActorJoinResults() {
    }

    static ZLinkActorJoinOutcome decode(
        ZLinkMessageSerializer serializer,
        int joinResultCode,
        ZLinkBackendActorRef actor,
        String meshName,
        List<Message> replyParts) {
        try {
            ZLinkMessage reply = replyParts.isEmpty() || replyParts.get(0).size() == 0
                ? ZLinkMessage.empty()
                : ZLinkMessage.fromEncoded(
                    ZLinkEncodedPayload.from(replyParts.get(0).toByteArray()),
                    serializer);
            return joinResultCode == 0
                ? new ZLinkActorJoinOutcome.Accepted(
                    ZLinkActorRuntime.toPublicActorRef(actor, meshName), reply)
                : new ZLinkActorJoinOutcome.Rejected(reply);
        } finally {
            closeReplyParts(replyParts);
        }
    }

    private static void closeReplyParts(List<Message> replyParts) {
        replyParts.forEach(Message::close);
    }
}
