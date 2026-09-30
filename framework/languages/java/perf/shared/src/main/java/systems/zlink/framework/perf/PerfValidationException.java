package systems.zlink.framework.perf;

// A harness (not Framework) failure: identity, payload, correlation or phase validation (§14.2 harness keys).
public final class PerfValidationException extends RuntimeException {
    private static final long serialVersionUID = 1L;
    private final String kind;

    public PerfValidationException(String kind, String message) {
        super(message);
        this.kind = kind;
    }

    public String kind() {
        return kind;
    }
}
