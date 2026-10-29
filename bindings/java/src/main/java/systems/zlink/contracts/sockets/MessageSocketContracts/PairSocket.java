/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;

import java.util.List;

/** An exclusive one-to-one peering socket with no routing. */
public interface PairSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);

    /** Copies the current Core connection-attempt facts and acknowledges their readiness. */
    List<ConnectionAttempt> connectionAttemptsSnapshot();

    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    SendOperation send();
    boolean recv(Received result, RecvFlags flags);
}
