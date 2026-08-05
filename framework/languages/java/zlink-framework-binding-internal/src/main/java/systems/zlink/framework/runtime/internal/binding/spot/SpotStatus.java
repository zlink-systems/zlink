/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/** A point-in-time status snapshot of a spot. */
public record SpotStatus(String spotId, SpotKind kind, long lifecycleGeneration,
                         long pendingApplicationMessages, long pendingInfrastructureMessages,
                         long pendingBytes, int activeActorCount, boolean draining,
                         int lastError, long lastChangedMs) {
}
