package dev.kairoscode.stream;

import dev.kairoscode.zlink.Context;
import dev.kairoscode.zlink.ContextOption;
import dev.kairoscode.zlink.SendFlag;
import dev.kairoscode.zlink.Socket;
import dev.kairoscode.zlink.SocketOption;
import dev.kairoscode.zlink.SocketType;
import dev.kairoscode.zlink.internal.LibraryLoader;
import dev.kairoscode.zlink.internal.Native;
import dev.kairoscode.zlink.internal.NativeLayouts;
import dev.kairoscode.zlink.internal.NativeMsg;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicLong;

public final class JvmZlinkStreamLen32BeServer {
    private static final int ZLINK_TCP_NODELAY = 118;
    private static final int ROUTING_ID_STRUCT_SIZE = 256;
    private static final int ROUTING_ID_SIZE_OFFSET = 0;
    private static final int ROUTING_ID_DATA_OFFSET = 1;
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
    private static final long MSG_SIZE = NativeLayouts.MSG_LAYOUT.byteSize();

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP = LibraryLoader.lookup();
    private static final FunctionDescriptor FD_SOCKET_MSG_HANDLER =
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);
    private static final MethodHandle MH_SOCKET_ATTACH_HANDLER =
      downcall("zlink_recv_handler",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.ADDRESS, ValueLayout.ADDRESS));
    private static final MethodHandle MH_CTX_SET = downcall("zlink_ctx_set",
      FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.JAVA_INT, ValueLayout.JAVA_INT));

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

        void addRecvMsg() {
            recvMsgs.incrementAndGet();
        }

        void addParseError() {
            parseError.incrementAndGet();
        }

        void addProtocolError() {
            protocolError.incrementAndGet();
        }

        void addSendError() {
            sendError.incrementAndGet();
        }
    }

    private static final class StreamCallbackEcho implements AutoCloseable {
        private final Socket socket;
        private final Metrics metrics;
        private final Arena callbackArena;
        private final MemorySegment callbackStub;
        private boolean attached;

        StreamCallbackEcho(Socket socket, Metrics metrics) {
            this.socket = socket;
            this.metrics = metrics;
            this.callbackArena = Arena.ofShared();
            try {
                MethodHandle cb = MethodHandles.lookup().findVirtual(
                  StreamCallbackEcho.class,
                  "onMessages",
                  MethodType.methodType(void.class, MemorySegment.class,
                    MemorySegment.class, long.class,
                    MemorySegment.class)).bindTo(this);
                this.callbackStub =
                  LINKER.upcallStub(cb, FD_SOCKET_MSG_HANDLER, callbackArena);
            } catch (NoSuchMethodException | IllegalAccessException ex) {
                throw new RuntimeException(ex);
            }
        }

        void attach() {
            int rc = socketAttachHandler(socket.handle(), callbackStub, MemorySegment.NULL);
            if (rc != 0)
                throw new RuntimeException("zlink_recv_handler failed");
            attached = true;
        }

        private void onMessages(MemorySegment rid,
                                MemorySegment msgs,
                                long msgCount,
                                MemorySegment userdata) {
            try {
                if (msgCount <= 0 || msgs == null || msgs.address() == 0
                    || rid == null || rid.address() == 0)
                    return;
                long routingId = routingId(rid);
                if (routingId < 0) {
                    metrics.addParseError();
                    metrics.addProtocolError();
                    return;
                }

                MemorySegment msgArray = msgs.reinterpret(MSG_SIZE * msgCount);
                for (int i = 0; i < msgCount; i++) {
                    MemorySegment msg = msgArray.asSlice((long) i * MSG_SIZE,
                      MSG_SIZE);
                    boolean consumed = false;
                    try {
                        long payloadSize = NativeMsg.msgSize(msg);
                        if (payloadSize <= 0)
                            continue;

                        if (payloadSize > Integer.MAX_VALUE
                            || payloadSize > MAX_PAYLOAD_SIZE) {
                            metrics.addParseError();
                            metrics.addProtocolError();
                            continue;
                        }

                        if (payloadSize < MIN_PAYLOAD_SIZE)
                            continue;

                        int payloadLen = (int) payloadSize;
                        metrics.addRecvMsg();
                        int sent = Native.streamSendMsg(
                          socket.handle(), rid, msg, SendFlag.NONE.getValue());
                        consumed = true;
                        if (sent != payloadLen)
                            metrics.addSendError();
                    } catch (Throwable sendError) {
                        metrics.addSendError();
                    } finally {
                        if (!consumed) {
                            try {
                                NativeMsg.msgClose(msg);
                            } catch (Throwable ignored) {
                                metrics.addSendError();
                            }
                        }
                    }
                }
            } catch (Throwable t) {
                metrics.addSendError();
            }
        }

        @Override
        public void close() {
            try {
                attached = false;
            } finally {
                if (callbackArena.scope().isAlive())
                    callbackArena.close();
            }
        }
    }

    private static MethodHandle downcall(String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(LOOKUP.find(name).orElseThrow(), fd);
    }

    private static int socketAttachHandler(MemorySegment socket,
                                           MemorySegment handler,
                                           MemorySegment userdata) {
        try {
            return (int) MH_SOCKET_ATTACH_HANDLER.invokeExact(socket, handler, userdata);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_recv_handler failed", t);
        }
    }

    private static int ctxSet(MemorySegment ctx, int option, int value) {
        try {
            return (int) MH_CTX_SET.invokeExact(ctx, option, value);
        } catch (Throwable t) {
            throw new RuntimeException("zlink_ctx_set failed", t);
        }
    }

    private static String endpoint(String host, int port) {
        return "tcp://" + host + ":" + port;
    }

    private static long routingId(MemorySegment rid) {
        if (rid == null || rid.address() == 0)
            return -1;
        MemorySegment view = rid.reinterpret(ROUTING_ID_STRUCT_SIZE);
        int size = view.get(ValueLayout.JAVA_BYTE, ROUTING_ID_SIZE_OFFSET) & 0xFF;
        if (size != 4)
            return -1;
        int b0 = view.get(ValueLayout.JAVA_BYTE, ROUTING_ID_DATA_OFFSET) & 0xFF;
        int b1 = view.get(ValueLayout.JAVA_BYTE, ROUTING_ID_DATA_OFFSET + 1) & 0xFF;
        int b2 = view.get(ValueLayout.JAVA_BYTE, ROUTING_ID_DATA_OFFSET + 2) & 0xFF;
        int b3 = view.get(ValueLayout.JAVA_BYTE, ROUTING_ID_DATA_OFFSET + 3) & 0xFF;
        return Integer.toUnsignedLong((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
    }

    private static MemorySegment contextHandle(Context ctx) {
        try {
            Method m = Context.class.getDeclaredMethod("handle");
            m.setAccessible(true);
            Object out = m.invoke(ctx);
            if (out instanceof MemorySegment)
                return (MemorySegment) out;
        } catch (Throwable ignored) {
        }
        return MemorySegment.NULL;
    }

    private static void setRawIntOption(Socket socket, int option, int value) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = arena.allocate(ValueLayout.JAVA_INT);
            buf.set(ValueLayout.JAVA_INT, 0, value);
            int rc = Native.setSockOpt(socket.handle(), option, buf,
              ValueLayout.JAVA_INT.byteSize());
            if (rc != 0)
                throw new RuntimeException("zlink_setsockopt failed");
        }
    }

    private static int runServer(ServerOptions opt, Metrics metrics) {
        Context ctx = null;
        Socket server = null;
        StreamCallbackEcho echo = null;
        try {
            ctx = new Context();
            MemorySegment ctxHandle = contextHandle(ctx);
            if (ctxHandle != null && ctxHandle.address() != 0) {
                int rc = ctxSet(ctxHandle, ContextOption.IO_THREADS.getValue(),
                  Math.max(1, opt.ioThreads));
                if (rc != 0)
                    throw new RuntimeException("zlink_ctx_set(IO_THREADS) failed");
            }
            server = new Socket(ctx, SocketType.STREAM);
            server.setSockOpt(SocketOption.SNDBUF, opt.sndbuf);
            server.setSockOpt(SocketOption.RCVBUF, opt.rcvbuf);
            server.setSockOpt(SocketOption.BACKLOG, opt.backlog);
            server.setSockOpt(SocketOption.SNDHWM, 100);
            server.setSockOpt(SocketOption.RCVHWM, 100);
            setRawIntOption(server, ZLINK_TCP_NODELAY, opt.tcpNoDelay);

            server.bind(endpoint(opt.host, opt.port));
            echo = new StreamCallbackEcho(server, metrics);
            echo.attach();

            while (true) {
                Thread.sleep(200);
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return 0;
        } catch (Throwable t) {
            System.err.printf("jvmzlink-len32be stream: %s%n", t.getMessage());
            return 2;
        } finally {
            if (echo != null) {
                try {
                    echo.close();
                } catch (Throwable ignored) {
                }
            }
            if (server != null) {
                try {
                    server.close();
                } catch (Throwable ignored) {
                }
            }
            if (ctx != null) {
                try {
                    ctx.close();
                } catch (Throwable ignored) {
                }
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
            System.err.printf("jvmzlink-len32be stream: size too large %d%n",
              opt.size);
            System.exit(2);
            return;
        }

        Metrics metrics = new Metrics();
        int rc = runServer(opt, metrics);
        System.out.printf(
          "METRIC stack=jvmzlink-len32be mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d connections=%d%n",
          opt.size,
          metrics.recvMsgs.get(),
          metrics.parseError.get(),
          metrics.protocolError.get(),
          metrics.sendError.get(),
          0);
        if (rc != 0)
            System.exit(rc);
    }
}
