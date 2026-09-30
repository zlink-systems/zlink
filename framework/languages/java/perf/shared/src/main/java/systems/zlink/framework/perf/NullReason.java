package systems.zlink.framework.perf;

// §15.5: every null metric, histogram or window value names its reason. `owner` and `lowerBoundMs` are always written
// (null when unused), like the .NET original, so the two languages' originals have the same key set.
public record NullReason(String code, String reason, String owner, Double lowerBoundMs) {
    public NullReason(String code, String reason) {
        this(code, reason, "perf/README.ko.md", null);
    }

    public NullReason(String code, String reason, String owner) {
        this(code, reason, owner, null);
    }
}
