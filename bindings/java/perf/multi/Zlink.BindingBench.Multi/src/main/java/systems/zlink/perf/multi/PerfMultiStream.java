/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.StreamRecvMode;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;

final class PerfMultiStream {
    private PerfMultiStream() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             StreamSocket server = ctx.createStreamSocket();
             var monitor = server.monitorOpen(config.monitorHwm(),
                 MonitorEventType.CONNECTION_READY)) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.options().recvMode(StreamRecvMode.PACKET);
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            if (!PerfControl.awaitStartOrStop(config.size(),
                    "multi stream server")) {
                return PerfUtil.Result.silent(config);
            }
            PerfUtil.waitForMonitorEvent(monitor,
                MonitorEventType.CONNECTION_READY, config.clients(),
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "multi stream server connections ready");
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiSocketAutoHwm(config, server, "server",
                "server-connected", SocketType.STREAM);
            PerfControl.emitServerStartReady(config.size());

            AtomicBoolean stopRequested =
                PerfControl.watchStopSignal("multi stream server");
            runPullEcho(server, stopRequested);
            return PerfUtil.Result.silent(config);
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        throw new UnsupportedOperationException(
            "MULTI_STREAM uses the external raw stream client");
    }

    private static void runPullEcho(StreamSocket server,
                                    AtomicBoolean stopRequested) {
        try (PerfSocketPollSet poller = PerfSocketPollSet.fromSockets(
                 List.of(server),
                 systems.zlink.contracts.eventing.PollEventFlags.POLLIN);
             StreamPacket packet = new StreamPacket()) {
            while (!stopRequested.get()) {
                int ready = poller.poll(50);
                if (ready <= 0 || !poller.readyHasEventAt(0,
                        systems.zlink.contracts.eventing.PollEventFlags.POLLIN)) {
                    continue;
                }
                while (!stopRequested.get() && recvPacket(server, packet)) {
                    if (PerfStopToken.isStopTokenMessage(packet.body())) {
                        stopRequested.set(true);
                        break;
                    }
                    RoutingId routingId = packet.routingId().orElseThrow();
                    try (Message frame = buildPacketFrame(
                             packet.header(), packet.body())) {
                        server.send(routingId).message(frame).submit_sync();
                    }
                }
            }
        }
    }

    private static boolean recvPacket(StreamSocket server,
                                      StreamPacket packet) {
        try {
            return server.recvPacket(packet, RecvFlags.DONT_WAIT);
        } catch (ZlinkRecvException error) {
            if (error.getResult() == RecvResult.NO_DATA
                || error.getResult() == RecvResult.BUSY
                || PerfErrno.isRetryableRecv(error.getNativeErrno())) {
                return false;
            }
            throw error;
        }
    }

    private static Message buildPacketFrame(Message header, Message body) {
        int headerSize = header.size();
        int bodySize = body.size();
        Message frame = Message.allocate(6 + headerSize + bodySize);
        frame.writeShortBe(0, (short) headerSize);
        frame.writeIntBe(2, bodySize);
        frame.copyFrom(header, 0, 6, headerSize);
        frame.copyFrom(body, 0, 6 + headerSize, bodySize);
        return frame;
    }
}
