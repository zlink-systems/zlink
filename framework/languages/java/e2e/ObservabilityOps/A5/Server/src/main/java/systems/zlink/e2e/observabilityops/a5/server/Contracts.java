package systems.zlink.e2e.observabilityops.a5.server;

public final class Contracts {
    public static final String MESH = "observability.ops.a5";
    public static final String CHANNEL = "observability.ops.a5.request";

    private Contracts() {
    }

    public record ProbeReq(String value, boolean fail) {
    }

    public record ProbeRes(String value) {
    }
}
