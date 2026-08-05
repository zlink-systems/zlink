package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.util.List;
import java.util.Objects;

public record ZLinkPlacementCapacity(
    ZLinkCapacityUsage actors,
    ZLinkCapacityUsage spots,
    List<ZLinkSpotTypeCapacity> spotTypes) {
    public ZLinkPlacementCapacity {
        Objects.requireNonNull(actors, "actors");
        Objects.requireNonNull(spots, "spots");
        spotTypes = List.copyOf(
            Objects.requireNonNull(spotTypes, "spotTypes"));
    }
}
