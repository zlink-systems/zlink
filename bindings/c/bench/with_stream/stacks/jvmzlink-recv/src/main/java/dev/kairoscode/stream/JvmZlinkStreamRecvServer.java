package dev.kairoscode.stream;

import dev.kairoscode.zlink.Context;
import dev.kairoscode.zlink.Message;
import dev.kairoscode.zlink.RecvException;
import dev.kairoscode.zlink.RecvResult;
import dev.kairoscode.zlink.Received;
import dev.kairoscode.zlink.RoutingId;
import dev.kairoscode.zlink.SendFlags;
import dev.kairoscode.zlink.StreamSocket;
import java.time.Duration;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class JvmZlinkStreamRecvServer {
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
    private static final int PREFIX_SIZE = 6;
    private static final byte STREAM_EVENT_CONNECT = 0x01;
    private static final byte STREAM_EVENT_DISCONNECT = 0x00;
    private static final byte[] MSG_NAME =
        "stream.echo".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

    private JvmZlinkStreamRecvServer() {
    }

    private static final class ServerOptions {
        String host = "0.0.0.0";
        int port = 38011;
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
                if (!key.startsWith("--")) {
                    continue;
                }
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
            return Math.max(parsed, min);
        }
    }

    private static final class Metrics {
        final AtomicLong recvMsgs = new AtomicLong();
        final AtomicLong parseError = new AtomicLong();
        final AtomicLong protocolError = new AtomicLong();
        final AtomicLong sendError = new AtomicLong();
        final AtomicLong recvPath = new AtomicLong();
    }

    private static final class FrameBuffer {
        byte[] bytes = new byte[0];
        int consumed;

        boolean empty() {
            return bytes.length == 0 && consumed == 0;
        }

        void append(Message chunk) {
            int chunkSize = chunk.size();
            if (chunkSize <= 0) {
                return;
            }
            if (consumed == bytes.length) {
                bytes = new byte[chunkSize];
                chunk.copyTo(bytes, 0, 0, chunkSize);
                consumed = 0;
                return;
            }
            if (consumed > 0) {
                compact();
            }
            int oldLen = bytes.length;
            bytes = Arrays.copyOf(bytes, oldLen + chunkSize);
            chunk.copyTo(bytes, 0, oldLen, chunkSize);
        }

        void compact() {
            if (consumed == 0) {
                return;
            }
            if (consumed >= bytes.length) {
                reset();
                return;
            }
            bytes = Arrays.copyOfRange(bytes, consumed, bytes.length);
            consumed = 0;
        }

        void consume(int length) {
            consumed += length;
            if (consumed >= bytes.length) {
                reset();
            }
        }

        void reset() {
            bytes = new byte[0];
            consumed = 0;
        }

        int available() {
            return bytes.length - consumed;
        }
    }

    private static final class FrameView {
        final int offset;
        final int size;

        FrameView(int offset, int size) {
            this.offset = offset;
            this.size = size;
        }
    }

    private static String endpoint(String host, int port) {
        return "tcp://" + host + ":" + port;
    }

    private static int u16be(byte[] data, int offset) {
        return ((data[offset] & 0xff) << 8) | (data[offset + 1] & 0xff);
    }

    private static int u32be(byte[] data, int offset) {
        return ((data[offset] & 0xff) << 24)
            | ((data[offset + 1] & 0xff) << 16)
            | ((data[offset + 2] & 0xff) << 8)
            | (data[offset + 3] & 0xff);
    }

    private static boolean validFrameSizes(int headerSize, int bodySize) {
        return headerSize == MSG_NAME.length
            && bodySize >= MIN_PAYLOAD_SIZE
            && bodySize <= MAX_PAYLOAD_SIZE;
    }

    private static boolean isMsgName(byte[] data, int offset, int headerSize) {
        if (headerSize != MSG_NAME.length) {
            return false;
        }
        for (int i = 0; i < MSG_NAME.length; i++) {
            if (data[offset + i] != MSG_NAME[i]) {
                return false;
            }
        }
        return true;
    }

    private static boolean isMsgName(Message data, int offset, int headerSize) {
        if (headerSize != MSG_NAME.length) {
            return false;
        }
        for (int i = 0; i < MSG_NAME.length; i++) {
            if (data.readByte(offset + i) != MSG_NAME[i]) {
                return false;
            }
        }
        return true;
    }

    private static boolean isCompleteStreamFrame(Message payload) {
        int payloadSize = payload.size();
        if (payloadSize < PREFIX_SIZE) {
            return false;
        }
        int headerSize = Short.toUnsignedInt(payload.readShortBe(0));
        int bodySize = payload.readIntBe(2);
        if (!validFrameSizes(headerSize, bodySize)) {
            return false;
        }
        int frameSize = PREFIX_SIZE + headerSize + bodySize;
        return payloadSize == frameSize
            && isMsgName(payload, PREFIX_SIZE, headerSize);
    }

    private static FrameView tryPeekFrame(FrameBuffer buffer) {
        if (buffer.available() < PREFIX_SIZE) {
            return null;
        }
        int base = buffer.consumed;
        byte[] data = buffer.bytes;
        int headerSize = u16be(data, base);
        int bodySize = u32be(data, base + 2);
        if (!validFrameSizes(headerSize, bodySize)) {
            return new FrameView(base, -1);
        }
        int frameSize = PREFIX_SIZE + headerSize + bodySize;
        if (buffer.available() < frameSize) {
            return null;
        }
        if (!isMsgName(data, base + PREFIX_SIZE, headerSize)) {
            return new FrameView(base, -1);
        }
        return new FrameView(base, frameSize);
    }

    private static int runServer(ServerOptions opt, Metrics metrics) {
        AtomicBoolean stop = new AtomicBoolean(false);
        CountDownLatch stopped = new CountDownLatch(1);
        Thread shutdownHook = new Thread(() -> {
            stop.set(true);
            stopped.countDown();
        });
        Map<RoutingId, FrameBuffer> buffers = new HashMap<>();

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

            Runtime.getRuntime().addShutdownHook(shutdownHook);
            while (!stop.get()) {
                try (Received received = server.recv()) {
                    handleReceived(server, received, buffers, metrics);
                } catch (RecvException ex) {
                    if (ex.getResult() == RecvResult.NO_DATA
                        || ex.getResult() == RecvResult.BUSY) {
                        continue;
                    }
                    throw ex;
                }
            }
            return 0;
        } catch (Throwable t) {
            System.err.printf("jvmzlink-recv stream: %s%n", t.getMessage());
            return 2;
        } finally {
            try {
                Runtime.getRuntime().removeShutdownHook(shutdownHook);
            } catch (IllegalStateException ignored) {
            }
        }
    }

    private static void handleReceived(StreamSocket server, Received received,
                                       Map<RoutingId, FrameBuffer> buffers,
                                       Metrics metrics) {
        RoutingId routingId = received.routingIdOrNull();
        if (routingId == null || !received.isSinglePart()) {
            metrics.parseError.incrementAndGet();
            metrics.protocolError.incrementAndGet();
            return;
        }

        Message part = received.singlePartOrThrow();
        int payloadSize = part.size();
        if (payloadSize == 0) {
            return;
        }

        if (payloadSize == 1) {
            byte event = part.readByte(0);
            if (event == STREAM_EVENT_DISCONNECT) {
                buffers.remove(routingId);
            }
            if (event == STREAM_EVENT_CONNECT || event == STREAM_EVENT_DISCONNECT) {
                return;
            }
        }

        FrameBuffer buffer = buffers.computeIfAbsent(routingId, ignored -> new FrameBuffer());
        if (buffer.empty() && isCompleteStreamFrame(part)) {
            metrics.recvMsgs.incrementAndGet();
            metrics.recvPath.incrementAndGet();
            try (Message reply = Message.from(part)) {
                server.send(routingId, reply, SendFlags.DONT_WAIT);
            } catch (RuntimeException ex) {
                metrics.sendError.incrementAndGet();
            }
            return;
        }

        buffer.append(part);
        while (true) {
            FrameView frame = tryPeekFrame(buffer);
            if (frame == null) {
                if (buffer.consumed > 0) {
                    buffer.compact();
                }
                return;
            }
            if (frame.size < 0) {
                metrics.parseError.incrementAndGet();
                metrics.protocolError.incrementAndGet();
                buffer.reset();
                return;
            }

            metrics.recvMsgs.incrementAndGet();
            metrics.recvPath.incrementAndGet();
            try (Message reply = Message.from(buffer.bytes, frame.offset, frame.size)) {
                server.send(routingId, reply, SendFlags.DONT_WAIT);
            } catch (RuntimeException ex) {
                metrics.sendError.incrementAndGet();
                buffer.reset();
                return;
            }
            buffer.consume(frame.size);
        }
    }

    public static void main(String[] args) {
        if (args.length == 0) {
            System.out.println("test_scenario_stream_jvmzlink_recv: no args -> skip");
            return;
        }

        ServerOptions opt = ServerOptions.parse(args);
        if (opt.size > MAX_PAYLOAD_SIZE) {
            System.err.printf("jvmzlink-recv stream: size too large %d%n", opt.size);
            System.exit(2);
            return;
        }

        Metrics metrics = new Metrics();
        int rc = runServer(opt, metrics);
        System.out.printf(
            "METRIC stack=%s mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d exact_fast=%d chunk_fast=%d buffered_path=%d connections=%d%n",
            "jvmzlink-recv",
            opt.size,
            metrics.recvMsgs.get(),
            metrics.parseError.get(),
            metrics.protocolError.get(),
            metrics.sendError.get(),
            0,
            0,
            metrics.recvPath.get(),
            0);
        if (rc != 0) {
            System.exit(rc);
        }
    }
}
