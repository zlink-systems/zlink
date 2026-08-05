/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.messaging.TopicMessage;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;

public final class PerfUtil {
    public static final int PHASE_UNKNOWN = -1;
    public static final int PHASE_WARMUP = 0;
    public static final int PHASE_ACTIVE = 1;
    public static final int PHASE_COOLDOWN = 2;
    public static final int HEADER_SIZE = 29;

    private PerfUtil() {
    }

    public record Config(
        String suite,
        String pattern,
        String transport,
        int size,
        int durationSeconds,
        String endpoint,
        int clients,
        int ioThreads,
        long sendHwm,
        long recvHwm,
        int sendBuffer,
        int recvBuffer,
        int sendTimeoutMs,
        int recvTimeoutMs,
        int monitorHwm,
        int connectReadyTimeoutMs,
        int connectConcurrency
    ) {
    }

    public record Header(byte phase, long latencyNanos, long sentTsNanos) {
    }

    public static final class Result {
        final String status;
        final String reason;
        final String pattern;
        final String transport;
        final int size;
        final double throughput;
        final double bandwidth;
        final double latencyMean;
        final double latencyP95;
        final double latencyP99;

        public Result(String status, String reason, String pattern, String transport,
                      int size, double throughput, double bandwidth,
                      double latencyMean, double latencyP95, double latencyP99) {
            this.status = status;
            this.reason = reason;
            this.pattern = pattern;
            this.transport = transport;
            this.size = size;
            this.throughput = throughput;
            this.bandwidth = bandwidth;
            this.latencyMean = latencyMean;
            this.latencyP95 = latencyP95;
            this.latencyP99 = latencyP99;
        }

        public static Result unsupported(String reason, Config config) {
            return new Result("unsupported", reason, config.pattern(),
                config.transport(), config.size(), 0.0d, 0.0d, 0.0d, 0.0d,
                0.0d);
        }

        public static Result silent(Config config) {
            return new Result("silent", "-", config.pattern(), config.transport(),
                config.size(), 0.0d, 0.0d, 0.0d, 0.0d, 0.0d);
        }

        public String status() {
            return status;
        }

        public String toLine(String libTag) {
            return PerfReport.format(this, libTag);
        }
    }

    public static final class Metrics {
        private final PerfMetricsCollector delegate;

        public Metrics(Config config) {
            delegate = new PerfMetricsCollector(config.suite());
        }

        public void recordNanos(long value) {
            delegate.recordNanos(value);
        }

        public void recordEvent() {
            delegate.recordEvent();
        }

        public void recordLatencySampleNanos(long value) {
            delegate.recordLatencySampleNanos(value);
        }

        public Result finishSingle(Config config) {
            return delegate.finishSingle(config);
        }

        public Result finishMulti(Config config) {
            return delegate.finishMulti(config);
        }
    }

    public static Config parseSingleArgs(String[] args) {
        return PerfArgs.parseSingleArgs(args);
    }

    public static Config parseMultiArgs(String[] args) {
        return PerfArgs.parseMultiArgs(args);
    }

    public static int runId() {
        return PerfMeasurement.runId();
    }

    public static Message payload(int size, byte phase, long sentNanoTime) {
        return PerfMeasurement.payload(size, phase, sentNanoTime);
    }

    public static Message payloadTemplate(int size) {
        return PerfMeasurement.payloadTemplateForReuse(size);
    }

    public static Message resetAndWritePayload(Message payload, int size,
                                               byte phase, long sentNanoTime) {
        return PerfMeasurement.resetAndWritePayload(payload, size, phase,
            sentNanoTime);
    }

    public static void writePayload(Message payload, int size, byte phase,
                                    long sentNanoTime) {
        PerfMeasurement.writePayload(payload, size, phase, sentNanoTime);
    }

    public static Header decodeHeader(Message message, int expectedSize) {
        return PerfMetricHeader.decode(message, expectedSize);
    }

    public static boolean recordActiveLatency(Metrics metrics, Message message,
                                              int expectedSize,
                                              boolean halfRoundTrip) {
        return PerfMetricHeader.recordActiveLatency(metrics, message,
            expectedSize, halfRoundTrip);
    }

    public static String endpoint(String transport, String token) {
        return PerfMeasurement.endpoint(transport, token);
    }

    public static String bindEndpoint(String endpoint, String transport) {
        return switch (transport) {
            case "tcp", "tls", "ws", "wss" ->
                endpoint.replaceFirst(":[0-9]+$", ":*");
            default -> endpoint;
        };
    }

    public static String connectedEndpoint(Socket boundSocket, String fallback,
                                           String transport) {
        return switch (transport) {
            case "tcp", "tls", "ws", "wss" -> boundSocket.options().lastEndpoint();
            default -> fallback;
        };
    }

    public static boolean isEchoPattern(String pattern) {
        return PerfMeasurement.isEchoPattern(pattern);
    }

    public static Result classifyFailure(Config config, Throwable failure) {
        return PerfPolicy.classifyFailure(config, failure);
    }

    public static void configureServerTls(Socket socket, String transport) {
        PerfTransport.configureServerTls(socket, transport);
    }

    public static void configureClientTls(Socket socket, String transport) {
        PerfTransport.configureClientTls(socket, transport);
    }

    public static Context newContext(Config config) {
        return PerfTransport.newContext(config);
    }

    public static void applySocketOptions(Socket socket, Config config) {
        PerfTransport.applySocketOptions(socket, config);
    }

    public static void recalculateAutoHwm(Context ctx) {
        PerfTransport.recalculateAutoHwm(ctx);
    }

    public static void printSingleMonitorAutoHwm(Config config,
                                                 SocketMonitor monitor,
                                                 String component,
                                                 SocketType socketType) {
        PerfAutoHwm.printSingleMonitor(config, monitor, component, socketType);
    }

    public static void printMultiSocketAutoHwm(Config config, Socket socket,
                                               String component, String label,
                                               SocketType socketType) {
        PerfAutoHwm.printMultiSocket(config, socket, component, label,
            socketType);
    }

    public static void printMultiMonitorAutoHwm(Config config,
                                                SocketMonitor monitor,
                                                String component, String label,
                                                SocketType socketType) {
        PerfAutoHwm.printMultiMonitor(config, monitor, component, label,
            socketType);
    }

    public static void await(CountDownLatch latch, String label, Duration timeout) {
        PerfTransport.await(latch, label, timeout);
    }

    public static void join(Thread thread, String label, Duration timeout) {
        PerfTransport.join(thread, label, timeout);
    }

    public static void waitForMonitorEvent(SocketMonitor monitor,
                                           systems.zlink.contracts.eventing.MonitorEventType expectedEvent,
                                           int expectedCount, Duration timeout,
                                           String label) {
        PerfTransport.waitForMonitorEvent(monitor, expectedEvent, expectedCount,
            timeout, label);
    }

    public static systems.zlink.contracts.messaging.Received recvNoWait(PairSocket socket) {
        try {
            systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received();
            return socket.recv(received, RecvFlags.DONT_WAIT) ? received : null;
        } catch (ZlinkRecvException ex) {
            return recvExceptionToNull(ex);
        }
    }

    public static systems.zlink.contracts.messaging.Received recvNoWait(DealerSocket socket) {
        try {
            systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received();
            return socket.recv(received, RecvFlags.DONT_WAIT) ? received : null;
        } catch (ZlinkRecvException ex) {
            return recvExceptionToNull(ex);
        }
    }

    public static systems.zlink.contracts.messaging.Received recvNoWait(RouterSocket socket) {
        try {
            systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received();
            return socket.recv(received, RecvFlags.DONT_WAIT) ? received : null;
        } catch (ZlinkRecvException ex) {
            return recvExceptionToNull(ex);
        }
    }

    private static systems.zlink.contracts.messaging.Received recvExceptionToNull(ZlinkRecvException ex) {
        if (ex.getResult() == RecvResult.NO_DATA
            || ex.getResult() == RecvResult.BUSY) {
            return null;
        }
        throw ex;
    }

    public static Optional<TopicMessage> subscribeNoWait(SubSocket socket) {
        TopicMessage message = new TopicMessage();
        return socket.subscribe(message, RecvFlags.DONT_WAIT)
            ? Optional.of(message)
            : Optional.empty();
    }

    public static void applyMonitorOptions(SocketMonitor monitor, Config config) {
        PerfTransport.applyMonitorOptions(monitor, config);
    }

    public static long nowNs() {
        return PerfMeasurement.nowNs();
    }

    private static <T> Optional<T> tryOptional(CheckedSupplier<T> supplier) {
        try {
            return Optional.ofNullable(supplier.get());
        } catch (ZlinkRecvException ex) {
            if (ex.getResult() == RecvResult.NO_DATA
                || ex.getResult() == RecvResult.BUSY) {
                return Optional.empty();
            }
            throw ex;
        }
    }

    @FunctionalInterface
    private interface CheckedSupplier<T> {
        T get();
    }

    public static int intEnv(String name, int fallback) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return Integer.parseInt(value);
    }
}
