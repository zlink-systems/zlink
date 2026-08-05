/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;



import java.util.Locale;

final class PerfArgs {
    private PerfArgs() {
    }

    static PerfUtil.Config parseSingleArgs(String[] args) {
        if (args.length < 3) {
            throw new IllegalArgumentException("usage: <pattern> <transport> <size>");
        }
        String pattern = args[0].toUpperCase(Locale.ROOT);
        String transport = args[1].toLowerCase(Locale.ROOT);
        int size = Integer.parseInt(args[2]);
        if (size < PerfUtil.HEADER_SIZE) {
            throw new IllegalArgumentException("msg size must be >= " + PerfUtil.HEADER_SIZE);
        }
        int duration = intEnv("PERF_SINGLE_DURATION_SECONDS",
            intEnv("PERF_DURATION_SECONDS", 5));
        int ioThreads = intEnv("PERF_IO_THREADS", 0);
        long sendHwm = unsignedLongEnv("PERF_SINGLE_SNDHWM",
            unsignedLongEnv("PERF_SNDHWM", unsignedLongEnv("PERF_SINGLE_HWM",
                unsignedLongEnv("PERF_HWM", 0L))));
        long recvHwm = unsignedLongEnv("PERF_SINGLE_RCVHWM",
            unsignedLongEnv("PERF_RCVHWM", unsignedLongEnv("PERF_SINGLE_HWM",
                unsignedLongEnv("PERF_HWM", 0L))));
        int sendBuffer = parseBufferEnv("PERF_SINGLE_SNDBUF",
            parseBufferEnv("PERF_SNDBUF", 0));
        int recvBuffer = parseBufferEnv("PERF_SINGLE_RCVBUF",
            parseBufferEnv("PERF_RCVBUF", 0));
        int sendTimeoutMs = intEnv("PERF_SINGLE_SNDTIMEO_MS",
            intEnv("PERF_SNDTIMEO_MS", 200));
        int recvTimeoutMs = intEnv("PERF_SINGLE_RCVTIMEO_MS",
            intEnv("PERF_RCVTIMEO_MS", 200));
        int monitorHwm = 1000;
        int connectReadyTimeoutMs = intEnv("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
        for (int i = 3; i < args.length; i += 2) {
            requireOptionValue(args, i);
            switch (args[i]) {
                case "--duration" -> duration = parseIntOption(args, i);
                case "--io-threads" -> ioThreads = parseIntOption(args, i);
                case "--hwm" -> {
                    long hwm = parseUnsignedLongOption(args, i);
                    sendHwm = hwm;
                    recvHwm = hwm;
                }
                case "--send-hwm" -> sendHwm = parseUnsignedLongOption(args, i);
                case "--recv-hwm" -> recvHwm = parseUnsignedLongOption(args, i);
                case "--sndbuf" -> sendBuffer = parseBufferSize(args[i + 1]);
                case "--rcvbuf" -> recvBuffer = parseBufferSize(args[i + 1]);
                case "--sndtimeo", "--send-timeout-ms" ->
                    sendTimeoutMs = parseIntOption(args, i);
                case "--rcvtimeo", "--recv-timeout-ms" ->
                    recvTimeoutMs = parseIntOption(args, i);
                case "--monitor-hwm" -> monitorHwm = parseIntOption(args, i);
                case "--connect-ready-timeout-ms" ->
                    connectReadyTimeoutMs = parseIntOption(args, i);
                default -> throw unknownOption(args[i]);
            }
        }
        return new PerfUtil.Config("single", pattern, transport, size, duration,
            "", 1, ioThreads, sendHwm, recvHwm, sendBuffer, recvBuffer,
            sendTimeoutMs, recvTimeoutMs, monitorHwm, connectReadyTimeoutMs, 0);
    }

    static PerfUtil.Config parseMultiArgs(String[] args) {
        if (args.length < 4) {
            throw new IllegalArgumentException("usage: --multi-(server|client) ...");
        }
        String pattern = args[1].toUpperCase(Locale.ROOT);
        if (pattern.startsWith("MULTI_")) {
            pattern = pattern.substring("MULTI_".length());
        }
        String transport = args[2].toLowerCase(Locale.ROOT);
        int size = Integer.parseInt(args[3]);
        if (size < PerfUtil.HEADER_SIZE) {
            throw new IllegalArgumentException("msg size must be >= " + PerfUtil.HEADER_SIZE);
        }
        boolean streamPattern = "STREAM".equals(pattern);
        int duration = intEnv("PERF_MULTI_DURATION_SECONDS",
            intEnv("PERF_DURATION_SECONDS", 5));
        String endpoint = "";
        int defaultClients = streamPattern
            ? intEnv("PERF_MULTI_DEFAULT_STREAM_CLIENTS",
                intEnv("PERF_STREAM_DEFAULT_CLIENTS", 10_000))
            : intEnv("PERF_MULTI_DEFAULT_CLIENTS",
                intEnv("PERF_DEFAULT_CLIENTS", 100));
        int clients = intEnv("PERF_MULTI_CLIENTS",
            intEnv("PERF_CLIENTS", defaultClients));
        int defaultIoThreads = intEnv("PERF_MULTI_DEFAULT_IO_THREADS",
            intEnv("PERF_DEFAULT_IO_THREADS", 4));
        int ioThreads = defaultIoThreads;
        if ("--multi-server".equals(args[0])) {
            ioThreads = intEnv(
                streamPattern ? "PERF_MULTI_STREAM_SERVER_IO_THREADS" : "PERF_MULTI_SERVER_IO_THREADS",
                streamPattern ? 4 : defaultIoThreads);
        } else if ("--multi-client".equals(args[0])) {
            ioThreads = intEnv(
                streamPattern ? "PERF_MULTI_STREAM_CLIENT_IO_THREADS" : "PERF_MULTI_CLIENT_IO_THREADS",
                streamPattern ? 4 : defaultIoThreads);
        }
        long sendHwm = unsignedLongEnv("PERF_MULTI_SNDHWM",
            unsignedLongEnv("PERF_SNDHWM", unsignedLongEnv("PERF_MULTI_HWM",
                unsignedLongEnv("PERF_HWM", 0L))));
        long recvHwm = unsignedLongEnv("PERF_MULTI_RCVHWM",
            unsignedLongEnv("PERF_RCVHWM", unsignedLongEnv("PERF_MULTI_HWM",
                unsignedLongEnv("PERF_HWM", 0L))));
        int sendBuffer = parseBufferEnv("PERF_MULTI_SNDBUF",
            parseBufferEnv("PERF_SNDBUF", 0));
        int recvBuffer = parseBufferEnv("PERF_MULTI_RCVBUF",
            parseBufferEnv("PERF_RCVBUF", 0));
        int sendTimeoutMs = intEnv("PERF_MULTI_SNDTIMEO_MS",
            intEnv("PERF_SNDTIMEO_MS", 200));
        int recvTimeoutMs = intEnv("PERF_MULTI_RCVTIMEO_MS",
            intEnv("PERF_RCVTIMEO_MS", 200));
        int monitorHwm = intEnv("PERF_MULTI_MONITOR_HWM",
            intEnv("PERF_MONITOR_HWM", 1000));
        int connectReadyTimeoutMs = intEnv("PERF_MULTI_CONNECT_READY_TIMEOUT_MS",
            intEnv("PERF_CONNECT_READY_TIMEOUT_MS", 1000));
        int connectConcurrency = intEnv("PERF_MULTI_CONNECT_CONCURRENCY",
            intEnv("PERF_CONNECT_CONCURRENCY", 0));
        boolean connectConcurrencySet = connectConcurrency > 0;
        for (int i = 4; i < args.length; i += 2) {
            requireOptionValue(args, i);
            switch (args[i]) {
                case "--duration" -> duration = parseIntOption(args, i);
                case "--endpoint" -> endpoint = args[i + 1];
                case "--clients" -> clients = parseIntOption(args, i);
                case "--io-threads" -> ioThreads = parseIntOption(args, i);
                case "--hwm" -> {
                    long hwm = parseUnsignedLongOption(args, i);
                    sendHwm = hwm;
                    recvHwm = hwm;
                }
                case "--send-hwm" -> sendHwm = parseUnsignedLongOption(args, i);
                case "--recv-hwm" -> recvHwm = parseUnsignedLongOption(args, i);
                case "--sndbuf" -> sendBuffer = parseBufferSize(args[i + 1]);
                case "--rcvbuf" -> recvBuffer = parseBufferSize(args[i + 1]);
                case "--sndtimeo", "--send-timeout-ms" ->
                    sendTimeoutMs = parseIntOption(args, i);
                case "--rcvtimeo", "--recv-timeout-ms" ->
                    recvTimeoutMs = parseIntOption(args, i);
                case "--monitor-hwm" -> monitorHwm = parseIntOption(args, i);
                case "--connect-ready-timeout-ms" ->
                    connectReadyTimeoutMs = parseIntOption(args, i);
                case "--connect-concurrency" -> {
                    connectConcurrency = parseIntOption(args, i);
                    connectConcurrencySet = true;
                }
                case "--control-port" -> parseIntOption(args, i);
                default -> throw unknownOption(args[i]);
            }
        }
        if (!connectConcurrencySet || connectConcurrency <= 0) {
            connectConcurrency = clients >= 10_000 ? 1024 : 128;
        }
        return new PerfUtil.Config("multi", pattern, transport, size, duration,
            endpoint, clients, ioThreads, sendHwm, recvHwm, sendBuffer,
            recvBuffer, sendTimeoutMs, recvTimeoutMs, monitorHwm,
            connectReadyTimeoutMs, connectConcurrency);
    }

    private static int intEnv(String name, int fallback) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return Integer.parseInt(value);
    }

    private static long unsignedLongEnv(String name, long fallback) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return Long.parseUnsignedLong(value);
    }

    private static void requireOptionValue(String[] args, int optionIndex) {
        if (optionIndex + 1 >= args.length || args[optionIndex + 1].startsWith("--")) {
            throw new IllegalArgumentException(
                args[optionIndex] + " requires a value");
        }
    }

    private static int parseIntOption(String[] args, int optionIndex) {
        try {
            return Integer.parseInt(args[optionIndex + 1]);
        } catch (NumberFormatException ex) {
            throw new IllegalArgumentException(
                args[optionIndex] + " must be an integer: "
                    + args[optionIndex + 1], ex);
        }
    }

    private static long parseUnsignedLongOption(String[] args,
                                                int optionIndex) {
        try {
            return Long.parseUnsignedLong(args[optionIndex + 1]);
        } catch (NumberFormatException ex) {
            throw new IllegalArgumentException(
                args[optionIndex] + " must be an unsigned 64-bit integer: "
                    + args[optionIndex + 1], ex);
        }
    }

    private static IllegalArgumentException unknownOption(String option) {
        return new IllegalArgumentException("unknown option: " + option);
    }

    private static int parseBufferEnv(String name, int fallback) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return parseBufferSize(value);
    }

    private static int parseBufferSize(String value) {
        String normalized = value.trim().toLowerCase(Locale.ROOT);
        int multiplier = 1;
        if (normalized.endsWith("kb")) {
            multiplier = 1024;
            normalized = normalized.substring(0, normalized.length() - 2);
        } else if (normalized.endsWith("k")) {
            multiplier = 1024;
            normalized = normalized.substring(0, normalized.length() - 1);
        } else if (normalized.endsWith("mb")) {
            multiplier = 1024 * 1024;
            normalized = normalized.substring(0, normalized.length() - 2);
        } else if (normalized.endsWith("m")) {
            multiplier = 1024 * 1024;
            normalized = normalized.substring(0, normalized.length() - 1);
        } else if (normalized.endsWith("b")) {
            normalized = normalized.substring(0, normalized.length() - 1);
        }
        return Math.toIntExact(Long.parseLong(normalized) * (long) multiplier);
    }
}
