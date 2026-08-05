/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;



import java.util.Locale;

final class PerfReport {
    private PerfReport() {
    }

    static String format(PerfUtil.Result result, String libTag) {
        if ("unsupported".equals(result.status)) {
            return String.format(Locale.ROOT, "UNSUPPORTED,%s,%s,%s",
                libTag, result.pattern, result.transport);
        }
        if ("fail".equals(result.status)) {
            return String.format(Locale.ROOT, "FAIL,%s,%s,%s,%d,%s",
                libTag, result.pattern, result.transport, result.size,
                result.reason);
        }
        if (!"ok".equals(result.status)) {
            return "";
        }
        String key = String.format(Locale.ROOT, "RESULT,%s,%s,%s,%d",
            libTag, result.pattern, result.transport, result.size);
        return String.join(System.lineSeparator(),
            metricLine(key, "throughput", result.throughput),
            metricLine(key, "bandwidth", result.bandwidth),
            metricLine(key, "latency", result.latencyMean),
            metricLine(key, "latency_p95", result.latencyP95),
            metricLine(key, "latency_p99", result.latencyP99));
    }

    private static String metricLine(String key, String metric, double value) {
        return String.format(Locale.ROOT, "%s,%s,%s", key, metric, metric(value));
    }

    private static String metric(double value) {
        return Double.isNaN(value) ? "N/A" : String.format(Locale.ROOT, "%.3f", value);
    }
}
