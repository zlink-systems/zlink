/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

final class PerfMultiStream {
    private PerfMultiStream() {
    }

    /**
     * Reusable per-server frame scratch buffer. The stream packet callback is
     * dispatched on a single socket-owned thread, so a single growable buffer
     * is reused across echoed packets instead of allocating
     * {@code new byte[6 + header + body]} per packet. This mirrors the C
     * reference (perf_multi_stream_session.hpp build_packet_frame ~192-221),
     * which writes the framed payload into a single message buffer without an
     * intermediate per-packet heap allocation or a redundant second copy.
     */
    private static final class FrameScratch {
        private byte[] buffer = new byte[0];

        byte[] ensureCapacity(int required) {
            if (buffer.length < required) {
                int next = Math.max(required, buffer.length * 2);
                buffer = new byte[next];
            }
            return buffer;
        }
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        AtomicBoolean stopRequested = new AtomicBoolean(false);
        Object stopSignal = new Object();
        Thread controlWatcher = startControlWatcher(stopRequested, stopSignal);

        try (Context ctx = PerfUtil.newContext(config);
            StreamSocket server = ctx.createStreamSocket()) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiSocketAutoHwm(config, server, "server",
                "server", SocketType.STREAM);
            PerfUtil.configureServerTls(server, config.transport());
            server.options().sendTimeout(java.time.Duration.ZERO);
            server.options().recvTimeout(java.time.Duration.ZERO);
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            FrameScratch scratch = new FrameScratch();
            server.onPacket(
                (routingId, header, body) ->
                    onPacket(server, routingId, header, body, scratch,
                        stopRequested, stopSignal));

            waitForStop(stopRequested, stopSignal);
            return PerfUtil.Result.silent(config);
        } finally {
            controlWatcher.interrupt();
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        throw new IllegalStateException("MULTI_STREAM requires the shared raw stream client");
    }

    private static Thread startControlWatcher(AtomicBoolean stopRequested,
                                              Object stopSignal) {
        Thread watcher = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if ("STOP".equals(line) || "QUIT".equals(line)) {
                        stopRequested.set(true);
                        signal(stopSignal);
                        return;
                    }
                }
            } catch (Exception ex) {
                throw new IllegalStateException("stream control watcher failed", ex);
            }
        }, "stream-control");
        watcher.setDaemon(true);
        watcher.start();
        return watcher;
    }

    private static void onPacket(StreamSocket server,
                                 RoutingId routingId,
                                 Message header,
                                 Message body,
                                 FrameScratch scratch,
                                 AtomicBoolean stopRequested,
                                 Object stopSignal) {
        if (routingId == null) {
            return;
        }
        if (PerfStopToken.isStopTokenMessage(body)) {
            stopRequested.set(true);
            signal(stopSignal);
            return;
        }
        try {
            sendFramedPacket(server, routingId, header, body, scratch);
        } catch (RuntimeException ex) {
            stopRequested.set(true);
            signal(stopSignal);
            throw ex;
        }
    }

    private static void sendFramedPacket(StreamSocket socket,
                                         RoutingId routingId,
                                         Message header,
                                         Message body,
                                         FrameScratch scratch) {
        try (Message packet = buildPacketFrame(header, body, scratch)) {
            if (!socket.send(routingId)
                .message(packet)
                .flags(SendFlags.DONT_WAIT)
                .submit()) {
                throw new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            }
        }
    }

    // C parity: perf_multi_stream_session.hpp build_packet_frame (~192-221)
    // writes the 2-byte BE header length, 4-byte BE body length, header and
    // body into a single message buffer with no intermediate per-packet heap
    // allocation. The frame staging buffer is reused across packets (allocated
    // once per server, grown only when a larger frame is seen); Message.from
    // is the single native copy into the owned send frame, matching C's single
    // zlink_msg_init_size + memcpy.
    private static Message buildPacketFrame(Message header, Message body,
                                            FrameScratch scratch) {
        int headerSize = header.size();
        int bodySize = body.size();
        int total = 6 + headerSize + bodySize;
        byte[] frame = scratch.ensureCapacity(total);
        frame[0] = (byte) ((headerSize >>> 8) & 0xFF);
        frame[1] = (byte) (headerSize & 0xFF);
        frame[2] = (byte) ((bodySize >>> 24) & 0xFF);
        frame[3] = (byte) ((bodySize >>> 16) & 0xFF);
        frame[4] = (byte) ((bodySize >>> 8) & 0xFF);
        frame[5] = (byte) (bodySize & 0xFF);
        header.copyTo(frame, 0, 6, headerSize);
        body.copyTo(frame, 0, 6 + headerSize, bodySize);
        return Message.from(frame, 0, total);
    }

    private static void waitForStop(AtomicBoolean stopRequested, Object stopSignal) {
        synchronized (stopSignal) {
            while (!stopRequested.get()) {
                try {
                    stopSignal.wait();
                } catch (InterruptedException ex) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("stream stop wait interrupted", ex);
                }
            }
        }
    }

    private static void signal(Object stopSignal) {
        synchronized (stopSignal) {
            stopSignal.notifyAll();
        }
    }
}
