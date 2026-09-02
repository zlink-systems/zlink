/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.internal.ContractAccess;

/** Caller-owned reusable output for one framed STREAM packet. */
public final class StreamPacket implements AutoCloseable {
    private RoutingId routingId;
    private Message header;
    private Message body;
    private boolean receiving;

    static {
        ContractAccess.register(new ContractAccess.StreamPacketAccess() {
            @Override
            public void begin(StreamPacket packet) {
                packet.beginReceive();
            }

            @Override
            public void complete(StreamPacket packet, RoutingId routingId,
                                 Message header, Message body) {
                packet.completeReceive(routingId, header, body);
            }

            @Override
            public void fail(StreamPacket packet) {
                packet.failReceive();
            }
        });
    }

    public StreamPacket() {
    }

    public synchronized boolean isEmpty() {
        return routingId == null && header == null && body == null;
    }

    public synchronized Optional<RoutingId> routingId() {
        return Optional.ofNullable(routingId);
    }

    public synchronized Message header() {
        if (header == null)
            throw new IllegalStateException("stream packet is empty");
        return header;
    }

    public synchronized Message body() {
        if (body == null)
            throw new IllegalStateException("stream packet is empty");
        return body;
    }

    @Override
    public synchronized void close() {
        if (receiving)
            throw new IllegalStateException("stream packet receive is active");
        reset();
    }

    private synchronized void beginReceive() {
        if (receiving)
            throw new IllegalStateException("stream packet receive is active");
        reset();
        receiving = true;
    }

    private synchronized void completeReceive(RoutingId newRoutingId,
                                              Message newHeader,
                                              Message newBody) {
        if (!receiving)
            throw new IllegalStateException("stream packet receive is not active");
        routingId = newRoutingId;
        header = newHeader;
        body = newBody;
        receiving = false;
    }

    private synchronized void failReceive() {
        reset();
        receiving = false;
    }

    private void reset() {
        closeQuietly(header);
        closeQuietly(body);
        routingId = null;
        header = null;
        body = null;
    }

    private static void closeQuietly(Message message) {
        if (message == null)
            return;
        try {
            message.close();
        } catch (RuntimeException ignored) {
        }
    }
}
