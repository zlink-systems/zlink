package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.sun.management.ThreadMXBean;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.management.ManagementFactory;
import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Measures allocations of the real multipart writer, including its binding calls. */
public final class ZLinkServiceMultipartWriteBudgetTest {
    private static final int ROUNDTRIPS = 1000;
    private static final int WARMUP_ROUNDTRIPS = 20000;
    // JDK 25, -Xbatch -XX:-DoEscapeAnalysis, fresh native parts per frame:
    // 1000 request/reply pairs allocate 2,384,000 B with copyTo versus 2,896,000 B
    // with source views. 2,500,000 B allows 116,000 B of headroom but rejects the
    // 512,000 B regression. Payload comparison allocations are outside this budget.
    private static final long MAX_ALLOCATED_BYTES = 2500000;

    @Test
    void serialRequestAndReplyWritePartsWithoutSourceBufferViews() throws Exception {
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
        // Escape analysis can eliminate the old writer's source views after inlining.
        // Use the same JDK in a separate VM so the budget observes actual view allocations,
        // independently of background compilation and the rest of the suite's JVM options.
        Process process =
                new ProcessBuilder(
                                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                "-Xbatch",
                                "-XX:-DoEscapeAnalysis",
                                "-cp",
                                String.join(java.io.File.pathSeparator, classpath),
                                getClass().getName())
                        .redirectErrorStream(true)
                        .start();
        try {
            assertTrue(process.waitFor(60, TimeUnit.SECONDS), "allocation JVM did not finish");
            String output =
                    new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            System.out.print(output);
            assertEquals(0, process.exitValue(), output);
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }

    public static void main(String[] args) {
        ThreadMXBean threads = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        assertTrue(threads.isThreadAllocatedMemorySupported(), "allocation accounting is required");
        threads.setThreadAllocatedMemoryEnabled(true);
        byte[] header = new byte[] {1, 2, 3};
        byte[] body = new byte[1024];
        for (int i = 0; i < body.length; i++) body[i] = (byte) i;
        byte[] expected = expectedFrame(header, body);
        writeRoundtrips(header, body, WARMUP_ROUNDTRIPS);
        long before = threads.getCurrentThreadAllocatedBytes();
        int frames = writeRoundtrips(header, body, ROUNDTRIPS);
        long allocated = threads.getCurrentThreadAllocatedBytes() - before;
        assertEquals(2 * ROUNDTRIPS, frames, "both request and reply were written");
        for (int roundtrip = 0; roundtrip < ROUNDTRIPS; roundtrip++) {
            for (int direction = 0; direction < 2; direction++) {
                try (Message first = Message.from(header);
                        Message second = Message.from(body);
                        Message frame =
                                ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(
                                        List.of(first, second))) {
                    assertArrayEquals(expected, frame.toByteArray());
                    assertArrayEquals(header, first.toByteArray());
                    assertArrayEquals(body, second.toByteArray());
                }
            }
        }
        System.out.println("multipart allocated bytes per 1000 roundtrips: " + allocated);
        assertTrue(
                allocated <= MAX_ALLOCATED_BYTES,
                () ->
                        "multipart allocated bytes per 1000 roundtrips: "
                                + allocated
                                + " exceeds "
                                + MAX_ALLOCATED_BYTES);
    }

    private static int writeRoundtrips(byte[] header, byte[] body, int roundtrips) {
        // Reusing parts would measure the binding's cached source views instead of
        // the single-use request/reply parts that this regression concerns.
        int frames = 0;
        for (int roundtrip = 0; roundtrip < roundtrips; roundtrip++) {
            for (int direction = 0; direction < 2; direction++) {
                try (Message first = Message.from(header);
                        Message second = Message.from(body);
                        Message frame =
                                ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(
                                        List.of(first, second))) {
                    frames++;
                }
            }
        }
        return frames;
    }

    private static byte[] expectedFrame(byte[] header, byte[] body) {
        // Build multipart bytes independently of the part writer.
        // The existing golden test pins the complete application frame.
        return new ZLinkServiceM6AWireCodec()
                .encodeApplicationPayload(
                        new ZLinkServiceM6AWireCodec.ApplicationPayload(
                                ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
                                ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE,
                                ByteBuffer.allocate(3 * Integer.BYTES + header.length + body.length)
                                        .putInt(2)
                                        .putInt(header.length)
                                        .put(header)
                                        .putInt(body.length)
                                        .put(body)
                                        .array()));
    }
}
