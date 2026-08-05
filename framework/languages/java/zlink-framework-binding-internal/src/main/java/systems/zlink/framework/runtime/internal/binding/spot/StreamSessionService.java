/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.StreamSocket;
import java.time.Duration;
import java.util.List;

/** Binds STREAM sessions to actors and relays messages between them. */
interface StreamSessionService extends AutoCloseable {
    /** Creates a STREAM session service over the given node and stream socket. */
    static StreamSessionService create(MeshNode node, StreamSocket stream) {
        throw new UnsupportedOperationException(
            "STREAM session service is provided by the Framework stream runtime");
    }

    /** Starts the service. */
    void start();

    /** Shuts the service down, waiting up to the given timeout. */
    void shutdown(Duration timeout);

    /** Returns a status snapshot of the service. */
    StreamSessionStatus status();

    /** Binds the given session to the given actor. */
    OperationId bindActor(RoutingId sessionRid, ActorRef actor, Duration timeout);

    /** Unbinds the given actor from the given session. */
    OperationId unbindActor(RoutingId sessionRid, ActorRef actor,
                            long expectedBindingGeneration, Duration timeout);

    /** Returns the bindings for the given session. */
    List<StreamSessionBinding> bindings(RoutingId sessionRid);

    /** Sends a message to a bound actor over the given session. */
    void sendToActor(RoutingId sessionRid, ActorRef actor, List<Message> parts,
                     SendFlags flags);

    /** Sends a request to a bound actor over the given session. */
    OperationId requestToActor(RoutingId sessionRid, ActorRef actor, List<Message> parts,
                               SendFlags flags, Duration timeout);

    @Override
    void close();
}
