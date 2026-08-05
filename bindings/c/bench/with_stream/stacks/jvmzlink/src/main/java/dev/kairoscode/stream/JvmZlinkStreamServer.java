package dev.kairoscode.stream;

import dev.kairoscode.zlink.Context;
import dev.kairoscode.zlink.Message;
import dev.kairoscode.zlink.SendFlags;
import dev.kairoscode.zlink.StreamSocket;
import dev.kairoscode.zlink.StreamUInt32FramedPacketHandler;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class JvmZlinkStreamServer {
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
    private static final int PREFIX_SIZE = 6;
    private static final byte[] MSG_NAME =
        "stream.echo".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] FRAME_PREFIX_AND_HEADER =
        buildPrefixAndHeader();
    private JvmZlinkStreamServer() {
    }

    private static final class ServerOptions {
        String host = "0.0.0.0";
        int port = 38008;
        int size = 1024;
        int sndbuf = 1024 * 1024;
        int rcvbuf = 1024 * 1024;
        int backlog = 32768;
        int tcpNoDelay = 1;
        int ioThreads = 4;

        static ServerOptions parse(String[] args) {
            ServerOptions opt = new ServerOptions();
            for (int i = 0; i + 1 < args.length; i++) {
                String key = args[i];
                if (!key.startsWith("--"))
                    continue;
                String value = args[++i];
                switch (key) {
                    case "--host":
                        opt.host = value;
                        break;
                    case "--port":
                        opt.port = parseInt(value, opt.port, 1);
                        break;
                    case "--size":
                        opt.size = parseInt(value, opt.size, MIN_PAYLOAD_SIZE);
                        break;
                    case "--sndbuf":
                        opt.sndbuf = parseInt(value, opt.sndbuf, 1);
                        break;
                    case "--rcvbuf":
                        opt.rcvbuf = parseInt(value, opt.rcvbuf, 1);
                        break;
                    case "--backlog":
                        opt.backlog = parseInt(value, opt.backlog, 1);
                        break;
                    case "--tcp-nodelay":
                        opt.tcpNoDelay = parseInt(value, opt.tcpNoDelay, 0);
                        break;
                    case "--io-threads":
                        opt.ioThreads = parseInt(value, opt.ioThreads, 1);
                        break;
                    default:
                        break;
                }
            }
            return opt;
        }

        private static int parseInt(String text, int fallback, int min) {
            int parsed;
            try {
                parsed = Integer.parseInt(text);
            } catch (Exception ignore) {
                return fallback;
            }
            if (parsed < min)
                return min;
            return parsed;
        }
    }

    private static final class Metrics {
        final AtomicLong recvMsgs = new AtomicLong();
        final AtomicLong parseError = new AtomicLong();
        final AtomicLong protocolError = new AtomicLong();
        final AtomicLong sendError = new AtomicLong();
        final AtomicLong framedPath = new AtomicLong();
    }

    private static String endpoint(String host, int port) {
        return "tcp://" + host + ":" + port;
    }

    private static byte[] buildPrefixAndHeader() {
        byte[] frame = new byte[PREFIX_SIZE + MSG_NAME.length];
        int headerSize = MSG_NAME.length;
        frame[0] = (byte) (headerSize >> 8);
        frame[1] = (byte) headerSize;
        System.arraycopy(MSG_NAME, 0, frame, PREFIX_SIZE, headerSize);
        return frame;
    }

    private static int runServer(ServerOptions opt, Metrics metrics) {
        final AtomicBoolean stop = new AtomicBoolean(false);
        final CountDownLatch stopped = new CountDownLatch(1);
        final Thread shutdownHook = new Thread(() -> {
            stop.set(true);
            stopped.countDown();
        });
        try (Context ctx = new Context(); StreamSocket server = new StreamSocket(ctx)) {
            ctx.options().ioThreads(Math.max(1, opt.ioThreads));
            server.options().sendBuffer(opt.sndbuf);
            server.options().recvBuffer(opt.rcvbuf);
            server.options().backlog(opt.backlog);
            server.options().sendHwm(100);
            server.options().recvHwm(100);
            server.options().recvTimeout(Duration.ofMillis(200));
            server.options().tcpNoDelay(opt.tcpNoDelay != 0);
            server.bind(endpoint(opt.host, opt.port));
            server.onFramedPacket(
                (StreamUInt32FramedPacketHandler) (routingId, header, body) ->
                    handleFramedPacket(server, routingId, header, body, metrics));

            Runtime.getRuntime().addShutdownHook(shutdownHook);
            while (!stop.get()) {
                stopped.await();
            }
            return 0;
        } catch (Throwable t) {
            System.err.printf("jvmzlink stream: %s%n", t.getMessage());
            return 2;
        } finally {
            try {
                Runtime.getRuntime().removeShutdownHook(shutdownHook);
            } catch (IllegalStateException ignored) {
            }
        }
    }

    public static void main(String[] args) {
        if (args.length == 0) {
            System.out.println("test_scenario_stream_jvmzlink: no args -> skip");
            return;
        }

        ServerOptions opt = ServerOptions.parse(args);
        if (opt.size > MAX_PAYLOAD_SIZE) {
            System.err.printf("jvmzlink stream: size too large %d%n", opt.size);
            System.exit(2);
            return;
        }

        Metrics metrics = new Metrics();
        int rc = runServer(opt, metrics);
        System.out.printf(
          "METRIC stack=%s mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d exact_fast=%d chunk_fast=%d buffered_path=%d connections=%d%n",
          "jvmzlink",
          opt.size,
          metrics.recvMsgs.get(),
          metrics.parseError.get(),
          metrics.protocolError.get(),
          metrics.sendError.get(),
          metrics.framedPath.get(),
          0,
          0,
          0);
        if (rc != 0)
            System.exit(rc);
    }

    private static void handleFramedPacket(StreamSocket server, int routingId,
                                           Message header,
                                           Message body,
                                           Metrics metrics) {
        int headerSize = header.size();
        if (headerSize != MSG_NAME.length) {
            metrics.parseError.incrementAndGet();
            metrics.protocolError.incrementAndGet();
            return;
        }

        if (!header.contentEquals(MSG_NAME)) {
            metrics.parseError.incrementAndGet();
            metrics.protocolError.incrementAndGet();
            return;
        }

        int bodySize = body.size();
        if (bodySize < MIN_PAYLOAD_SIZE || bodySize > MAX_PAYLOAD_SIZE) {
            metrics.parseError.incrementAndGet();
            metrics.protocolError.incrementAndGet();
            return;
        }

        metrics.recvMsgs.incrementAndGet();
        metrics.framedPath.incrementAndGet();
        int totalSize = PREFIX_SIZE + headerSize + bodySize;
        try {
            try (Message response = new Message(totalSize)) {
                response.copyFrom(FRAME_PREFIX_AND_HEADER, 0, 0,
                    FRAME_PREFIX_AND_HEADER.length);
                response.writeIntBe(2, bodySize);
                response.copyFrom(body, 0, PREFIX_SIZE + headerSize, bodySize);
                server.send(routingId, response, SendFlags.DONT_WAIT);
            }
        } catch (RuntimeException ex) {
            metrics.sendError.incrementAndGet();
        }
    }
}
