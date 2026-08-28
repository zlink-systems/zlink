package systems.zlink.perf.multi;



import org.junit.jupiter.api.Test;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.LockSupport;
import java.util.regex.Pattern;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiDealerDealerRegressionTest {
    private static final Duration PROCESS_TIMEOUT = Duration.ofSeconds(30);
    private static final Pattern THROUGHPUT = Pattern.compile(
        "RESULT,current,DEALER_DEALER,tcp,64,throughput,([0-9.]+)");

    @Test
    void splitProcessServerAndClientCompleteWithoutTimeout() throws Exception {
        runSplitProcessDealerDealer(1);
    }

    @Test
    void splitProcessServerAndClientCompleteWithMultipleClients() throws Exception {
        runSplitProcessDealerDealer(4);
    }

    private static void runSplitProcessDealerDealer(int clients) throws Exception {
        String endpoint = "tcp://127.0.0.1:" + freePort();

        Process server = startPerfProcess(List.of(
            "--multi-server", "MULTI_DEALER_DEALER", "tcp", "64",
            "--endpoint", endpoint,
            "--clients", Integer.toString(clients),
            "--duration", "1"
        ));
        ProcessOutput serverOutput = ProcessOutput.capture(server);
        waitForOutput(server, serverOutput, "READY,", Duration.ofSeconds(10), "server ready");

        Process client = startPerfProcess(List.of(
            "--multi-client", "MULTI_DEALER_DEALER", "tcp", "64",
            "--endpoint", endpoint,
            "--clients", Integer.toString(clients),
            "--duration", "1"
        ));
        ProcessOutput clientOutput = ProcessOutput.capture(client);
        waitForOutput(client, clientOutput, "CLIENT_READY,64", Duration.ofSeconds(10),
            "client ready");
        writeLine(server.getOutputStream(), "START,64");
        writeLine(client.getOutputStream(), "START,64");

        String clientOut = waitAndRead(client, clientOutput, PROCESS_TIMEOUT, "client");
        String serverOut = waitAndRead(server, serverOutput, PROCESS_TIMEOUT, "server");

        assertTrue(serverOut.contains("RESULT,current,DEALER_DEALER,tcp,64,throughput,"),
            "unexpected server output:\n" + serverOut);
        var throughput = THROUGHPUT.matcher(serverOut);
        assertTrue(throughput.find(), "missing throughput:\n" + serverOut);
        assertTrue(Double.parseDouble(throughput.group(1)) > 0.0,
            "async client traffic was not delivered:\n" + serverOut);
        assertTrue(!clientOut.contains("RESULT,current,DEALER_DEALER,tcp,64,throughput,"),
            "unexpected client output:\n" + clientOut);
    }

    private static Process startPerfProcess(List<String> args) throws Exception {
        String javaBin = Path.of(System.getProperty("java.home"), "bin", "java").toString();
        String classPath = System.getProperty("java.class.path");
        ProcessBuilder pb = new ProcessBuilder();
        pb.command().add(javaBin);
        pb.command().add("--enable-native-access=ALL-UNNAMED");
        pb.command().add("-cp");
        pb.command().add(classPath);
        pb.command().add("systems.zlink.perf.multi.PerfMain");
        pb.command().addAll(args);
        pb.redirectErrorStream(true);
        return pb.start();
    }

    private static void waitForOutput(Process process, ProcessOutput output, String token,
                                      Duration timeout,
                                      String label) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (System.nanoTime() < deadline) {
            String current = output.snapshot();
            if (current.contains(token)) {
                return;
            }
            if (!process.isAlive()) {
                throw new AssertionError(label + " process exited early:\n" + current);
            }
            LockSupport.parkNanos(Duration.ofMillis(1).toNanos());
        }
        throw new AssertionError(label + " timed out");
    }

    private static String waitAndRead(Process process, ProcessOutput output, Duration timeout,
                                      String label)
        throws Exception {
        process.getOutputStream().close();
        boolean exited = process.waitFor(timeout.toMillis(), TimeUnit.MILLISECONDS);
        if (!exited) {
            process.destroy();
            process.waitFor(1, TimeUnit.SECONDS);
        }
        output.awaitClose(Duration.ofSeconds(1));
        String text = output.snapshot();
        assertTrue(exited, label + " timed out:\n" + text);
        assertEquals(0, process.exitValue(), label + " failed:\n" + text);
        return text;
    }

    private static void writeLine(OutputStream output, String line) throws Exception {
        output.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        output.flush();
    }

    private static int freePort() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        }
    }

    private static final class ProcessOutput {
        private final SynchronizedBuffer buffer = new SynchronizedBuffer();
        private final Thread reader;

        private ProcessOutput(InputStream input) {
            reader = new Thread(() -> {
                try (input) {
                    input.transferTo(buffer);
                } catch (Exception ignored) {
                }
            }, "perf-output-reader");
            reader.setDaemon(true);
            reader.start();
        }

        static ProcessOutput capture(Process process) {
            return new ProcessOutput(process.getInputStream());
        }

        String snapshot() {
            return buffer.snapshot();
        }

        void awaitClose(Duration timeout) throws Exception {
            reader.join(timeout.toMillis());
        }
    }

    private static final class SynchronizedBuffer extends ByteArrayOutputStream {
        @Override
        public synchronized void write(byte[] data, int offset, int length) {
            super.write(data, offset, length);
        }

        synchronized String snapshot() {
            return toString(StandardCharsets.UTF_8);
        }
    }
}
