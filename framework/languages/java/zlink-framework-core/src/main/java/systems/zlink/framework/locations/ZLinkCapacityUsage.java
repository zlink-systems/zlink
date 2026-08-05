package systems.zlink.framework.locations;

public record ZLinkCapacityUsage(int active, int reserved, int limit) {
    public ZLinkCapacityUsage {
        if (active < 0
            || reserved < 0
            || limit < 0
            || limit > 0 && (long) active + reserved > limit) {
            throw new IllegalArgumentException(
                "capacity usage must be non-negative and within limit");
        }
    }
}
