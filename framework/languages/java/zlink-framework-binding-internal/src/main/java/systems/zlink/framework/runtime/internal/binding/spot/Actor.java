/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import java.time.Duration;
import java.util.List;

/** A mesh actor: an addressable, relocatable unit of ownership. */
public interface Actor extends AutoCloseable {
    /** Returns this actor's reference. */
    ActorRef ref();

    /** Joins the given spot. */
    OperationId joinSpot(RoutingId targetNodeRid, RoutingId targetSpotId,
                         long targetSpotGeneration, List<Message> creationParts,
                         Duration timeout);

    /** Joins the entry spot of the given node. */
    OperationId joinEntrySpot(RoutingId targetNodeRid, List<Message> creationParts,
                              Duration timeout);

    /** Leaves the currently joined spot. */
    OperationId leaveSpot(long expectedMembershipEpoch, Duration timeout);

    /** Sends a message to another actor. */
    void sendTo(ActorRef target, List<Message> parts, SendFlags flags);

    /** Sends a request to another actor. */
    OperationId requestTo(ActorRef target, List<Message> parts, SendFlags flags,
                          Duration timeout);

    /** Sends a message to this actor's bound STREAM session. */
    void sendBoundSession(List<Message> parts, SendFlags flags);

    /** Closes this actor's bound STREAM session. */
    OperationId closeBoundSession(long expectedBindingGeneration, Duration timeout);

    /** Destroys this actor. */
    OperationId destroy(Duration timeout);

    @Override
    void close();
}
