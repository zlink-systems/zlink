/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;



import java.util.Locale;

final class PerfPolicy {
    private PerfPolicy() {
    }

    static PerfUtil.Result classifyFailure(PerfUtil.Config config, Throwable failure) {
        String unsupportedReason = "multi".equals(config.suite())
            ? unsupportedReason(failure)
            : null;
        if (unsupportedReason != null) {
            return PerfUtil.Result.unsupported(unsupportedReason, config);
        }
        return new PerfUtil.Result("fail", sanitizeReason(failure), config.pattern(),
            config.transport(), config.size(), 0.0d, 0.0d, 0.0d, 0.0d, 0.0d);
    }

    private static String unsupportedReason(Throwable failure) {
        for (Throwable current = failure; current != null; current = current.getCause()) {
            if (protocolNotSupported(current.getMessage())) {
                return "protocol_not_supported";
            }
        }
        return null;
    }

    private static String sanitizeReason(Throwable failure) {
        Throwable selected = failure;
        String message = null;
        for (Throwable current = failure; current != null; current = current.getCause()) {
            if (current.getMessage() != null && !current.getMessage().isBlank()) {
                selected = current;
                message = current.getMessage();
            }
        }
        if (message == null || message.isBlank()) {
            return selected == null ? "unknown_error"
                : selected.getClass().getSimpleName().toLowerCase(Locale.ROOT);
        }
        return message.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "_")
            .replaceAll("^_+|_+$", "");
    }

    private static boolean protocolNotSupported(String message) {
        return message != null
            && message.toLowerCase(Locale.ROOT).contains("protocol not supported");
    }
}
