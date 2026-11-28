/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.TreeMap;
import java.util.concurrent.Callable;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ConcurrentCloseContractTest {
    private static final int ITERATIONS = 2_000;

    @TempDir static Path directory;
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
            if (arch.equals("amd64")) {
                arch = "x86_64";
            }
            core = directory.resolve("libzlink.so.0");
            try (var resource =
                    Objects.requireNonNull(
                            Zlink.class.getResourceAsStream(
                                    "/native/linux-" + arch + "/libzlink.so"),
                            "Core native resource")) {
                Files.copy(resource, core);
            }
        }
        shim = directory.resolve("concurrent_close_shim.so");
        run(
                new ProcessBuilder(
                        "cc",
                        "-shared",
                        "-fPIC",
                        "-o",
                        shim.toString(),
                        "src/test/native/concurrent_close_shim.c",
                        "-I../../core/include",
                        "-Wl,--no-as-needed",
                        core.toString(),
                        "-Wl,-rpath," + core.getParent(),
                        "-ldl"),
                "compile");
    }

    @Test
    void contextCallsTermOnce() throws Exception {
        verify("context");
    }

    @Test
    void monitorCallsCloseOnce() throws Exception {
        verify("monitor");
    }

    @Test
    void pollerCallsDestroyOnce() throws Exception {
        verify("poller");
    }

    @Test
    void timerCallsDestroyOnce() throws Exception {
        verify("timer");
    }

    private static void verify(String resource) throws Exception {
        Path java = Path.of(System.getProperty("java.home"), "bin", "java");
        var probe =
                new ProcessBuilder(
                        java.toString(),
                        "--enable-native-access=ALL-UNNAMED",
                        "-cp",
                        System.getProperty("java.class.path"),
                        Probe.class.getName(),
                        resource);
        probe.environment().put("ZLINK_LIBRARY_PATH", shim.toString());
        run(probe, resource);
    }

    private static void run(ProcessBuilder builder, String name) throws Exception {
        Path output = directory.resolve(name + ".log");
        Process process = builder.redirectErrorStream(true).redirectOutput(output.toFile()).start();
        try {
            assertTrue(process.waitFor(30, TimeUnit.SECONDS), name + " timed out");
            String log = Files.readString(output);
            System.out.print(log);
            assertEquals(0, process.exitValue(), log);
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                process.waitFor();
            }
        }
    }

    public static final class Probe {
        public static void main(String[] args) throws Throwable {
            var lookup =
                    SymbolLookup.libraryLookup(
                            Path.of(System.getenv("ZLINK_LIBRARY_PATH")), Arena.global());
            MethodHandle takeCalls =
                    Linker.nativeLinker()
                            .downcallHandle(
                                    lookup.find("zlink_test_take_close_calls").orElseThrow(),
                                    FunctionDescriptor.of(ValueLayout.JAVA_INT));

            switch (args[0]) {
                case "context" -> race(args[0], Zlink::createContext, takeCalls);
                case "monitor" -> {
                    try (var context = Zlink.createContext();
                            var socket = context.createDealerSocket()) {
                        race(
                                args[0],
                                () -> socket.monitorOpen(MonitorEventType.CONNECTION_READY),
                                takeCalls);
                    }
                }
                case "poller" -> race(args[0], Zlink::createPoller, takeCalls);
                case "timer" -> race(args[0], Zlink::createTimer, takeCalls);
                default -> throw new IllegalArgumentException(args[0]);
            }
        }

        private static void race(
                String resource, Supplier<? extends AutoCloseable> open, MethodHandle takeCalls)
                throws Throwable {
            Map<Integer, Integer> distribution = new TreeMap<>();
            Throwable firstFailure = null;
            int exceptions = 0;
            var start = new CyclicBarrier(2);
            try (var workers = Executors.newFixedThreadPool(2)) {
                for (int i = 0; i < ITERATIONS; i++) {
                    var target = open.get();
                    Callable<Void> close =
                            () -> {
                                start.await();
                                target.close();
                                return null;
                            };
                    for (var result : List.of(workers.submit(close), workers.submit(close))) {
                        try {
                            result.get();
                        } catch (ExecutionException failure) {
                            exceptions++;
                            if (firstFailure == null) {
                                firstFailure = failure.getCause();
                            }
                        }
                    }
                    int calls = (int) takeCalls.invokeExact();
                    distribution.merge(calls, 1, Integer::sum);
                }
            }
            System.out.printf(
                    "concurrent-close %s iterations=%d calls=%s exceptions=%d%n",
                    resource, ITERATIONS, distribution, exceptions);
            assertNull(firstFailure, "close threw");
            assertEquals(Map.of(1, ITERATIONS), distribution, "native close calls per object");
        }
    }
}
