/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;

/**
 * The current membership location of an actor.
 *
 * @param actor the actor reference
 * @param spotId the spot the actor is a member of, if any
 * @param spotGeneration the joined spot's lifecycle generation
 * @param membershipEpoch the actor's membership epoch within the spot
 */
public record ActorLocation(ActorRef actor, String spotId, long spotGeneration,
                            long membershipEpoch) {
}
