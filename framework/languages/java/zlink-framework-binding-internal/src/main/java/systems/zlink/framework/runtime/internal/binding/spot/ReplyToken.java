/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

/**
 * An opaque handle that authorizes a single reply to a received request or
 * actor-join. Obtained from a {@link ReceiveRecord}; consumed by
 * {@link Dispatch#reply} or {@link Dispatch#actorJoinReply}.
 */
public final class ReplyToken {
    private final byte[] opaque;
    private final AtomicReference<ReplySender> sender;

    ReplyToken(byte[] opaque) {
        this(opaque, null);
    }

    ReplyToken(byte[] opaque, ReplySender sender) {
        this.opaque = Objects.requireNonNull(opaque, "opaque").clone();
        this.sender = new AtomicReference<>(sender);
    }

    byte[] opaque() {
        return opaque.clone();
    }

    void reply(ActorJoinDecision decision, List<Message> parts, SendFlags flags) {
        ReplySender claimed = sender.getAndSet(null);
        if (claimed == null) {
            throw new IllegalStateException("reply token has already been consumed");
        }
        claimed.send(decision, parts, flags);
    }

    @FunctionalInterface
    interface ReplySender {
        void send(ActorJoinDecision decision, List<Message> parts, SendFlags flags);
    }
}
