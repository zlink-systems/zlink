/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.List;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/** Exact source-A cell configuration supplied by the runner. */
public final class BenchOptions {
    public final String scenario;
    public final String implementation;
    public final List<Integer> payloadSizes;
    public final int requestWindow;
    public final int sendConcurrency;
    public final int latencySampleLimit;
    public final double warmupSeconds;
    public final double warmupSegmentSeconds;
    public final int warmup;
    public final double durationSeconds;
    public final int commandSettleMs;
    public final int drainBoundMs;
    public final int windowSettleMs;
    public final int requestTimeoutMs;
    public final int routeReadyMs;
    public final String triggerUrl;
    public final String statsUrl;
    public final String targetEndpoint;
    public final String targetCommandEndpoint;
    public final String targetStatsUrl;
    public final String grpcUrl;
    public final String grpcStatsUrl;
    public final String zlinkEndpoint;
    public final String zlinkStatsUrl;
    public final String zlinkRawEndpoint;
    public final String zlinkRawStatsUrl;
    public final String zlinkRawCommandEndpoint;
    public final String rawSocket;
    public final String runIdText;
    public final String cellId;
    public final int runId;
    public final String output;
    public final String reportFile;

    public BenchOptions(String[] argv) {
        scenario = Args.value(argv, "--scenario", "");
        implementation = Args.value(argv, "--implementation", "");
        int payload = Args.integer(argv, "--payload-size", 1024);
        payloadSizes = List.of(payload);
        requestWindow = Args.integer(argv, "--request-window", 100);
        sendConcurrency = Args.integer(argv, "--send-concurrency", 8);
        latencySampleLimit = Args.integer(argv, "--latency-sample-limit", 200_000);
        warmupSeconds = Args.number(argv, "--warmup-seconds", 20.0);
        warmupSegmentSeconds = Args.number(argv, "--warmup-segment-seconds", 2.0);
        warmup = Args.integer(argv, "--warmup", 0);
        durationSeconds = Args.number(argv, "--duration-seconds", 5.0);
        commandSettleMs = Args.integer(argv, "--command-settle-ms", 200);
        drainBoundMs = Args.integer(argv, "--drain-bound-ms", 30_000);
        windowSettleMs = drainBoundMs;
        requestTimeoutMs = Args.integer(argv, "--request-timeout-ms", 30_000);
        routeReadyMs = Args.integer(argv, "--route-ready-ms", 30_000);
        triggerUrl = Args.value(argv, "--trigger-url", "");
        statsUrl = Args.value(argv, "--stats-url", "");
        targetEndpoint = Args.value(argv, "--target-endpoint", "");
        targetCommandEndpoint = Args.value(argv, "--target-command-endpoint", "");
        targetStatsUrl = Args.value(argv, "--target-stats-url", "");
        grpcUrl = targetEndpoint;
        grpcStatsUrl = targetStatsUrl;
        zlinkEndpoint = targetEndpoint;
        zlinkStatsUrl = targetStatsUrl;
        zlinkRawEndpoint = targetEndpoint;
        zlinkRawStatsUrl = targetStatsUrl;
        zlinkRawCommandEndpoint = targetCommandEndpoint;
        rawSocket = Args.value(argv, "--raw-socket", "router");
        runIdText = Args.value(argv, "--run-id", "");
        cellId = Args.value(argv, "--cell-id", "");
        runId = headerRunId(runIdText);
        output = Args.value(argv, "--output", "log/latest");
        reportFile = Args.value(argv, "--report-file", "report.txt");

        if (!List.of("request-serial", "request-window", "request-backpressure",
            "send-saturation").contains(scenario)) {
            throw new IllegalArgumentException("unknown scenario: " + scenario);
        }
        if (payload < BenchMetricHeader.HEADER_SIZE || requestWindow != 100
            || sendConcurrency != 8 || latencySampleLimit <= 0 || drainBoundMs != 30_000
            || requestTimeoutMs != 30_000 || routeReadyMs != 30_000) {
            throw new IllegalArgumentException("fixed benchmark options are invalid");
        }
        if (!"router".equals(rawSocket)) {
            throw new IllegalArgumentException("RAW_SOCKET must remain router");
        }
        if (runIdText.isBlank() || cellId.isBlank()) {
            throw new IllegalArgumentException("run-id and cell-id are required");
        }
        requireLoopback(triggerUrl, "http://", "trigger URL");
        requireLoopback(statsUrl, "http://", "stats URL");
        requireLoopback(targetStatsUrl, "http://", "target stats URL");
    }

    private static void requireLoopback(String value, String scheme, String label) {
        if (!value.startsWith(scheme + "127.0.0.1:")) {
            throw new IllegalArgumentException(label + " must use IPv4 loopback: " + value);
        }
    }

    public boolean runsImplementation(String name) {
        return implementation.equals(name);
    }

    public boolean runsPattern(String name) {
        return scenario.equals(name);
    }

    private static int headerRunId(String text) {
        long hash = 2_166_136_261L;
        for (byte value : text.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
            hash = ((hash ^ (value & 0xffL)) * 16_777_619L) & 0xffff_ffffL;
        }
        int result = (int) hash;
        return result == 0 ? 1 : result;
    }
}
