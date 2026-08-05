/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/** One event received from a MeshNode monitor. */
public record MeshMonitorEvent(
    MeshMonitorEventKind kind,
    long timestampMs,
    long meshLifecycleGeneration,
    long meshDescriptorRevision,
    MeshNodeState meshState,
    RoutingId peerRid,
    long peerLifecycleGeneration,
    long peerDescriptorRevision,
    /** Owner kind, or {@code null} when the event is not associated with an owner. */
    OwnerKind ownerKind,
    String spotId,
    ActorRef actor,
    String channelName,
    OperationId operationId,
    int resultCode,
    int failureErrno) {
}
