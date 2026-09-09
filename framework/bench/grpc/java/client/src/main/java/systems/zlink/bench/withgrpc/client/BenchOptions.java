/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ThreadLocalRandom;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/** The common CLI of plan section 4, with the java-specific warmup control added. */
public final class BenchOptions {
    public final String scenario;
    public final String implementation;
    public final List<Integer> payloadSizes = new ArrayList<>();
    public final int requestWindow;
    public final int sendConcurrency;
    public final int latencySampleLimit;
    /**
     * spec section 8.2: warmup length is set per language and recorded. The java row warms
     * up for a duration rather than for an iteration count, and it warms up through
     * the SAME driver that the measured window uses, so the JIT sees the shape it
     * will be measured on. The per-segment throughput of this window is reported
     * with every cell as the evidence that the runtime had reached steady state.
     */
    public final double warmupSeconds;
    public final double warmupSegmentSeconds;
    public final int warmup;
    public final double durationSeconds;
    public final int commandSettleMs;
    public final int drainBoundMs;
    public final int windowSettleMs;
    public final int requestTimeoutMs;
    public final int routeReadyMs;
    public final String grpcUrl;
    public final String grpcStatsUrl;
    public final String zlinkEndpoint;
    public final String zlinkStatsUrl;
    public final String zlinkRawEndpoint;
    public final String zlinkRawStatsUrl;
    public final String zlinkRawCommandEndpoint;
    public final String rawSocket;
    public final int runId;
    public final String output;
    public final String reportFile;

    public BenchOptions(String[] argv) {
        scenario = Args.value(argv, "--scenario", "all");
        implementation = Args.value(argv, "--implementation", "all");
        for (String piece : Args.value(argv, "--payload-sizes", "1024,4096").split(",")) {
            int size = Integer.parseInt(piece.trim());
            if (size < BenchMetricHeader.HEADER_SIZE) {
                throw new IllegalArgumentException(
                    "payload size must be at least " + BenchMetricHeader.HEADER_SIZE);
            }
            payloadSizes.add(size);
        }
        requestWindow = Args.integer(argv, "--request-window", 100);
        sendConcurrency = Args.integer(argv, "--send-concurrency", 8);
        latencySampleLimit = Args.integer(argv, "--latency-sample-limit", 200000);
        warmupSeconds = Args.number(argv, "--warmup-seconds", 20.0);
        warmupSegmentSeconds = Args.number(argv, "--warmup-segment-seconds", 2.0);
        warmup = Args.integer(argv, "--warmup", 0);
        durationSeconds = Args.number(argv, "--duration-seconds", 5.0);
        commandSettleMs = Args.integer(argv, "--command-settle-ms", 200);
        drainBoundMs = Args.integer(argv, "--drain-bound-ms", 30000);
        windowSettleMs = Args.integer(argv, "--window-settle-ms", 5000);
        requestTimeoutMs = Args.integer(argv, "--timeout-seconds", 30) * 1000;
        routeReadyMs = Args.integer(argv, "--route-ready-ms", 15000);
        grpcUrl = Args.value(argv, "--grpc-url", "127.0.0.1:5091");
        grpcStatsUrl = Args.value(argv, "--grpc-stats-url", "http://127.0.0.1:5094");
        zlinkEndpoint = Args.value(argv, "--zlink-endpoint", "tcp://127.0.0.1:5092");
        zlinkStatsUrl = Args.value(argv, "--zlink-stats-url", "http://127.0.0.1:5093");
        zlinkRawEndpoint = Args.value(argv, "--zlink-raw-endpoint", "tcp://127.0.0.1:5095");
        zlinkRawStatsUrl = Args.value(argv, "--zlink-raw-stats-url", "http://127.0.0.1:5096");
        zlinkRawCommandEndpoint =
            Args.value(argv, "--zlink-raw-command-endpoint", "tcp://127.0.0.1:5097");
        rawSocket = Args.value(argv, "--raw-socket",
            System.getenv("RAW_SOCKET") == null ? "router" : System.getenv("RAW_SOCKET"));
        runId = ThreadLocalRandom.current().nextInt(1, Integer.MAX_VALUE);
        output = Args.value(argv, "--output", "log/latest");
        reportFile = Args.value(argv, "--report-file", "with_grpc_java.txt");

        if (!"router".equals(rawSocket) && !"dealer".equals(rawSocket)) {
            throw new IllegalArgumentException("raw socket must be router or dealer");
        }
        for (String url : List.of(grpcStatsUrl, zlinkStatsUrl, zlinkRawStatsUrl)) {
            if (!url.startsWith("http://127.0.0.1:")) {
                throw new IllegalArgumentException("stats url must be loopback: " + url);
            }
        }
        for (String endpoint
            : List.of(zlinkEndpoint, zlinkRawEndpoint, zlinkRawCommandEndpoint)) {
            if (!endpoint.startsWith("tcp://127.0.0.1:")) {
                throw new IllegalArgumentException("endpoint must be loopback: " + endpoint);
            }
        }
    }

    public boolean runsImplementation(String name) {
        return "all".equals(implementation) || implementation.equals(name);
    }

    public boolean runsPattern(String name) {
        return "all".equals(scenario) || scenario.equals(name);
    }
}
