package systems.zlink.framework.locations;

public record ZLinkCapacityUsage(int active, int reserved, int limit) {
    public ZLinkCapacityUsage {
        if (active < 0 || reserved < 0 || limit < 0) {
            throw new IllegalArgumentException("capacity usage and limit must be non-negative");
        }
    }

    /**
     * Whether {@code required} more objects fit: active and reserved slots plus the new ones stay
     * within the limit, and limit {@code 0} skips the check (MeshNode §5.1).
     */
    public boolean hasRoomFor(int required) {
        return limit == 0 || (long) active + reserved + required <= limit;
    }
}
