/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/** A point-in-time status snapshot of a mesh node. */
public record MeshNodeStatus(MeshNodeState state, RoutingId routingId, String meshName,
                             String localEndpoint, long lifecycleGeneration,
                             long descriptorRevision, int channelCount,
                             int configuredPeerCount, int admittedPeerCount,
                             int drainingPeerCount, long pendingApplicationMessages,
                             long pendingInfrastructureMessages, long pendingBytes,
                             int lastError, long lastChangedMs) {
}
