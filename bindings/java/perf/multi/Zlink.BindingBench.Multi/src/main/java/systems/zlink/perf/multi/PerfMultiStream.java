/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class PerfMultiStream {
    private PerfMultiStream() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        AtomicBoolean stopRequested = new AtomicBoolean(false);
        Object stopSignal = new Object();
        PendingPackets pending = new PendingPackets();
        Thread controlWatcher = startControlWatcher(stopRequested, stopSignal);

        try (Context ctx = PerfUtil.newContext(config);
            StreamSocket server = ctx.createStreamSocket()) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            Duration streamTimeout = Duration.ofMillis(streamTimeoutMs());
            server.options().sendTimeout(streamTimeout);
            server.options().recvTimeout(streamTimeout);
            server.bind(config.endpoint());
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiSocketAutoHwm(config, server, "server",
                "server", SocketType.STREAM);
            PerfControl.emitReady(config.endpoint());
            server.onPacket(
                (routingId, header, body) ->
                    onPacket(server, routingId, header, body,
                        pending, stopRequested, stopSignal));

            runPendingLoop(server, pending, stopRequested, stopSignal);
            return PerfUtil.Result.silent(config);
        } finally {
            stopRequested.set(true);
            pending.close();
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
                                 PendingPackets pending,
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
            pending.sendOrQueue(server, routingId, buildPacketFrame(header, body),
                stopRequested, stopSignal);
        } catch (RuntimeException ex) {
            stopRequested.set(true);
            signal(stopSignal);
            throw ex;
        }
    }

    // C parity: write the framing prefix and the received native frames into
    // the outbound native Message. The public Message API copies native-to-
    // native, so this avoids staging the complete packet in a Java byte[].
    private static Message buildPacketFrame(Message header, Message body) {
        int headerSize = header.size();
        int bodySize = body.size();
        int total = 6 + headerSize + bodySize;
        Message packet = Message.allocate(total);
        packet.writeShortBe(0, (short) headerSize);
        packet.writeIntBe(2, bodySize);
        packet.copyFrom(header, 0, 6, headerSize);
        packet.copyFrom(body, 0, 6 + headerSize, bodySize);
        return packet;
    }

    private static void runPendingLoop(StreamSocket server,
                                       PendingPackets pending,
                                       AtomicBoolean stopRequested,
                                       Object stopSignal) {
        // C runs a dedicated pending-send loop: a queued reply waits for the
        // STREAM socket's POLLOUT readiness and is then drained until it
        // backpressures again. Keep this benchmark's explicit polling loop;
        // the runtime does not own a readiness scheduler for the operation.
        try (PerfSocketPollSet writable = PerfSocketPollSet.fromSockets(
                 List.of(server), PollEventFlags.POLLOUT)) {
            while (!stopRequested.get()) {
                if (!pending.hasPending()) {
                    // C waits on its pending-queue condition with the same
                    // auxiliary 50 ms control bound before it has POLLOUT
                    // work. This is not an active-phase timer fallback.
                    pending.awaitWork(stopRequested, 50);
                    continue;
                }
                writable.setEvents(0, PollEventFlags.POLLOUT);
                int readyCount = writable.poll(50);
                if (readyCount > 0
                    && writable.readyHasEventAt(0, PollEventFlags.POLLOUT)) {
                    pending.drain(server, stopRequested, stopSignal);
                }
            }
        }
    }

    private static void signal(Object stopSignal) {
        synchronized (stopSignal) {
            stopSignal.notifyAll();
        }
    }

    private static int streamTimeoutMs() {
        String configured = System.getenv("PERF_STREAM_TIMEOUT_MS");
        if (configured == null || configured.isBlank()) {
            return 5_000;
        }
        try {
            int value = Integer.parseInt(configured);
            return value >= 0 ? value : 5_000;
        } catch (NumberFormatException ignored) {
            return 5_000;
        }
    }

    private static final class PendingPackets {
        private final Object lock = new Object();
        private final Deque<PendingPacket> queue = new ArrayDeque<>();
        private boolean closed;

        void sendOrQueue(StreamSocket socket, RoutingId routingId, Message packet,
                         AtomicBoolean stopRequested, Object stopSignal) {
            synchronized (lock) {
                if (closed || stopRequested.get()) {
                    packet.close();
                    return;
                }
                if (queue.isEmpty()) {
                    try {
                        if (trySend(socket, routingId, packet)) {
                            return;
                        }
                    } catch (ZlinkSubmitException ex) {
                        if (!isTransient(ex)) {
                            packet.close();
                            throw ex;
                        }
                    }
                }
                queue.addLast(new PendingPacket(routingId, packet));
                lock.notifyAll();
            }
        }

        boolean hasPending() {
            synchronized (lock) {
                return !queue.isEmpty();
            }
        }

        void awaitWork(AtomicBoolean stopRequested, long timeoutMs) {
            synchronized (lock) {
                if (closed || stopRequested.get() || !queue.isEmpty()) {
                    return;
                }
                try {
                    lock.wait(timeoutMs);
                } catch (InterruptedException ex) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("stream pending wait interrupted", ex);
                }
            }
        }

        void drain(StreamSocket socket, AtomicBoolean stopRequested,
                   Object stopSignal) {
            synchronized (lock) {
                while (!closed && !stopRequested.get() && !queue.isEmpty()) {
                    PendingPacket packet = queue.peekFirst();
                    try {
                        if (!trySend(socket, packet.routingId(), packet.message())) {
                            return;
                        }
                    } catch (ZlinkSubmitException ex) {
                        if (isTransient(ex)) {
                            return;
                        }
                        queue.removeFirst();
                        packet.message().close();
                        stopRequested.set(true);
                        signal(stopSignal);
                        throw ex;
                    }
                    queue.removeFirst();
                }
            }
        }

        void close() {
            synchronized (lock) {
                closed = true;
                while (!queue.isEmpty()) {
                    queue.removeFirst().message().close();
                }
            }
        }

        private static boolean trySend(StreamSocket socket, RoutingId routingId,
                                       Message packet) {
            return socket.send(routingId)
                .message(packet)
                .flags(SendFlags.DONT_WAIT)
                .submit();
        }
    }

    private record PendingPacket(RoutingId routingId, Message message) {
    }

    private static boolean isTransient(ZlinkSubmitException ex) {
        return ex.getResult() == SubmitResult.BACKPRESSURED
            || ex.getResult() == SubmitResult.NOT_CONNECTED;
    }
}
