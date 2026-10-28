package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Counts temporary header buffer views separately from the owned wire bytes. */
public final class ZLinkEnvelopePartViewAllocationTest {
    private static final int SENDS = 100;
    private static final byte[] BODY = new byte[1024];
    private static final ZLinkChannelEnvelope.Header HEADER =
            ZLinkChannelEnvelope.create(
                    ZLinkChannelEnvelope.KIND_COMMAND,
                    "orders",
                    "Update",
                    "application/json",
                    null,
                    Map.of(),
                    null);
    private static volatile Object observed;

    @Test
    void outboundHeadersDoNotRetainOrDuplicateHeapViews() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    @Test
    void directAndMaterializedEnvelopesHaveIdenticalWireBytes() {
        for (var metadata : List.of(Map.<String, String>of(), Map.of("키", "a\"\\\n값"))) {
            var header =
                    ZLinkChannelEnvelope.create(
                            ZLinkChannelEnvelope.KIND_COMMAND,
                            "채널",
                            "Update😀",
                            "application/json",
                            "topic",
                            metadata,
                            null);
            byte[] expectedHeader = ZLinkChannelEnvelope.encodeHeaderBytes(header);
            for (int size : List.of(0, 1024)) {
                try (Message referenceHeader = Message.from(expectedHeader);
                        Message referenceBody = Message.from(new byte[size]);
                        Message reference =
                                ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(
                                        List.of(referenceHeader, referenceBody))) {
                    var parts = ZLinkChannelEnvelope.encode(header, Message.from(new byte[size]));
                    try (Message actual =
                            ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts)) {
                        assertArrayEquals(reference.toByteArray(), actual.toByteArray());
                        byte[] actualHeader = new byte[expectedHeader.length];
                        ZLinkFrameworkMultipartView.buffer(parts, 0).get(actualHeader);
                        assertArrayEquals(expectedHeader, actualHeader);
                    } finally {
                        ZLinkFrameworkMultipartView.closeAll(parts);
                    }
                }
            }
        }
    }

    @Test
    void headerReadsAndMaterializationKeepTheSameWireBytes() {
        Message body = Message.from(BODY);
        var parts = ZLinkChannelEnvelope.encode(HEADER, body);
        try (Message before = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts)) {
            ByteBuffer first = ZLinkFrameworkMultipartView.buffer(parts, 0);
            ByteBuffer second = ZLinkFrameworkMultipartView.buffer(parts, 0);
            first.position(first.limit());
            assertEquals(0, second.position());
            Message header = parts.getFirst();
            assertSame(header, parts.getFirst());
            try (Message after = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts)) {
                assertArrayEquals(before.toByteArray(), after.toByteArray());
            }
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
        assertThrows(IllegalStateException.class, body::refCount);
    }

    @Test
    void invalidMetadataIsRejectedBeforeTakingTheBody() {
        var header =
                ZLinkChannelEnvelope.create(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "orders",
                        "Update",
                        "application/json",
                        null,
                        Map.of("key", "\0"),
                        null);
        try (Message body = Message.from(BODY)) {
            assertThrows(
                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                    () -> ZLinkChannelEnvelope.encode(header, body));
            assertEquals(BODY.length, body.size());
        }
    }

    public static long[] measureSendPath(String path, int count) {
        var bean =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        long bytes = bean.getCurrentThreadAllocatedBytes();
        long cpu = bean.getCurrentThreadCpuTime();
        long time = System.nanoTime();
        for (int i = 0; i < count; i++) sendFrame();
        return new long[] {
            System.nanoTime() - time,
            bean.getCurrentThreadAllocatedBytes() - bytes,
            bean.getCurrentThreadCpuTime() - cpu
        };
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            ZLinkServiceSendHeaderAllocationTest.benchmarkPaths(
                    java.nio.file.Path.of(args[1]),
                    ZLinkEnvelopePartViewAllocationTest.class,
                    List.of("frame"),
                    20_000);
            return;
        }
        for (int i = 0; i < 200; i++) sendFrame();
        Path file = Files.createTempFile("zlink-envelope-frame-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                control();
                for (int i = 0; i < SENDS; i++) sendFrame();
                recording.stop();
                recording.dump(file);
            }
            long arrays = 0,
                    views = 0,
                    writerViews = 0,
                    copies = 0,
                    controlArrays = 0,
                    controlViews = 0;
            Map<String, Long> copyObjects = new HashMap<>();
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    var stack = event.getStackTrace();
                    if (stack == null) continue;
                    String type = event.getClass("objectClass").getName();
                    boolean envelope = false, copy = false, control = false, send = false;
                    String allocationOwner = null;
                    for (var frame : stack.getFrames()) {
                        String owner = frame.getMethod().getType().getName();
                        String method = frame.getMethod().getName();
                        if (allocationOwner == null && owner.startsWith("systems.zlink.framework."))
                            allocationOwner = owner;
                        envelope |= owner.equals(ZLinkChannelEnvelope.class.getName());
                        copy |= owner.equals(Message.class.getName()) && method.equals("copyTo");
                        control |=
                                owner.equals(ZLinkEnvelopePartViewAllocationTest.class.getName())
                                        && method.equals("control");
                        send |=
                                owner.equals(ZLinkEnvelopePartViewAllocationTest.class.getName())
                                        && method.equals("sendFrame");
                    }
                    if (control && type.equals("[B")) controlArrays++;
                    if (control && type.equals("java.nio.HeapByteBuffer")) controlViews++;
                    if (send && envelope && type.equals("[B")) arrays++;
                    if (send
                            && ZLinkChannelEnvelope.class.getName().equals(allocationOwner)
                            && type.equals("java.nio.HeapByteBuffer")) writerViews++;
                    if (send
                            && ZLinkFrameworkMultipartView.class.getName().equals(allocationOwner)
                            && type.equals("java.nio.HeapByteBuffer")) views++;
                    if (send && copy) {
                        copyObjects.merge(type, 1L, Long::sum);
                        if (type.equals("jdk.internal.foreign.NativeMemorySegmentImpl")) copies++;
                    }
                }
            }
            System.out.printf(
                    "sends=%d headerArrays=%d retainedViews=%d writerViews=%d payloadCopies=%d controlArrays=%d controlViews=%d copyObjects=%s%n",
                    SENDS,
                    arrays,
                    views,
                    writerViews,
                    copies,
                    controlArrays,
                    controlViews,
                    copyObjects);
            assertEquals(SENDS, controlArrays);
            assertEquals(SENDS, controlViews);
            assertEquals(SENDS, arrays, "one owned header byte array is retained for each send");
            assertEquals(SENDS, copies, "one payload copy into the final native frame");
            assertEquals(0, views, "retained header bytes need no temporary heap buffer");
            assertEquals(0, writerViews, "header bytes need no temporary heap buffer for encoding");
        } finally {
            Files.deleteIfExists(file);
        }
    }

    private static void control() {
        for (int i = 0; i < SENDS; i++) observed = ByteBuffer.allocate(64).array();
    }

    private static void sendFrame() {
        var parts = ZLinkChannelEnvelope.encode(HEADER, Message.from(BODY));
        try (Message frame = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts)) {
            observed = frame;
            assertTrue(frame.size() > BODY.length);
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }
}
