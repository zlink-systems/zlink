/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;
import java.util.Objects;

/**
 * References an actor: the node hosting it, its id, and its generation.
 * @param nodeRid the routing id of the node hosting the actor
 * @param actorId the actor's identifier
 * @param generation the actor's generation counter; 0 indicates an unchecked ref
 */
public record ActorRef(RoutingId nodeRid, String actorId, long generation) {
    public ActorRef {
        Objects.requireNonNull(nodeRid, "nodeRid");
        Objects.requireNonNull(actorId, "actorId");
    }

    boolean unchecked() {
        return generation == 0L;
    }
}
