/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/** Polls a bench server's stats endpoint: reset, snapshot, and the settle drain. */
public final class StatsClient {
    private final HttpClient http = HttpClient.newBuilder()
        .connectTimeout(Duration.ofSeconds(5))
        .build();

    public void reset(String statsUrl) throws Exception {
        HttpRequest request = HttpRequest.newBuilder(URI.create(statsUrl + "/bench/reset"))
            .POST(HttpRequest.BodyPublishers.noBody())
            .timeout(Duration.ofSeconds(10))
            .build();
        HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) {
            throw new IllegalStateException(
                "reset " + statsUrl + " failed: " + response.statusCode());
        }
    }

    public ServerSnapshot stats(String statsUrl) throws Exception {
        HttpRequest request = HttpRequest.newBuilder(URI.create(statsUrl + "/bench/stats"))
            .GET()
            .timeout(Duration.ofSeconds(10))
            .build();
        HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
        if (response.statusCode() != 200) {
            throw new IllegalStateException(
                "stats " + statsUrl + " failed: " + response.statusCode());
        }
        return ServerSnapshot.parse(response.body());
    }

    public String info(String statsUrl) {
        try {
            HttpRequest request = HttpRequest.newBuilder(URI.create(statsUrl + "/bench/info"))
                .GET()
                .timeout(Duration.ofSeconds(5))
                .build();
            return http.send(request, HttpResponse.BodyHandlers.ofString()).body();
        } catch (Exception error) {
            return "{}";
        }
    }

    /**
     * spec section 3 / FB-008 settle: no fixed sleep. Poll the server's received count
     * until it stops moving, bounded. On bound expiry the caller marks the next cell
     * that uses this same server contaminated and excludes it, rather than measuring a
     * cell that is standing behind the previous cell's backlog.
     */
    public DrainOutcome waitForServerDrain(String statsUrl, long quietMs, long boundMs)
        throws Exception {
        long startNs = BenchMetricHeader.nowNs();
        ServerSnapshot latest = null;
        long lastCount = -1;
        double lastChangeMs = 0;
        while (true) {
            double elapsedMs = (BenchMetricHeader.nowNs() - startNs) / 1e6;
            if (elapsedMs >= boundMs) {
                return new DrainOutcome(latest, elapsedMs, true);
            }
            latest = stats(statsUrl);
            long count = latest.activeMessages() + latest.errors();
            double nowMs = (BenchMetricHeader.nowNs() - startNs) / 1e6;
            if (count != lastCount) {
                lastCount = count;
                lastChangeMs = nowMs;
            } else if (nowMs - lastChangeMs >= quietMs) {
                return new DrainOutcome(latest, nowMs, false);
            }
            Thread.sleep(10);
        }
    }

    public record DrainOutcome(ServerSnapshot snapshot, double drainMs, boolean boundHit) {
    }

    public record ServerSnapshot(
        long activeMessages,
        long totalMessages,
        long errors,
        double meanMicros,
        double p95Micros,
        double p99Micros,
        double cpuSeconds,
        double workingSetMb) {

        static ServerSnapshot parse(String json) {
            return new ServerSnapshot(
                (long) field(json, "activeMessages"),
                (long) field(json, "totalMessages"),
                (long) field(json, "errors"),
                field(json, "meanMicros"),
                field(json, "p95Micros"),
                field(json, "p99Micros"),
                field(json, "cpuSeconds"),
                field(json, "workingSetMb"));
        }

        private static double field(String json, String name) {
            String key = "\"" + name + "\":";
            int start = json.indexOf(key);
            if (start < 0) {
                return 0;
            }
            start += key.length();
            int end = start;
            while (end < json.length() && "-+.eE0123456789".indexOf(json.charAt(end)) >= 0) {
                end++;
            }
            try {
                return Double.parseDouble(json.substring(start, end));
            } catch (RuntimeException error) {
                return 0;
            }
        }
    }
}
