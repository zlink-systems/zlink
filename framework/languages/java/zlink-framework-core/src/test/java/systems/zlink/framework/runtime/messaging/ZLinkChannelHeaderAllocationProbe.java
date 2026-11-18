package systems.zlink.framework.runtime.messaging;

import com.sun.management.ThreadMXBean;

import java.lang.management.ManagementFactory;
import java.nio.ByteBuffer;
import java.util.Map;

/** Runs the allocation window in an isolated JVM with escape analysis disabled. */
public final class ZLinkChannelHeaderAllocationProbe {
    private ZLinkChannelHeaderAllocationProbe() {}

    public static void main(String[] args) {
        ThreadMXBean bean = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        if (!bean.isThreadAllocatedMemorySupported())
            throw new IllegalStateException("thread allocated memory is not supported");
        if (!bean.isThreadAllocatedMemoryEnabled()) bean.setThreadAllocatedMemoryEnabled(true);

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
        int checksum = 0;
        for (int i = 0; i < 50_000; i++) {
            wire = ZLinkChannelEnvelope.encodeHeaderBytes(header);
            checksum += ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(wire), false).kind();
        }

        long threadId = Thread.currentThread().threadId();
        long beforeEncode = bean.getThreadAllocatedBytes(threadId);
        for (int i = 0; i < 1_000; i++)
            checksum += ZLinkChannelEnvelope.encodeHeaderBytes(header).length;
        long encodeBytes = bean.getThreadAllocatedBytes(threadId) - beforeEncode;

        long beforeDecode = bean.getThreadAllocatedBytes(threadId);
        for (int i = 0; i < 1_000; i++)
            checksum += ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(wire), false).kind();
        long decodeBytes = bean.getThreadAllocatedBytes(threadId) - beforeDecode;
        if (checksum <= 0) throw new IllegalStateException("allocation probe checksum is invalid");

        System.out.println(encodeBytes + " " + decodeBytes);
    }
}
