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
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** Echoes raw len32be STREAM chunks through the binding's pull receive API. */
public final class JvmZlinkStreamLen32BeServer {
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;

    private JvmZlinkStreamLen32BeServer() {
    }

    private static final class ServerOptions {
        String host = "0.0.0.0";
        int port = 38010;
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
                    case "--host": opt.host = value; break;
                    case "--port": opt.port = parseInt(value, opt.port, 1); break;
                    case "--size": opt.size = parseInt(value, opt.size, MIN_PAYLOAD_SIZE); break;
                    case "--sndbuf": opt.sndbuf = parseInt(value, opt.sndbuf, 1); break;
                    case "--rcvbuf": opt.rcvbuf = parseInt(value, opt.rcvbuf, 1); break;
                    case "--backlog": opt.backlog = parseInt(value, opt.backlog, 1); break;
                    case "--tcp-nodelay": opt.tcpNoDelay = parseInt(value, opt.tcpNoDelay, 0); break;
                    case "--io-threads": opt.ioThreads = parseInt(value, opt.ioThreads, 1); break;
                    default: break;
                }
            }
            return opt;
        }

        private static int parseInt(String text, int fallback, int minimum) {
            try {
                return Math.max(Integer.parseInt(text), minimum);
            } catch (RuntimeException ignored) {
                return fallback;
            }
        }
    }

    private static final class Metrics {
        final AtomicLong recvMsgs = new AtomicLong();
        final AtomicLong parseError = new AtomicLong();
        final AtomicLong protocolError = new AtomicLong();
        final AtomicLong sendError = new AtomicLong();
    }

    private static int runServer(ServerOptions opt, Metrics metrics) {
        AtomicBoolean stop = new AtomicBoolean(false);
        Thread shutdownHook = new Thread(() -> stop.set(true));
        try (Context ctx = new Context(); StreamSocket server = new StreamSocket(ctx)) {
            ctx.options().ioThreads(Math.max(1, opt.ioThreads));
            server.options().sendBuffer(opt.sndbuf);
            server.options().recvBuffer(opt.rcvbuf);
            server.options().backlog(opt.backlog);
            server.options().sendHwm(100);
            server.options().recvHwm(100);
            server.options().recvTimeout(Duration.ofMillis(200));
            server.options().tcpNoDelay(opt.tcpNoDelay != 0);
            server.bind("tcp://" + opt.host + ":" + opt.port);

            Runtime.getRuntime().addShutdownHook(shutdownHook);
            while (!stop.get()) {
                try (Received received = server.recv()) {
                    RoutingId routingId = received.routingIdOrNull();
                    if (routingId == null || !received.isSinglePart()) {
                        metrics.parseError.incrementAndGet();
                        metrics.protocolError.incrementAndGet();
                        continue;
                    }
                    Message chunk = received.singlePartOrThrow();
                    if (chunk.size() == 0) {
                        continue;
                    }
                    if (chunk.size() == 1) {
                        byte event = chunk.readByte(0);
                        if (event == 0x00 || event == 0x01) {
                            continue;
                        }
                    }
                    if (chunk.size() > MAX_PAYLOAD_SIZE) {
                        metrics.parseError.incrementAndGet();
                        metrics.protocolError.incrementAndGet();
                        continue;
                    }
                    try (Message reply = Message.from(chunk)) {
                        server.send(routingId, reply, SendFlags.DONT_WAIT);
                        metrics.recvMsgs.incrementAndGet();
                    } catch (RuntimeException sendFailure) {
                        metrics.sendError.incrementAndGet();
                    }
                } catch (RecvException receiveFailure) {
                    if (receiveFailure.getResult() != RecvResult.NO_DATA
                        && receiveFailure.getResult() != RecvResult.BUSY) {
                        throw receiveFailure;
                    }
                }
            }
            return 0;
        } catch (Throwable failure) {
            System.err.printf("jvmzlink-len32be stream: %s%n", failure.getMessage());
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
            System.out.println("test_scenario_stream_jvmzlink_len32be: no args -> skip");
            return;
        }
        ServerOptions opt = ServerOptions.parse(args);
        if (opt.size > MAX_PAYLOAD_SIZE) {
            System.err.printf("jvmzlink-len32be stream: size too large %d%n", opt.size);
            System.exit(2);
            return;
        }
        Metrics metrics = new Metrics();
        int rc = runServer(opt, metrics);
        System.out.printf(
          "METRIC stack=jvmzlink-len32be mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d connections=%d%n",
          opt.size, metrics.recvMsgs.get(), metrics.parseError.get(), metrics.protocolError.get(),
          metrics.sendError.get(), 0);
        if (rc != 0) {
            System.exit(rc);
        }
    }
}
