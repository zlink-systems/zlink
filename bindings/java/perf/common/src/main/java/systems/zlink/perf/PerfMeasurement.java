/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.PooledByteBufAllocator;
import java.net.ServerSocket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

final class PerfMeasurement {
    private static final int MAGIC = 0x5A4C4E4B;
    private static final int RUN_ID = 1;
    private static final AtomicLong SEQ = new AtomicLong();
    private static final boolean USE_NETTY_BYTEBUF_POOL =
        "1".equals(System.getenv("PERF_JAVA_NETTY_BYTEBUF_POOL"));

    private PerfMeasurement() {
    }

    static int runId() {
        return RUN_ID;
    }

    static Message payload(int size, byte phase, long sentNanoTime) {
        Message payload = payloadTemplate(size);
        writePayload(payload, size, phase, sentNanoTime);
        return payload;
    }

    static Message payloadTemplateForReuse(int size) {
        return payloadTemplate(size);
    }

    private static Message payloadTemplate(int size) {
        int capacity = Math.max(size, PerfUtil.HEADER_SIZE);
        if (USE_NETTY_BYTEBUF_POOL) {
            return nettyPooledPayloadTemplate(size, capacity);
        }
        Message payload = new Message(capacity);
        payload.fill((byte) 'a');
        payload.writeIntLe(0, MAGIC);
        payload.writeIntLe(4, RUN_ID);
        payload.writeIntLe(9, size);
        return payload;
    }

    private static Message nettyPooledPayloadTemplate(int size, int capacity) {
        ByteBuf payload = PooledByteBufAllocator.DEFAULT.directBuffer(
            capacity, capacity);
        try {
            payload.writerIndex(capacity);
            for (int i = 0; i < capacity; i++) {
                payload.setByte(i, 'a');
            }
            payload.setIntLE(0, MAGIC);
            payload.setIntLE(4, RUN_ID);
            payload.setIntLE(9, size);
            return Message.from(payload);
        } finally {
            payload.release();
        }
    }

    static void writePayload(Message payload, int size, byte phase, long sentNanoTime) {
        long sentTsNs = nowNs();
        writePayloadHeader(payload, size, phase, SEQ.getAndIncrement(), sentTsNs);
    }

    static Message resetAndWritePayload(Message payload, int size, byte phase,
                                        long sentNanoTime) {
        if (payload != null) {
            payload.close();
        }
        payload = payloadTemplate(size);
        writePayload(payload, size, phase, sentNanoTime);
        return payload;
    }

    static void writePayloadHeader(Message payload, int size, byte phase,
                                   long seq, long sentTsNs) {
        payload.writeIntLe(0, MAGIC);
        payload.writeIntLe(4, RUN_ID);
        payload.writeByte(8, phase);
        payload.writeIntLe(9, size);
        payload.writeLongLe(13, seq);
        payload.writeLongLe(21, sentTsNs);
    }

    static String endpoint(String transport, String token) {
        return switch (transport) {
            case "tcp", "tls", "ws", "wss" -> transport + "://127.0.0.1:" + freePort();
            case "inproc" -> "inproc://perf-" + token + "-" + UUID.randomUUID();
            case "ipc" -> "ipc://" + ipcFile(token).toAbsolutePath();
            default -> throw new IllegalArgumentException("unsupported transport: " + transport);
        };
    }

    static boolean isEchoPattern(String pattern) {
        return "DEALER_ROUTER".equals(pattern)
            || "DEALER_ROUTER_REQREP".equals(pattern)
            || "ROUTER_ROUTER".equals(pattern)
            || "ROUTER_ROUTER_REQREP".equals(pattern)
            || "STREAM".equals(pattern)
            || "SPOT_REQREP".equals(pattern)
            || "SPOT_SENDSEND".equals(pattern)
            || "MULTI_DEALER_ROUTER_REQREP".equals(pattern)
            || "MULTI_ROUTER_ROUTER_REQREP".equals(pattern)
            || "MULTI_SPOT_REQREP".equals(pattern)
            || "MULTI_SPOT_SENDSEND".equals(pattern);
    }

    static long nowNs() {
        return System.nanoTime();
    }

    static double bytesToMb(long bytes) {
        return bytes / 1_000_000.0d;
    }

    private static int freePort() {
        try (ServerSocket socket = new ServerSocket(0)) {
            return socket.getLocalPort();
        } catch (java.io.IOException ex) {
            throw new IllegalStateException("failed to allocate port", ex);
        }
    }

    private static Path ipcFile(String token) {
        Path dir = Path.of(System.getProperty("java.io.tmpdir"), "zlink-java-perf-ipc");
        try {
            Files.createDirectories(dir);
        } catch (java.io.IOException ex) {
            throw new IllegalStateException("failed to create ipc dir", ex);
        }
        String safeToken = token.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "");
        if (safeToken.length() > 8) {
            safeToken = safeToken.substring(0, 8);
        }
        String id = UUID.randomUUID().toString().replace("-", "").substring(0, 8);
        return dir.resolve("zj-" + safeToken + "-" + id + ".sock");
    }
}
