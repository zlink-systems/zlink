package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

public record ZLinkPlacementCapacityBundle(
    int actorSlots,
    int spotSlots,
    Optional<ZLinkSpotTypeCapacityDelta> spotType) {
    public ZLinkPlacementCapacityBundle {
        if (actorSlots < 0 || spotSlots < 0) {
            throw new IllegalArgumentException(
                "capacity bundle slots must not be negative");
        }
        spotType = Objects.requireNonNull(spotType, "spotType");
        if (spotType.isPresent() != (spotSlots > 0)) {
            throw new IllegalArgumentException(
                "spot slots and spot type capacity must be present together");
        }
        spotType.ifPresent(delta -> {
            if (delta.slots() != spotSlots) {
                throw new IllegalArgumentException(
                    "spot type slots must equal spotSlots");
            }
        });
    }

    public static ZLinkPlacementCapacityBundle actor(int slots) {
        return new ZLinkPlacementCapacityBundle(
            slots,
            0,
            Optional.empty());
    }

    public static ZLinkPlacementCapacityBundle spot(
        ZLinkPlacementObjectKind objectKind,
        String stableType,
        int slots) {
        return new ZLinkPlacementCapacityBundle(
            0,
            slots,
            Optional.of(new ZLinkSpotTypeCapacityDelta(
                objectKind,
                stableType,
                slots)));
    }
}
