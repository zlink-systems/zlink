package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.core.util.BufferRecycler;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.testsupport.ChildJvm;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Checks codec scratch storage across the short-lived workers used by serial dispatch. */
public final class ZLinkJsonBufferRecyclingTest {
    private static final int RECORDS = 32;
    private static volatile Object observed;

    @Test
    void shortLivedWorkersReuseJsonScratchBuffers() throws Exception {
        ChildJvm.runInAllocationJvm(getClass());
    }

    @Test
    void concurrentWorkersKeepTheirResultsAfterScratchStorageIsReleased() throws Exception {
        var serializer = new ZLinkJsonMessageSerializer();
        var start = new CountDownLatch(1);
        List<Future<List<ZLinkEncodedPayload>>> results = new ArrayList<>();
        try (var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            for (int worker = 0; worker < 8; worker++) {
                int id = worker;
                results.add(
                        workers.submit(
                                () -> {
                                    assertTrue(start.await(5, TimeUnit.SECONDS));
                                    List<ZLinkEncodedPayload> retained = new ArrayList<>();
                                    for (int round = 0; round < 40; round++) {
                                        var expected =
                                                new Payload(
                                                        id
                                                                + ":"
                                                                + round
                                                                + ":"
                                                                + "x"
                                                                        .repeat(
                                                                                round % 2 == 0
                                                                                        ? 1024
                                                                                        : 4096));
                                        var encoded = serializer.serialize(expected);
                                        retained.add(encoded);
                                        assertEquals(
                                                expected,
                                                serializer.deserialize(encoded, Payload.class));
                                        assertThrows(
                                                IllegalArgumentException.class,
                                                () ->
                                                        serializer.deserializeBorrowed(
                                                                ByteBuffer.wrap(
                                                                        "{\"value\":1,\"value\":2}"
                                                                                .getBytes(
                                                                                        java.nio
                                                                                                .charset
                                                                                                .StandardCharsets
                                                                                                .UTF_8)),
                                                                Payload.class));
                                    }
                                    return retained;
                                }));
            }
            start.countDown();
            for (int worker = 0; worker < results.size(); worker++) {
                List<ZLinkEncodedPayload> retained = results.get(worker).get(10, TimeUnit.SECONDS);
                for (int round = 0; round < retained.size(); round++) {
                    assertEquals(
                            new Payload(
                                    worker
                                            + ":"
                                            + round
                                            + ":"
                                            + "x".repeat(round % 2 == 0 ? 1024 : 4096)),
                            serializer.deserialize(retained.get(round), Payload.class));
                }
            }
        } finally {
            start.countDown();
        }
    }

    public static void main(String[] args) throws Exception {
        var serializer = new ZLinkJsonMessageSerializer();
        var value = new Payload("x".repeat(1012));
        byte[] json = serializer.serialize(value).bytes();
        assertEquals(1024, json.length);
        ByteBuffer input = ByteBuffer.allocateDirect(json.length).put(json).flip();
        Runnable roundTrip = () -> roundTrip(serializer, input, value);
        for (int i = 0; i < 100; i++) onWorker(roundTrip);
        Path file = Files.createTempFile("zlink-json-recycling-", ".jfr");
        try {
            Counts actual = record(file, () -> onWorkers(roundTrip));
            Counts control =
                    record(
                            file,
                            () -> {
                                for (int i = 0; i < RECORDS; i++) {
                                    var recycler = new BufferRecycler();
                                    observed =
                                            recycler.allocByteBuffer(
                                                    BufferRecycler.BYTE_READ_IO_BUFFER);
                                }
                            });
            System.out.printf(
                    "records=%d codecRecyclers=%d codecScratchArrays=%d codecScratchBytes=%d codecObjects=%d codecBytes=%d controlRecyclers=%d controlScratchArrays=%d%n",
                    RECORDS,
                    actual.recyclers,
                    actual.arrays,
                    actual.bytes,
                    actual.totalObjects,
                    actual.totalBytes,
                    control.recyclers,
                    control.arrays);
            assertEquals(
                    RECORDS, control.recyclers, "JFR must observe deliberately cold recyclers");
            assertEquals(RECORDS, control.arrays, "JFR must observe deliberately cold buffers");
            assertEquals(
                    0, actual.recyclers, "worker lifetime must not determine recycler lifetime");
            assertEquals(0, actual.arrays, "warmed serial codec scratch buffers must be reused");
        } finally {
            Files.deleteIfExists(file);
        }
    }

    private static void roundTrip(
            ZLinkJsonMessageSerializer serializer, ByteBuffer input, Payload value) {
        assertEquals(
                value, serializer.deserializeBorrowed(input.asReadOnlyBuffer(), Payload.class));
        observed = serializer.serialize(value);
    }

    private static void onWorkers(Runnable work) {
        for (int i = 0; i < RECORDS; i++) onWorker(work);
    }

    private static void onWorker(Runnable work) {
        var failure = new AtomicReference<Throwable>();
        Thread worker =
                Thread.ofVirtual()
                        .start(
                                () -> {
                                    try {
                                        work.run();
                                    } catch (Throwable error) {
                                        failure.set(error);
                                    }
                                });
        try {
            worker.join();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
        if (failure.get() != null) throw new AssertionError(failure.get());
    }

    private static Counts record(Path file, Runnable work) throws Exception {
        try (var recording = new Recording()) {
            recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
            recording.enable("jdk.DataLoss");
            recording.start();
            work.run();
            recording.stop();
            recording.dump(file);
        }
        long recyclers = 0, arrays = 0, bytes = 0, totalObjects = 0, totalBytes = 0;
        try (var events = new RecordingFile(file)) {
            while (events.hasMoreEvents()) {
                var event = events.readEvent();
                assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                    continue;
                String type = event.getClass("objectClass").getName();
                if (type.equals(BufferRecycler.class.getName())) recyclers++;
                var trace = event.getStackTrace();
                if (trace == null) continue;
                if (trace.getFrames().stream()
                        .anyMatch(
                                frame ->
                                        frame.getMethod()
                                                        .getType()
                                                        .getName()
                                                        .equals(
                                                                ZLinkJsonBufferRecyclingTest.class
                                                                        .getName())
                                                && frame.getMethod()
                                                        .getName()
                                                        .equals("roundTrip"))) {
                    totalObjects++;
                    totalBytes += event.getLong("allocationSize");
                }
                if (!type.equals("[B") && !type.equals("[C")) continue;
                if (trace.getFrames().stream()
                        .anyMatch(
                                frame ->
                                        frame.getMethod()
                                                        .getType()
                                                        .getName()
                                                        .equals(BufferRecycler.class.getName())
                                                && (frame.getMethod().getName().equals("balloc")
                                                        || frame.getMethod()
                                                                .getName()
                                                                .equals("calloc")))) {
                    arrays++;
                    bytes += event.getLong("allocationSize");
                }
            }
        }
        return new Counts(recyclers, arrays, bytes, totalObjects, totalBytes);
    }

    public record Payload(String value) {}

    private record Counts(
            long recyclers, long arrays, long bytes, long totalObjects, long totalBytes) {}
}
