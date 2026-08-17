/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RoutedSendOperation;

/** Routes messages to peers addressed by routing id; the request/reply server side. */
public interface RouterSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    /**
     * Disconnects only the physical transport pair identified by a monitor
     * event from this router.
     */
    void disconnectTransportPair(long transportPairId,
                                 long transportPairGeneration);
    void setRoutingId(RoutingId rid);
    RoutingId getRoutingId();
    RoutedSendOperation send(RoutingId rid);
    /**
     * Creates a send operation pinned to the specified physical transport
     * pair. The pair identity must come from a monitor event for
     * this router; a stale or unknown pair is rejected by the native socket.
     */
    RoutedSendOperation send(RoutingId rid,
                             long transportPairId,
                             long transportPairGeneration);
    boolean recv(Received result, RecvFlags flags);
    /**
     * Receives for a Framework backend and retains each physical part's
     * origin Core HWM credit until {@code result} is closed or reused.
     */
    boolean recvRetained(Received result, RecvFlags flags);
    RequestOperation request(RoutingId rid);
    /**
     * Creates a request operation pinned to the specified physical transport
     * pair. The pair identity must come from a monitor event for this router;
     * a stale or unknown pair is rejected by the native socket.
     */
    RequestOperation request(RoutingId rid,
                             long transportPairId,
                             long transportPairGeneration);
    ReplyOperation reply(RoutingId rid, long requestSequence);
    @Override RouterSocketOptions options();
}
