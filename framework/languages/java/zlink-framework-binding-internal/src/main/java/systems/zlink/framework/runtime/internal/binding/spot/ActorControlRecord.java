/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/** The typed payload of an actor lifecycle Spot control record. */
public record ActorControlRecord(
    ActorLifecycleKind kind,
    ActorRef previousActor,
    ActorRef currentActor,
    RoutingId previousSpotId,
    RoutingId currentSpotId,
    long previousSpotGeneration,
    long currentSpotGeneration,
    long previousMembershipEpoch,
    long currentMembershipEpoch,
    int resultCode) implements MeshRecordPayload {
}
