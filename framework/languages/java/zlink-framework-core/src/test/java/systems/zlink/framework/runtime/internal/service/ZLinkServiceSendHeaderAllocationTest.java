package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
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
            benchmarkPaths(
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

    public static void benchmarkPaths(
            Path baselineClasses, Class<?> fixtureClass, List<String> paths, int count)
            throws Exception {
        var current = new ArrayList<URL>();
        for (String entry :
                System.getProperty("java.class.path").split(java.io.File.pathSeparator)) {
            current.add(Path.of(entry).toUri().toURL());
        }
        var original = new ArrayList<URL>();
        original.add(baselineClasses.toUri().toURL());
        original.addAll(current);
        try (var before = new FrameworkLoader(original.toArray(URL[]::new));
                var after = new FrameworkLoader(current.toArray(URL[]::new));
                var control = new FrameworkLoader(current.toArray(URL[]::new))) {
            var methods =
                    List.of(before, after, control).stream()
                            .map(
                                    loader -> {
                                        try {
                                            return loader.loadClass(fixtureClass.getName())
                                                    .getMethod(
                                                            "measureSendPath",
                                                            String.class,
                                                            int.class);
                                        } catch (ReflectiveOperationException failure) {
                                            throw new IllegalStateException(failure);
                                        }
                                    })
                            .toList();
            for (String path : paths) {
                for (int warmup = 0; warmup < 8; warmup++) {
                    for (var method : methods) method.invoke(null, path, count);
                }
                double[][][] values = new double[3][3][10];
                for (int round = 0; round < 10; round++) {
                    for (int step = 0; step < 3; step++) {
                        int variant = (round & 1) == 0 ? step : 2 - step;
                        long[] result = (long[]) methods.get(variant).invoke(null, path, count);
                        for (int metric = 0; metric < 3; metric++) {
                            values[variant][metric][round] = result[metric] / (double) count;
                        }
                    }
                    System.out.printf(
                            "raw path=%s round=%d A_ns=%.2f B_ns=%.2f C_ns=%.2f A_bytes=%.2f B_bytes=%.2f C_bytes=%.2f A_cpu=%.2f B_cpu=%.2f C_cpu=%.2f%n",
                            path,
                            round,
                            values[0][0][round],
                            values[1][0][round],
                            values[2][0][round],
                            values[0][1][round],
                            values[1][1][round],
                            values[2][1][round],
                            values[0][2][round],
                            values[1][2][round],
                            values[2][2][round]);
                }
                double[][] medians = new double[3][3];
                for (int variant = 0; variant < 3; variant++) {
                    for (int metric = 0; metric < 3; metric++) {
                        Arrays.sort(values[variant][metric]);
                        medians[variant][metric] =
                                (values[variant][metric][4] + values[variant][metric][5]) / 2;
                    }
                }
                System.out.printf(
                        "median path=%s A_ns=%.2f B_ns=%.2f C_ns=%.2f A_bytes=%.2f B_bytes=%.2f C_bytes=%.2f A_cpu=%.2f B_cpu=%.2f C_cpu=%.2f%n",
                        path,
                        medians[0][0],
                        medians[1][0],
                        medians[2][0],
                        medians[0][1],
                        medians[1][1],
                        medians[2][1],
                        medians[0][2],
                        medians[1][2],
                        medians[2][2]);
            }
        }
    }

    /** Shares the binding/native library while selecting the original or current Framework. */
    private static final class FrameworkLoader extends URLClassLoader {
        FrameworkLoader(URL[] urls) {
            super(urls, ZLinkServiceSendHeaderAllocationTest.class.getClassLoader());
        }

        @Override
        protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            synchronized (getClassLoadingLock(name)) {
                if (!name.startsWith("systems.zlink.framework."))
                    return super.loadClass(name, resolve);
                Class<?> loaded = findLoadedClass(name);
                if (loaded == null) loaded = findClass(name);
                if (resolve) resolveClass(loaded);
                return loaded;
            }
        }
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
