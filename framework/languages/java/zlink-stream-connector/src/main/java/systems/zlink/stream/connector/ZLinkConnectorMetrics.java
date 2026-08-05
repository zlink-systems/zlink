package systems.zlink.stream.connector;

import io.micrometer.core.instrument.Metrics;

final class ZLinkConnectorMetrics {
    private ZLinkConnectorMetrics() { }

    static void reconnectAttempt() {
        Metrics.counter("zlink.stream.reconnects").increment();
    }
}
