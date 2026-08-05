/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;



import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class PerfControl {
    private static final Object STDIN_LOCK = new Object();
    private static final BufferedReader STDIN_READER = new BufferedReader(
        new InputStreamReader(System.in, StandardCharsets.UTF_8));

    private PerfControl() {
    }

    public static void emitReady(String endpoint) {
        emitLine("READY," + endpoint);
    }

    public static void emitClientReady(int size) {
        emitLine("CLIENT_READY," + size);
    }

    public static void emitClientDone(int size) {
        emitLine("CLIENT_DONE," + size);
    }

    public static void emitControlReady(String endpoint) {
        emitLine("CONTROL_READY," + endpoint);
    }

    public static void emitClientControlEndpoint(String endpoint) {
        emitLine("CLIENT_CONTROL_ENDPOINT," + endpoint);
    }

    public static void emitControlConnected(String endpoint) {
        emitLine("CONTROL_CONNECTED," + endpoint);
    }

    public static void awaitStart(int size, String label) {
        String expected = "START," + size;
        try {
            synchronized (STDIN_LOCK) {
                String line;
                while ((line = STDIN_READER.readLine()) != null) {
                    if (expected.equals(line)) {
                        return;
                    }
                }
            }
        } catch (java.io.IOException ex) {
            throw new IllegalStateException(label + " control read failed", ex);
        }
        throw new IllegalStateException(label + " missing " + expected);
    }

    public static void awaitControlConnected(String endpoint, String label) {
        String expected = "CONTROL_CONNECTED," + endpoint;
        try {
            synchronized (STDIN_LOCK) {
                String line;
                while ((line = STDIN_READER.readLine()) != null) {
                    if (expected.equals(line)) {
                        return;
                    }
                }
            }
        } catch (java.io.IOException ex) {
            throw new IllegalStateException(label + " control read failed", ex);
        }
        throw new IllegalStateException(label + " missing " + expected);
    }

    public static void awaitStartAndAckControl(int size, String label) {
        String expectedStart = "START," + size;
        try {
            synchronized (STDIN_LOCK) {
                String line;
                while ((line = STDIN_READER.readLine()) != null) {
                    if (line.startsWith("CONNECT_CONTROL,")) {
                        emitControlConnected(line.substring("CONNECT_CONTROL,".length()));
                        continue;
                    }
                    if (expectedStart.equals(line)) {
                        return;
                    }
                }
            }
        } catch (java.io.IOException ex) {
            throw new IllegalStateException(label + " control read failed", ex);
        }
        throw new IllegalStateException(label + " missing " + expectedStart);
    }

    public static AtomicBoolean watchStopSignal(String label) {
        AtomicBoolean stopRequested = new AtomicBoolean(false);
        Thread watcher = new Thread(() -> {
            try {
                BufferedReader reader = new BufferedReader(
                    new InputStreamReader(System.in, StandardCharsets.UTF_8));
                String line;
                while ((line = reader.readLine()) != null) {
                    if ("STOP".equals(line) || "QUIT".equals(line)) {
                        stopRequested.set(true);
                        return;
                    }
                }
            } catch (java.io.IOException ex) {
                throw new IllegalStateException(label + " control read failed", ex);
            }
        }, label + "-control");
        watcher.setDaemon(true);
        watcher.start();
        return stopRequested;
    }

    public static void sendControlLine(BufferedWriter writer, String line) {
        try {
            writer.write(line);
            writer.newLine();
            writer.flush();
        } catch (IOException ex) {
            throw new IllegalStateException("control write failed: "
                + line.toLowerCase(Locale.ROOT), ex);
        }
    }

    public static BufferedReader utf8Reader(java.io.InputStream input) {
        return new BufferedReader(new InputStreamReader(input, StandardCharsets.UTF_8));
    }

    public static BufferedWriter utf8Writer(java.io.OutputStream output) {
        return new BufferedWriter(new OutputStreamWriter(output, StandardCharsets.UTF_8));
    }

    private static void emitLine(String line) {
        System.out.println(line);
        System.out.flush();
    }
}
