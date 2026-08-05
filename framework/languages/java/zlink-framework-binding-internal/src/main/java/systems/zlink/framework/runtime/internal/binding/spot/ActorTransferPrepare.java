/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

/** Inputs to the formal Core actor transfer prepare operation. */
public record ActorTransferPrepare(
    ActorTransferRole role,
    ActorTransferId transferId,
    ActorRef actor,
    long expectedMembershipEpoch,
    RoutingId peerNodeRid,
    long finalSequence,
    long reserveMessageCount,
    long reserveByteCount) {

    public ActorTransferPrepare {
        Objects.requireNonNull(role, "role");
        Objects.requireNonNull(transferId, "transferId");
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(peerNodeRid, "peerNodeRid");
    }
}
