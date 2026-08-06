package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.spots.ZLinkSpotContext;

/** Uses the normal User Spot lifecycle with a separate factory type for capacity tests. */
public final class CapacitySpot extends UserSpot {
    public CapacitySpot(ZLinkSpotContext context, ScenarioState evidence) {
        super(context, evidence);
    }
}
