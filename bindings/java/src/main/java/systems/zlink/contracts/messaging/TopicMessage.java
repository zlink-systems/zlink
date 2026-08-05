/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.internal.ContractAccess;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/** Topic-aware recv result used by raw subscription paths. */
public final class TopicMessage implements AutoCloseable {
    private RoutingId routingId;
    private String topic;
    private List<Message> parts;
    private boolean closed;
    // Reusable single-element parts list for the subscribe hot path so a
    // steady stream of single-part deliveries does not allocate a fresh
    // immutable List per message (parity with Received.populateRoutedSinglePart
    // zero-realloc reuse).
    private ArrayList<Message> reusableSingle;
    private Message reusableSinglePart;

    static {
        ContractAccess.register(new ContractAccess.TopicMessageAccess() {
            @Override
            public TopicMessage create(RoutingId routingId, String topicId,
                                       Message[] parts) {
                return new TopicMessage(routingId, topicId, parts);
            }

            @Override
            public void adoptSingle(TopicMessage target, RoutingId routingId,
                                    String topicId, Message part) {
                target.adoptSingle(routingId, topicId, part);
            }

            @Override
            public Message prepareReusableSinglePart(TopicMessage target) {
                return target.prepareReusableSinglePart();
            }

            @Override
            public void adoptFrom(TopicMessage target, TopicMessage source) {
                target.adoptFrom(source);
            }
        });
    }

    public TopicMessage() {
        this(null, "", null);
    }

    TopicMessage(RoutingId routingId, String topicId, Message[] parts) {
        this.routingId = routingId;
        this.topic = topicId == null ? "" : topicId;
        this.parts = parts == null ? List.of() : List.of(parts);
    }

    // Hot-path adopt for the common single-part subscribe result. Avoids the
    // intermediate fresh TopicMessage + Message[] + List.of allocations that
    // adoptFrom(subscribe(...)) incurs per message.
    void adoptSingle(RoutingId routingId, String topicId, Message part) {
        closePartsExcept(part);
        ArrayList<Message> slot = reusableSingle;
        if (slot == null) {
            slot = new ArrayList<>(1);
            reusableSingle = slot;
        } else {
            slot.clear();
        }
        slot.add(part);
        this.routingId = routingId;
        this.topic = topicId == null ? "" : topicId;
        this.parts = slot;
        this.closed = false;
    }

    Message prepareReusableSinglePart() {
        Message part = reusableSinglePart;
        if (part == null || !ContractAccess.messageIsReusable(part)) {
            part = new Message();
            reusableSinglePart = part;
            return part;
        }
        ContractAccess.messageResetReusable(part);
        return part;
    }

    private void closePartsExcept(Message keep) {
        if (closed || parts == null || parts.isEmpty()) {
            return;
        }
        for (Message part : parts) {
            if (part == keep) {
                continue;
            }
            part.close();
        }
    }

    void adoptFrom(TopicMessage source) {
        if (source == this)
            return;
        close();
        this.routingId = source.routingId;
        this.topic = source.topic;
        this.parts = source.parts;
        this.closed = false;
        source.routingId = null;
        source.topic = "";
        source.parts = List.of();
        source.closed = true;
    }

    public Optional<RoutingId> getRoutingId() {
        return Optional.ofNullable(routingId);
    }

    public String topic() {
        return topic;
    }

    /** Returns the payload parts as an immutable view. */
    public List<Message> parts() {
        return parts;
    }

    /** Returns whether the payload contains exactly one part. */
    public boolean isSinglePart() {
        return parts.size() == 1;
    }

    /** Returns the first payload part. */
    public Message firstPart() {
        if (parts.isEmpty())
            throw new ZlinkRecvException(RecvResult.NO_DATA);
        return parts.get(0);
    }

    /** Returns the single payload part, or throws when the payload is multipart. */
    public Message singlePartOrThrow() {
        if (!isSinglePart()) {
            throw new ZlinkRecvException(RecvResult.NOT_SUPPORTED);
        }
        return parts.get(0);
    }

    @Override
    public void close() {
        if (closed)
            return;
        closed = true;
        Message.closeAll(parts);
        if (reusableSinglePart != null
            && (parts == null || !parts.contains(reusableSinglePart))) {
            reusableSinglePart.close();
        }
    }
}
