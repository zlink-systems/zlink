/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.samples;

import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.RecvFlags;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class SampleSupport {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    static final String PAIR_PAYLOAD = "hello-pair";
    static final String DEALER_REQUEST = "ping";
    static final String DEALER_REPLY = "pong";
    static final String STREAM_PAYLOAD = "hello-stream";
    static final String PUBSUB_TOPIC = "prices";
    static final String PUBSUB_PAYLOAD = "101.25";
    private static final int TCP_PORT_BASE =
        40_000 + (int) (ProcessHandle.current().pid() % 10_000);
    private static final AtomicInteger NEXT_TCP_PORT =
        new AtomicInteger(TCP_PORT_BASE);

    private SampleSupport() {
    }

    static String tcpEndpoint() {
        return "tcp://127.0.0.1:" + nextTcpPort();
    }

    private static int nextTcpPort() {
        for (int attempt = 0; attempt < 200; attempt++) {
            int port = NEXT_TCP_PORT.getAndIncrement();
            if (isBindable(port)) {
                return port;
            }
        }
        throw new IllegalStateException("failed to allocate tcp port");
    }

    private static boolean isBindable(int port) {
        try (ServerSocket server = new ServerSocket(port)) {
            server.setReuseAddress(false);
            return true;
        } catch (IOException ex) {
            return false;
        }
    }

    static void ensureNative() {
        Zlink.version();
    }

    static void await(CountDownLatch latch, String label) {
        try {
            if (!latch.await(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS)) {
                throw new IllegalStateException(label + " timed out");
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(label + " interrupted", ex);
        }
    }

    static void waitUntil(String label, Condition condition) {
        long deadlineNanos = System.nanoTime() + TIMEOUT.toNanos();
        while (System.nanoTime() < deadlineNanos) {
            if (condition.check()) {
                return;
            }
            sleepQuietly(label);
        }
        throw new IllegalStateException(label + " timed out");
    }

    private static void sleepQuietly(String label) {
        try {
            TimeUnit.MILLISECONDS.sleep(10);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(label + " interrupted", ex);
        }
    }

    static String singleUtf8(Received received) {
        return received.singlePartOrThrow().toUtf8String();
    }

    static void waitConnected(SocketMonitor... monitors) {
        for (SocketMonitor monitor : monitors) {
            waitConnectedMonitor(monitor);
        }
    }

    private static void waitConnectedMonitor(SocketMonitor monitor) {
        long deadlineNanos = System.nanoTime() + TIMEOUT.toNanos();
        try (Poller poller = Zlink.createPoller()) {
            poller.add(monitor, 0L, PollEventFlags.POLLIN);
            PollEvents ready = new PollEvents(1);
            while (System.nanoTime() < deadlineNanos) {
                Duration remaining = Duration.ofNanos(
                    deadlineNanos - System.nanoTime());
                if (poller.wait(ready, remaining) == 0) {
                    break;
                }
                var event = monitor.recv(RecvFlags.DONT_WAIT);
                if (event == null) {
                    continue;
                }
                if (event.event() == MonitorEventType.CONNECTION_READY) {
                    return;
                }
                throw new IllegalStateException(
                    "unexpected monitor event: " + event.event());
            }
        }
        throw new IllegalStateException("socket connection timed out");
    }

    static void waitStreamConnected(SocketMonitor monitor) {
        try (Poller poller = Zlink.createPoller()) {
            poller.add(monitor, 0L, PollEventFlags.POLLIN);
            PollEvents ready = new PollEvents(1);
            while (poller.wait(ready, TIMEOUT) != 0) {
                var event = monitor.recv(RecvFlags.DONT_WAIT);
                if (event != null && (event.event() == MonitorEventType.ACCEPTED
                    || event.event() == MonitorEventType.CONNECTION_READY)) {
                    return;
                }
            }
        }
        throw new IllegalStateException("stream connection timed out");
    }

    static void waitPubSubReady(SocketMonitor pubMonitor,
                                SocketMonitor subMonitor) {
        waitMonitorEvent(subMonitor, MonitorEventType.CONNECTION_READY);
        waitMonitorEvent(pubMonitor, MonitorEventType.CONNECTION_READY);
    }

    static void closeQuietly(AutoCloseable resource) {
        if (resource == null) {
            return;
        }
        try {
            resource.close();
        } catch (Exception ignored) {
        }
    }

    static java.net.Socket connectRawTcp(String endpoint) {
        try {
            InetSocketAddress address = tcpAddress(endpoint);
            java.net.Socket socket = new java.net.Socket();
            socket.connect(address, (int) TIMEOUT.toMillis());
            socket.setSoTimeout((int) TIMEOUT.toMillis());
            return socket;
        } catch (IOException ex) {
            throw new IllegalStateException("failed to connect raw tcp client", ex);
        }
    }

    static void sendRawTcp(java.net.Socket socket, byte[] payload) {
        try {
            OutputStream out = socket.getOutputStream();
            out.write(payload);
            out.flush();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to send raw tcp payload", ex);
        }
    }

    /** Sends a STREAM socket packet frame (empty header, big-endian lengths). */
    static void sendStreamPacket(java.net.Socket socket, byte[] body) {
        byte[] frame = new byte[6 + body.length];
        frame[2] = (byte) (body.length >>> 24);
        frame[3] = (byte) (body.length >>> 16);
        frame[4] = (byte) (body.length >>> 8);
        frame[5] = (byte) body.length;
        System.arraycopy(body, 0, frame, 6, body.length);
        sendRawTcp(socket, frame);
    }

    static byte[] recvExactRawTcp(java.net.Socket socket, int length) {
        try {
            InputStream in = socket.getInputStream();
            return readExactly(in, length);
        } catch (IOException ex) {
            throw new IllegalStateException("failed to receive raw tcp payload", ex);
        }
    }

    private static InetSocketAddress tcpAddress(String endpoint) {
        if (!endpoint.startsWith("tcp://")) {
            throw new IllegalArgumentException(
                "raw tcp helper requires tcp:// endpoint: " + endpoint);
        }
        String hostPort = endpoint.substring("tcp://".length());
        int colon = hostPort.lastIndexOf(':');
        if (colon <= 0 || colon == hostPort.length() - 1) {
            throw new IllegalArgumentException("invalid tcp endpoint: " + endpoint);
        }
        String host = hostPort.substring(0, colon);
        int port = Integer.parseInt(hostPort.substring(colon + 1));
        return new InetSocketAddress(host, port);
    }

    private static byte[] readExactly(InputStream in, int length)
      throws IOException {
        byte[] data = new byte[length];
        int read = 0;
        while (read < length) {
            int n = in.read(data, read, length - read);
            if (n < 0) {
                throw new IOException("unexpected end of stream");
            }
            read += n;
        }
        return data;
    }

    private static void waitMonitorEvent(SocketMonitor monitor,
                                         MonitorEventType eventType) {
        try (Poller poller = Zlink.createPoller()) {
            poller.add(monitor, 0L, PollEventFlags.POLLIN);
            PollEvents ready = new PollEvents(1);
            while (poller.wait(ready, TIMEOUT) != 0) {
                var event = monitor.recv(RecvFlags.DONT_WAIT);
                if (event != null && event.event() == eventType) {
                    return;
                }
            }
        }
        throw new IllegalStateException("monitor event timed out: " + eventType);
    }

    @FunctionalInterface
    interface Condition {
        boolean check();
    }
}
