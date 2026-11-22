package systems.zlink.framework.runtime.messaging;

import systems.zlink.framework.testutil.PerCallAllocation;

import java.nio.ByteBuffer;
import java.util.Map;

/** Runs the allocation measurements in an isolated JVM with escape analysis disabled. */
public final class ZLinkChannelHeaderAllocationProbe {
    static final int CALLS_PER_WINDOW = 1_000;
    private static final int WARMUP_CALLS = 50_000;

    private ZLinkChannelHeaderAllocationProbe() {}

    public static void main(String[] args) {
        var header =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "orders",
                        "Move",
                        "application/json",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of("tenant", "alpha"),
                        null,
                        null);
        byte[] wire = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        long encodeBytes =
                PerCallAllocation.minWindow(
                        () -> ZLinkChannelEnvelope.encodeHeaderBytes(header),
                        CALLS_PER_WINDOW,
                        WARMUP_CALLS);
        long decodeBytes =
                PerCallAllocation.minWindow(
                        () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(wire), false),
                        CALLS_PER_WINDOW,
                        WARMUP_CALLS);

        System.out.println(encodeBytes + " " + decodeBytes);
    }
}
