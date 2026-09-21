package systems.zlink.framework.spots;

public sealed interface SpotHandle permits FrameworkSpotHandle {
    String meshName();

    String spotId();
}
