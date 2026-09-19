import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Transparent ZMP proxy that blocks command 44 for the ZoneWorld B8 lane.
 *
 * <p>Single-file source ({@code java SessionRouteBlockProxy.java <args>}) so the sample needs no
 * interpreter beyond the JDK the rest of the sample already requires. Ported 1:1 from the Python
 * original this file replaces; keep the two in step if either changes.
 */
public final class SessionRouteBlockProxy {
    private static final int ZMP_HEADER_SIZE = 8;
    private static final int ZMP_REQUEST_SEQUENCE_SIZE = 8;
    private static final int ZMP_FLAG_MORE = 0x01;
    private static final Set<Integer> ZMP_REQUEST_REPLY_KINDS = Set.of(0x01, 0x02, 0x03);
    private static final byte[] WIRE_MAGIC = {(byte) 90, (byte) 77};
    private static final int SESSION_RELOCATION_ROUTE = 44;

    public static void main(String[] args) throws IOException {
        Map<String, String> options = parseArgs(args);
        String listenHost = require(options, "listen-host");
        int listenPort = Integer.parseInt(require(options, "listen-port"));
        String targetHost = require(options, "target-host");
        int targetPort = Integer.parseInt(require(options, "target-port"));
        Path armFile = Path.of(require(options, "arm-file"));

        try (ServerSocket server = new ServerSocket()) {
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(listenHost, listenPort));
            System.out.println(
                "proxy-ready listen=" + listenHost + ":" + listenPort
                    + " target=" + targetHost + ":" + targetPort);
            System.out.flush();
            while (true) {
                Socket accepted = server.accept();
                Thread.ofVirtual().start(() -> handleConnection(accepted, targetHost, targetPort, armFile));
            }
        }
    }

    private static void handleConnection(
            Socket client, String targetHost, int targetPort, Path armFile) {
        try (client) {
            try (Socket upstream = new Socket(targetHost, targetPort)) {
                System.out.println(
                    "proxy-connection listen=" + client.getLocalSocketAddress()
                        + " target=" + targetHost + ":" + targetPort);
                System.out.flush();
                Thread downstream = Thread.ofVirtual().start(
                    () -> pump(upstream, client, "gateway-to-peer", armFile));
                pump(client, upstream, "peer-to-gateway", armFile);
                downstream.join(Duration.ofSeconds(5));
            }
        } catch (IOException | InterruptedException error) {
            // The accepted connection failed to establish or drain; nothing more to do for it.
        }
    }

    private static void pump(Socket source, Socket sink, String direction, Path armFile) {
        FrameParser parser = new FrameParser();
        ByteArrayOutputStream message = new ByteArrayOutputStream();
        boolean blocked = false;
        try {
            InputStream in = source.getInputStream();
            OutputStream out = sink.getOutputStream();
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) != -1) {
                for (Frame frame : parser.feed(buffer, read)) {
                    message.write(frame.raw());
                    if (!blocked && isCommand44(frame.body()) && Files.exists(armFile)) {
                        blocked = true;
                    }
                    if ((frame.flags() & ZMP_FLAG_MORE) != 0) {
                        continue;
                    }
                    if (blocked) {
                        System.out.println("blocked-command-44 direction=" + direction);
                        System.out.flush();
                    } else {
                        out.write(message.toByteArray());
                        out.flush();
                    }
                    message.reset();
                    blocked = false;
                }
            }
        } catch (IOException error) {
            System.out.println("proxy-pump-ended direction=" + direction + " error=" + error);
            System.out.flush();
        } finally {
            try {
                sink.shutdownOutput();
            } catch (IOException ignored) {
                // Sink already closed by its own reader; nothing to clean up.
            }
        }
    }

    private static boolean isCommand44(byte[] body) {
        return body.length >= 5
            && body[0] == WIRE_MAGIC[0]
            && body[1] == WIRE_MAGIC[1]
            && body[3] == SESSION_RELOCATION_ROUTE;
    }

    private record Frame(byte[] raw, int flags, byte[] body) {
    }

    /** Splits a byte stream into ZMP frames, holding a partial trailing frame across calls. */
    private static final class FrameParser {
        private byte[] buffer = new byte[0];

        List<Frame> feed(byte[] data, int length) {
            byte[] combined = new byte[buffer.length + length];
            System.arraycopy(buffer, 0, combined, 0, buffer.length);
            System.arraycopy(data, 0, combined, buffer.length, length);
            buffer = combined;

            List<Frame> frames = new ArrayList<>();
            int offset = 0;
            while (buffer.length - offset >= ZMP_HEADER_SIZE) {
                if ((buffer[offset] & 0xFF) != 0x5A || (buffer[offset + 1] & 0xFF) != 0x01) {
                    throw new IllegalStateException("unexpected ZMP frame header");
                }
                int flags = buffer[offset + 2] & 0xFF;
                int kind = buffer[offset + 3] & 0xFF;
                long size = ((long) (buffer[offset + 4] & 0xFF) << 24)
                    | ((buffer[offset + 5] & 0xFF) << 16)
                    | ((buffer[offset + 6] & 0xFF) << 8)
                    | (buffer[offset + 7] & 0xFF);
                int headerSize = ZMP_HEADER_SIZE
                    + (ZMP_REQUEST_REPLY_KINDS.contains(kind) ? ZMP_REQUEST_SEQUENCE_SIZE : 0);
                long total = headerSize + size;
                if (buffer.length - offset < total) {
                    break;
                }
                byte[] raw = Arrays.copyOfRange(buffer, offset, offset + (int) total);
                byte[] body = Arrays.copyOfRange(raw, headerSize, raw.length);
                frames.add(new Frame(raw, flags, body));
                offset += (int) total;
            }
            buffer = Arrays.copyOfRange(buffer, offset, buffer.length);
            return frames;
        }
    }

    private static String require(Map<String, String> options, String key) {
        String value = options.get(key);
        if (value == null) {
            throw new IllegalArgumentException("missing --" + key);
        }
        return value;
    }

    private static Map<String, String> parseArgs(String[] args) {
        Map<String, String> options = new HashMap<>();
        for (int i = 0; i < args.length; i++) {
            String argument = args[i];
            if (argument.startsWith("--") && i + 1 < args.length) {
                options.put(argument.substring(2), args[++i]);
            }
        }
        return options;
    }

    private SessionRouteBlockProxy() {
    }
}
