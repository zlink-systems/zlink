package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;

/** Copying into a caller buffer preserves its bounds without a temporary view. */
public final class MessageCopyBufferContractTest {
    private static final int COPIES = 100;
    private static volatile Object observed;

    @Test
    void copiesAtThePositionAndPreservesLimitAndSurroundingBytes() {
        for (boolean direct : new boolean[] {false, true}) {
            ByteBuffer destination =
                    direct ? ByteBuffer.allocateDirect(12) : ByteBuffer.allocate(12);
            for (int index = 0; index < destination.capacity(); index++)
                destination.put(index, (byte) 9);
            destination.position(3).limit(10);
            try (Message message = Message.from(new byte[] {1, 2, 3, 4})) {
                assertEquals(4, message.copyTo(destination));
            }
            assertEquals(7, destination.position());
            assertEquals(10, destination.limit());
            byte[] actual = new byte[12];
            destination.clear().get(actual);
            assertArrayEquals(new byte[] {9, 9, 9, 1, 2, 3, 4, 9, 9, 9, 9, 9}, actual);
        }
    }

    @Test
    void rejectedCopiesPreserveThePositionAndEmptyCopiesDoNotWrite() {
        try (Message message = Message.from(new byte[] {1, 2, 3, 4});
                Message empty = Message.allocate(0)) {
            ByteBuffer small = ByteBuffer.allocate(8).position(3).limit(6);
            assertThrows(IllegalArgumentException.class, () -> message.copyTo(small));
            assertEquals(3, small.position());
            assertEquals(6, small.limit());
            ByteBuffer readOnly = ByteBuffer.allocate(8).asReadOnlyBuffer().position(2);
            assertThrows(IllegalArgumentException.class, () -> message.copyTo(readOnly));
            assertEquals(2, readOnly.position());
            assertEquals(0, empty.copyTo(readOnly));
            assertEquals(2, readOnly.position());
        }
    }

    @Test
    void copyUsesTheExistingDestinationView() throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = getClass().getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
            }
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        Path log = Files.createTempFile("zlink-copy-buffer-", ".log");
        try {
            var child =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-XX:-UseTLAB",
                                    "-XX:-DoEscapeAnalysis",
                                    "-XX:ActiveProcessorCount=2",
                                    "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                                    "-cp",
                                    String.join(java.io.File.pathSeparator, classpath),
                                    getClass().getName())
                            .redirectErrorStream(true)
                            .redirectOutput(log.toFile())
                            .start();
            int exit = child.waitFor();
            String output = Files.readString(log);
            System.out.print(output);
            assertEquals(0, exit, output);
        } finally {
            Files.deleteIfExists(log);
        }
    }

    public static void main(String[] args) throws Exception {
        var contracts = new MessageCopyBufferContractTest();
        contracts.copiesAtThePositionAndPreservesLimitAndSurroundingBytes();
        contracts.rejectedCopiesPreserveThePositionAndEmptyCopiesDoNotWrite();
        for (boolean direct : new boolean[] {false, true}) {
            ByteBuffer destination =
                    direct ? ByteBuffer.allocateDirect(16) : ByteBuffer.allocate(16);
            try (Message message = Message.from(new byte[] {1, 2, 3, 4})) {
                for (int index = 0; index < 200; index++) message.copyTo(destination.clear());
                long control =
                        views(
                                () -> {
                                    for (int index = 0; index < COPIES; index++)
                                        observed = destination.slice();
                                },
                                false);
                long actual =
                        views(
                                () -> {
                                    for (int index = 0; index < COPIES; index++)
                                        message.copyTo(destination.clear());
                                },
                                true);
                System.out.printf(
                        "direct=%s copies=%d controlViews=%d copyViews=%d%n",
                        direct, COPIES, control, actual);
                assertEquals(COPIES, control);
                assertEquals(
                        0, actual, "the destination already has the required position and limit");
            }
        }
    }

    public static long[] measureCopy(String path, int count) {
        ByteBuffer destination =
                path.equals("direct") ? ByteBuffer.allocateDirect(1024) : ByteBuffer.allocate(1024);
        var metrics =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        try (Message message = Message.from(new byte[1024])) {
            long bytes = metrics.getCurrentThreadAllocatedBytes();
            long cpu = metrics.getCurrentThreadCpuTime();
            for (int index = 0; index < count; index++) {
                message.copyTo(destination.clear());
            }
            return new long[] {
                metrics.getCurrentThreadCpuTime() - cpu,
                metrics.getCurrentThreadAllocatedBytes() - bytes
            };
        }
    }

    private static long views(Runnable action, boolean copy) throws Exception {
        Path file = Files.createTempFile("zlink-copy-buffer-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                action.run();
                recording.stop();
                recording.dump(file);
            }
            long count = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || !(event.getClass("objectClass")
                                            .getName()
                                            .equals("java.nio.HeapByteBuffer")
                                    || event.getClass("objectClass")
                                            .getName()
                                            .equals("java.nio.DirectByteBuffer"))
                            || event.getStackTrace() == null) continue;
                    String owner =
                            copy
                                    ? Message.class.getName()
                                    : MessageCopyBufferContractTest.class.getName();
                    if (event.getStackTrace().getFrames().stream()
                            .anyMatch(
                                    frame ->
                                            frame.getMethod().getType().getName().equals(owner)
                                                    && (!copy
                                                            || frame.getMethod()
                                                                    .getName()
                                                                    .equals("copyTo")))) count++;
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
