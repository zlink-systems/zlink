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
            server.onPacket(
                (routingId, header, body) ->
                    onPacket(server, routingId, header, body,
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
            sendFramedPacket(server, routingId, header, body);
        } catch (RuntimeException ex) {
            stopRequested.set(true);
            signal(stopSignal);
            throw ex;
        }
    }

    private static void sendFramedPacket(StreamSocket socket,
                                         RoutingId routingId,
                                         Message header,
                                         Message body) {
        try (Message packet = buildPacketFrame(header, body)) {
            if (!socket.send(routingId)
                .message(packet)
                .flags(SendFlags.DONT_WAIT)
                .submit()) {
                throw new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            }
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
