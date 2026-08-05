/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/**
 * A single entry in a dispatch ready batch: an owner with pending work.
 *
 * @param ownerKind the owner kind (node, spot or actor)
 * @param domain the ready domain bit mask
 * @param spotId the owning spot's routing id, if applicable
 * @param actor the owning actor reference, if applicable
 */
public record ReadyRecord(OwnerKind ownerKind, int domain, String spotId,
                          ActorRef actor) {
}
