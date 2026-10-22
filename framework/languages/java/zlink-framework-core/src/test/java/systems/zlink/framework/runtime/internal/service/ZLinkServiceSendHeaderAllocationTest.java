package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;

/** Counts temporary heap buffer views, separately from the required wire array. */
public final class ZLinkServiceSendHeaderAllocationTest {
    private static final int SENDS = 100;
    private static volatile byte[] observed;

    @Test
    void sendHeadersAllocateOnlyTheirWireStorage() throws Exception {
        runInAllocationJvm(ZLinkServiceSendHeaderAllocationTest.class);
    }

    public static void runInAllocationJvm(Class<?> fixture) throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = fixture.getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
            }
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        Path output = Files.createTempFile("zlink-send-header-", ".log");
        try {
            var process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                                    "-XX:-UseTLAB",
                                    "-XX:-DoEscapeAnalysis",
                                    "-XX:ActiveProcessorCount=2",
                                    "-cp",
                                    String.join(java.io.File.pathSeparator, classpath),
                                    fixture.getName())
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            int exit = process.waitFor();
            String log = Files.readString(output);
            System.out.print(log);
            assertEquals(0, exit, log);
        } finally {
            Files.deleteIfExists(output);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            systems.zlink.framework.runtime.channels.ZLinkSendAdmissionAllocationTest
                    .benchmarkPaths(
                            Path.of(args[1]),
                            ZLinkServiceSendHeaderAllocationTest.class,
                            List.of("node", "channel"),
                            100_000);
            return;
        }
        var codec = new ZLinkServiceM6AWireCodec();
        for (int i = 0; i < 200; i++) {
            observed = codec.encodeNodeSendHeader(0);
            observed = codec.encodeChannelSendHeader("orders", 0);
        }
        long control =
                buffers(
                        () -> {
                            for (int i = 0; i < SENDS; i++)
                                observed = ByteBuffer.allocate(5).array();
                        });
        long node =
                buffers(
                        () -> {
                            for (int i = 0; i < SENDS; i++)
                                observed = codec.encodeNodeSendHeader(0);
                        });
        long channel =
                buffers(
                        () -> {
                            for (int i = 0; i < SENDS; i++)
                                observed = codec.encodeChannelSendHeader("orders", 0);
                        });
        System.out.printf(
                "sends=%d controlBuffers=%d nodeBuffers=%d channelBuffers=%d%n",
                SENDS, control, node, channel);
        assertEquals(SENDS, control, "the same-JVM heap buffer is the positive control");
        assertEquals(0, node, "node send requires a wire array, without a temporary heap buffer");
        assertEquals(
                0, channel, "channel send requires a wire array, without a temporary heap buffer");
    }

    public static long[] measureSendPath(String path, int count) {
        var bean =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        var codec = new ZLinkServiceM6AWireCodec();
        long bytes = bean.getCurrentThreadAllocatedBytes();
        long cpu = bean.getCurrentThreadCpuTime();
        long time = System.nanoTime();
        for (int i = 0; i < count; i++)
            observed =
                    path.equals("node")
                            ? codec.encodeNodeSendHeader(0)
                            : codec.encodeChannelSendHeader("orders", 0);
        return new long[] {
            System.nanoTime() - time,
            bean.getCurrentThreadAllocatedBytes() - bytes,
            bean.getCurrentThreadCpuTime() - cpu
        };
    }

    private static long buffers(Runnable action) throws Exception {
        Path file = Files.createTempFile("zlink-send-header-", ".jfr");
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
                    if (event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            && event.getClass("objectClass")
                                    .getName()
                                    .equals("java.nio.HeapByteBuffer")
                            && event.getStackTrace() != null
                            && event.getStackTrace().getFrames().stream()
                                    .anyMatch(
                                            frame ->
                                                    frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .equals(
                                                                            ZLinkServiceSendHeaderAllocationTest
                                                                                    .class
                                                                                    .getName())
                                                            && frame.getMethod()
                                                                    .getName()
                                                                    .startsWith("lambda$main$")))
                        count++;
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
