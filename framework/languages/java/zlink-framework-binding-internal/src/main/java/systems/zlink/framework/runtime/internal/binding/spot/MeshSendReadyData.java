/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/**
 * Identifies the exact Mesh destination whose send capacity became available.
 * Fields that do not apply to {@link #destinationKind()} contain their empty
 * contract value.
 */
public record MeshSendReadyData(
    MeshDestinationKind destinationKind,
    RoutingId targetNodeRid,
    RoutingId targetSpotId,
    ActorRef targetActor,
    String channelName) implements MeshRecordPayload {
}
