package dev.kairoscode.stream;

import io.github.ulalax.zmq.Context;
import io.github.ulalax.zmq.ContextOption;
import io.github.ulalax.zmq.SendFlags;
import io.github.ulalax.zmq.Socket;
import io.github.ulalax.zmq.SocketOption;
import io.github.ulalax.zmq.SocketType;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class JvmZmqStreamServer {
    private static final byte STREAM_EVENT_CONNECT = 0x01;
    private static final byte STREAM_EVENT_DISCONNECT = 0x00;
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
    private static final byte[] MSG_NAME =
        "stream.echo".getBytes(StandardCharsets.US_ASCII);
    private static final int ROUTING_ID_CAPACITY = 256;
    private static final int FRAME_CAPACITY =
        6 + MSG_NAME.length + MAX_PAYLOAD_SIZE;

    private JvmZmqStreamServer() {
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
    }

    private static final class FrameBuffer {
        byte[] data = new byte[0];
        int size = 0;

        void append(byte[] chunk, int length) {
            if (length <= 0)
                return;
            int needed = size + length;
            if (data.length < needed) {
                int grow = Math.max(needed, Math.max(64, data.length * 2));
                data = Arrays.copyOf(data, grow);
            }
            System.arraycopy(chunk, 0, data, size, length);
            size = needed;
        }

        void clear() {
            size = 0;
        }

        void consume(int count) {
            if (count <= 0)
                return;
            if (count >= size) {
                size = 0;
                return;
            }
            System.arraycopy(data, count, data, 0, size - count);
            size -= count;
        }
    }

    private static String endpoint(String host, int port) {
        return "tcp://" + host + ":" + port;
    }

    public static void main(String[] args) {
        if (args.length == 0) {
            System.out.println("test_scenario_stream_jvmzmq: no args -> skip");
            return;
        }

        ServerOptions opt = ServerOptions.parse(args);
        if (opt.size > MAX_PAYLOAD_SIZE) {
            System.err.printf("jvmzmq stream: size too large %d%n", opt.size);
            System.exit(2);
            return;
        }

        Metrics metrics = new Metrics();
        int rc = runServer(opt, metrics);
        System.out.printf(
            "METRIC stack=%s mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d connections=%d%n",
            "jvmzmq",
            opt.size,
            metrics.recvMsgs.get(),
            metrics.parseError.get(),
            metrics.protocolError.get(),
            metrics.sendError.get(),
            0);
        if (rc != 0)
            System.exit(rc);
    }

    private static int runServer(ServerOptions opt, Metrics metrics) {
        AtomicBoolean stop = new AtomicBoolean(false);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> stop.set(true)));

        try (Context ctx = new Context();
             Socket server = new Socket(ctx, SocketType.STREAM)) {
            ctx.setOption(ContextOption.IO_THREADS, Math.max(1, opt.ioThreads));
            server.setOption(SocketOption.SNDBUF, opt.sndbuf);
            server.setOption(SocketOption.RCVBUF, opt.rcvbuf);
            server.setOption(SocketOption.BACKLOG, opt.backlog);
            server.setOption(SocketOption.SNDHWM, 100);
            server.setOption(SocketOption.RCVHWM, 100);
            server.setOption(SocketOption.STREAM_NOTIFY, 1);
            try {
                server.setOption(SocketOption.TCP_KEEPALIVE, 0);
            } catch (RuntimeException ignored) {
            }
            server.bind(endpoint(opt.host, opt.port));

            Map<String, FrameBuffer> frameBuffers = new HashMap<>();
            byte[] routing = new byte[ROUTING_ID_CAPACITY];
            byte[] payload = new byte[FRAME_CAPACITY];
            byte[] reply = new byte[FRAME_CAPACITY];
            while (!stop.get()) {
                int idRc = server.tryRecv(routing);
                if (idRc == Socket.NO_MESSAGE)
                    continue;
                int payloadRc = server.tryRecv(payload);
                if (payloadRc == Socket.NO_MESSAGE) {
                    metrics.parseError.incrementAndGet();
                    continue;
                }

                if (idRc <= 0) {
                    metrics.parseError.incrementAndGet();
                    continue;
                }

                if (payloadRc <= 0) {
                    frameBuffers.remove(routingKey(routing, idRc));
                    continue;
                }
                if (isStreamEvent(payload, payloadRc)) {
                    frameBuffers.remove(routingKey(routing, idRc));
                    continue;
                }

                String key = routingKey(routing, idRc);
                FrameBuffer buffer = frameBuffers.computeIfAbsent(key,
                    unused -> new FrameBuffer());
                buffer.append(payload, payloadRc);
                processBufferedFrames(server, routing, idRc, buffer, reply,
                    metrics);
                if (buffer.size == 0)
                    frameBuffers.remove(key);
            }
            return 0;
        } catch (Throwable t) {
            System.err.printf("jvmzmq stream: %s%n", t.getMessage());
            t.printStackTrace(System.err);
            return 2;
        }
    }

    private static void processBufferedFrames(Socket server, byte[] rid,
                                             int ridLength,
                                             FrameBuffer buffer,
                                             byte[] reply,
                                             Metrics metrics) {
        while (true) {
            if (buffer.size < 6)
                return;
            int headerSize = ((buffer.data[0] & 0xFF) << 8)
                | (buffer.data[1] & 0xFF);
            int bodySize = ((buffer.data[2] & 0xFF) << 24)
                | ((buffer.data[3] & 0xFF) << 16)
                | ((buffer.data[4] & 0xFF) << 8)
                | (buffer.data[5] & 0xFF);
            if (headerSize != MSG_NAME.length
                || bodySize < MIN_PAYLOAD_SIZE
                || bodySize > MAX_PAYLOAD_SIZE) {
                metrics.parseError.incrementAndGet();
                metrics.protocolError.incrementAndGet();
                buffer.clear();
                return;
            }
            int total = 6 + headerSize + bodySize;
            if (buffer.size < total)
                return;
            for (int i = 0; i < MSG_NAME.length; i++) {
                if (buffer.data[6 + i] != MSG_NAME[i]) {
                    metrics.parseError.incrementAndGet();
                    metrics.protocolError.incrementAndGet();
                    buffer.clear();
                    return;
                }
            }
            metrics.recvMsgs.incrementAndGet();
            System.arraycopy(buffer.data, 0, reply, 0, total);
            if (!sendReply(server, rid, ridLength, reply, total)) {
                metrics.sendError.incrementAndGet();
                return;
            }
            buffer.consume(total);
        }
    }

    private static boolean sendReply(Socket server, byte[] rid, int ridLength,
                                     byte[] payload,
                                     int length) {
        try {
            if (!server.send(rid, ridLength, SendFlags.SEND_MORE))
                return false;
            return server.send(payload, length, SendFlags.NONE);
        } catch (RuntimeException ex) {
            return false;
        }
    }

    private static String routingKey(byte[] rid, int length) {
        return new String(rid, 0, length, StandardCharsets.ISO_8859_1);
    }

    private static boolean isStreamEvent(byte[] payload, int length) {
        return length == 1
            && (payload[0] == STREAM_EVENT_CONNECT
                || payload[0] == STREAM_EVENT_DISCONNECT);
    }
}
