package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.nio.file.Files;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

/** Admission cleanup releases the retained envelope without materializing its header. */
public final class ZLinkOneWayEnvelopeCleanupTest {
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
    void cleanupDoesNotCreateNativeMessages() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    @Test
    void immediateAdmissionReleasesTheBodyAndPreservesItsMarker() {
        release(0);
    }

    @Test
    void pendingAdmissionKeepsTheBodyUntilSuccess() {
        release(1);
    }

    @Test
    void pendingAdmissionKeepsTheBodyUntilFailure() {
        release(2);
    }

    public static long[] measureSendPath(String path, int count) {
        var bean =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        long bytes = bean.getCurrentThreadAllocatedBytes();
        long cpu = bean.getCurrentThreadCpuTime();
        long time = System.nanoTime();
        for (int i = 0; i < count; i++) {
            var parts = ZLinkChannelEnvelope.encode(HEADER, Message.from(BODY));
            observed =
                    ZLinkOneWayCalls.closeAfterAdmission(
                            ZLinkOneWayCalls.immediateAdmission(), parts);
        }
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
                    ZLinkOneWayEnvelopeCleanupTest.class,
                    java.util.List.of("cleanup"),
                    20_000);
            return;
        }
        for (int i = 0; i < 200; i++) for (int mode = 0; mode < 3; mode++) release(mode);
        var file = Files.createTempFile("zlink-envelope-cleanup-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                control();
                for (int i = 0; i < SENDS; i++) for (int mode = 0; mode < 3; mode++) release(mode);
                recording.stop();
                recording.dump(file);
            }
            long created = 0, control = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || !event.getClass("objectClass")
                                    .getName()
                                    .equals(Message.class.getName())
                            || event.getStackTrace() == null) continue;
                    for (var frame : event.getStackTrace().getFrames()) {
                        if (frame.getMethod()
                                .getType()
                                .getName()
                                .equals(ZLinkOneWayCalls.class.getName())) {
                            created++;
                            break;
                        }
                        if (frame.getMethod()
                                        .getType()
                                        .getName()
                                        .equals(ZLinkOneWayEnvelopeCleanupTest.class.getName())
                                && frame.getMethod().getName().equals("control")) {
                            control++;
                            break;
                        }
                    }
                }
            }
            System.out.printf(
                    "admissions=%d cleanupMessages=%d controlMessages=%d%n",
                    SENDS * 3, created, control);
            assertEquals(SENDS, control);
            assertEquals(0, created, "releasing an envelope must not create a native message");
        } finally {
            Files.deleteIfExists(file);
        }
    }

    private static void control() {
        for (int i = 0; i < SENDS; i++)
            try (Message message = Message.from(BODY)) {
                observed = message;
            }
    }

    private static void release(int mode) {
        try (Message body = Message.from(BODY)) {
            var parts = ZLinkChannelEnvelope.encode(HEADER, body);
            var pending = new CompletableFuture<Void>();
            var submission = mode == 0 ? ZLinkOneWayCalls.immediateAdmission() : pending;
            var released = ZLinkOneWayCalls.closeAfterAdmission(submission, parts);
            if (mode == 0) {
                assertSame(submission, released);
            } else {
                assertFalse(released.toCompletableFuture().isDone());
                assertEquals(BODY.length, body.size());
                if (mode == 1) {
                    pending.complete(null);
                    released.toCompletableFuture().join();
                } else {
                    var failure = new IllegalStateException("admission failed");
                    pending.completeExceptionally(failure);
                    assertSame(
                            failure,
                            assertThrows(
                                            CompletionException.class,
                                            () -> released.toCompletableFuture().join())
                                    .getCause());
                }
            }
            assertThrows(IllegalStateException.class, body::refCount);
        }
    }
}
