/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/** A single mesh peer entry. */
public record MeshPeerEntry(RoutingId routingId, String endpoint, long connectionIntentId,
                            MeshPeerSource source, MeshPeerState state,
                            long lifecycleGeneration, long descriptorRevision,
                            int channelCount, int lastError, long lastChangedMs) {
}
