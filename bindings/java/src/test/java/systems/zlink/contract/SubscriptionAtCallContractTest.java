/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.TimeUnit;
import java.util.function.IntFunction;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.messaging.SubscriptionEntry;

public class SubscriptionAtCallContractTest {
    @TempDir
    static Path directory;
    private static Path shim;

    @BeforeAll
    static void compileShim() throws Exception {
        assumeTrue(System.getProperty("os.name").equals("Linux"));
        String libraryPath = System.getenv("ZLINK_LIBRARY_PATH");
        Path core;
        if (libraryPath != null && !libraryPath.isEmpty()) {
            core = Path.of(libraryPath).toAbsolutePath();
        } else {
            String arch = System.getProperty("os.arch");
            if (arch.equals("amd64"))
                arch = "x86_64";
            core = directory.resolve("libzlink.so.0");
            try (var resource = Zlink.class.getResourceAsStream(
                    "/native/linux-" + arch + "/libzlink.so")) {
                Files.copy(resource, core);
            }
        }
        shim = directory.resolve("subscription_at_shim.so");
        run(new ProcessBuilder("cc", "-shared", "-fPIC", "-o", shim.toString(),
            "src/test/native/subscription_at_shim.c", "-I../../core/include",
            "-Wl,--no-as-needed", core.toString(),
            "-Wl,-rpath," + core.getParent(), "-ldl"), "compile");
    }

    @ParameterizedTest(name = "{0}")
    @CsvSource({
        "missing, false, 1, NOT_FOUND,",
        "empty, true, 0, OK,",
        "long, false, 0, BUFFER_TOO_SMALL, OK",
        "removed-before-retry, true, 0, BUFFER_TOO_SMALL, NOT_FOUND",
        "invalid-argument, false, 0, INVALID_ARGUMENT,",
        "buffer-still-too-small, true, 0, BUFFER_TOO_SMALL, BUFFER_TOO_SMALL"
    })
    void callsCoreOnlyForRequestedIndex(String scenario, boolean xsub, int index,
                                       ConfigResult first, ConfigResult retry) throws Exception {
        Path trace = directory.resolve(scenario + ".trace");
        ConfigResult result = retry == null ? first : retry;
        var probe = new ProcessBuilder(Path.of(System.getProperty("java.home"), "bin", "java").toString(),
            "--enable-native-access=ALL-UNNAMED", "-cp", System.getProperty("java.class.path"),
            Probe.class.getName(), scenario, Boolean.toString(xsub), Integer.toString(index),
            result.name());
        probe.environment().put("ZLINK_LIBRARY_PATH", shim.toString());
        probe.environment().put("ZLINK_JAVA_SUBSCRIPTION_CASE", scenario);
        probe.environment().put("ZLINK_JAVA_SUBSCRIPTION_TRACE", trace.toString());
        run(probe, scenario);

        List<List<Long>> calls = Files.readAllLines(trace).stream()
            .map(line -> Arrays.stream(line.split(" ")).map(Long::parseLong).toList())
            .toList();
        assertEquals(retry == null ? 1 : 2, calls.size(), calls.toString());
        assertTrue(calls.getFirst().get(1) > 0, calls.toString());
        assertEquals(List.of((long) index, calls.getFirst().get(1), (long) first.value()),
            calls.getFirst());
        if (retry != null)
            assertEquals(List.of((long) index, 4096L, (long) retry.value()), calls.get(1));
    }

    private static void run(ProcessBuilder builder, String name) throws Exception {
        Path output = directory.resolve(name + ".log");
        Process process = builder.redirectErrorStream(true).redirectOutput(output.toFile()).start();
        try {
            assertTrue(process.waitFor(30, TimeUnit.SECONDS), name + " timed out");
            assertEquals(0, process.exitValue(), Files.readString(output));
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }

    public static final class Probe {
        public static void main(String[] args) {
            String scenario = args[0];
            int index = Integer.parseInt(args[2]);
            ConfigResult result = ConfigResult.valueOf(args[3]);
            String filter = switch (scenario) {
                case "long", "removed-before-retry", "buffer-still-too-small" -> "x".repeat(4096);
                default -> "";
            };
            try (var context = Zlink.createContext()) {
                if (Boolean.parseBoolean(args[1])) {
                    try (var socket = context.createXSubSocket()) {
                        socket.setSubscription(filter);
                        verify(socket::subscriptionAt, index, result, filter);
                    }
                } else {
                    try (var socket = context.createSubSocket()) {
                        socket.setSubscription(filter);
                        verify(socket::subscriptionAt, index, result, filter);
                    }
                }
            }
        }

        private static void verify(IntFunction<Optional<SubscriptionEntry>> query, int index,
                                   ConfigResult result, String filter) {
            switch (result) {
                case OK -> assertEquals(Optional.of(new SubscriptionEntry(filter, false)),
                    query.apply(index));
                case NOT_FOUND -> assertEquals(Optional.empty(), query.apply(index));
                default -> {
                    var error = assertThrows(ZlinkConfigException.class, () -> query.apply(index));
                    assertEquals(result, error.getResult());
                }
            }
        }
    }
}
