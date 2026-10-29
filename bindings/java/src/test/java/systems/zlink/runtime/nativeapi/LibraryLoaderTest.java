package systems.zlink.runtime.nativeapi;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;
import java.util.stream.Stream;

class LibraryLoaderTest {
    private static final String RESOURCE =
            "/systems/zlink/runtime/nativeapi/native-test.bin";
    private static final String FILE_NAME = "libzlink-test.so";
    private static final long PROBE_TIMEOUT_SECONDS = 30;
    private static final String OLD_CORE_LIBRARY_ENV = "ZLINK_TEST_OLD_CORE_LIBRARY";

    @TempDir
    Path temporaryDirectory;

    @Test
    void reusesOneContentAddressedCacheEntryWithoutCreatingNativeTempDirectory()
            throws Exception {
        Path cacheRoot = temporaryDirectory.resolve("cache");
        Set<Path> tempDirectoriesBefore = nativeTempDirectories();

        Path first = LibraryLoader.extractResource(RESOURCE, FILE_NAME, cacheRoot);
        Path second = LibraryLoader.extractResource(RESOURCE, FILE_NAME, cacheRoot);

        assertEquals(first, second);
        assertEquals(HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256").digest(resourceBytes())),
                first.getParent().getFileName().toString());
        try (Stream<Path> cachedFiles = Files.list(first.getParent())) {
            assertEquals(1, cachedFiles.filter(Files::isRegularFile).count());
        }
        assertEquals(tempDirectoriesBefore, nativeTempDirectories());
    }

    @Test
    void replacesCacheEntryWhenItsSizeDoesNotMatchTheResource() throws IOException {
        Path cacheRoot = temporaryDirectory.resolve("cache");
        Path extracted = LibraryLoader.extractResource(RESOURCE, FILE_NAME, cacheRoot);
        Files.write(extracted, new byte[] { 1 });

        Path repaired = LibraryLoader.extractResource(RESOURCE, FILE_NAME, cacheRoot);

        assertEquals(extracted, repaired);
        assertArrayEquals(resourceBytes(), Files.readAllBytes(repaired));
    }

    @Test
    void loadsThroughTempFallbackWhenConfiguredCacheCannotBeCreated() throws Exception {
        Path unusableCache = temporaryDirectory.resolve("cache-is-a-file");
        Files.writeString(unusableCache, "not a directory");
        Path java = Path.of(System.getProperty("java.home"), "bin", "java");
        ProcessBuilder processBuilder = new ProcessBuilder(
                java.toString(),
                "--enable-native-access=ALL-UNNAMED",
                "-cp",
                System.getProperty("java.class.path"),
                LibraryLoaderFallbackProbe.class.getName());
        processBuilder.environment().put("ZLINK_JAVA_NATIVE_CACHE",
                unusableCache.toString());
        processBuilder.environment().remove("ZLINK_LIBRARY_PATH");
        processBuilder.redirectErrorStream(true);

        Process process = processBuilder.start();
        if (!process.waitFor(PROBE_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail("fallback probe did not terminate");
        }
        String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8);
        assertEquals(0, process.exitValue(), output);
    }

    @Test
    void compatibleCoreCreatesContextBeforeAnySnapshot() throws Exception {
        ProcessResult result = runContextProbe(null);
        assertEquals(0, result.exitCode(), result.output());
        assertTrue(result.output().contains("CONTEXT_CREATED"), result.output());
    }

    @Test
    @EnabledIfEnvironmentVariable(named = OLD_CORE_LIBRARY_ENV, matches = ".+")
    void coreWithoutSnapshotExportIsRejectedBeforeContextCreation() throws Exception {
        ProcessResult result = runContextProbe(System.getenv(OLD_CORE_LIBRARY_ENV));
        assertTrue(result.exitCode() != 0, result.output());
        assertTrue(
                result.output()
                        .contains(
                                "Missing native symbol 'zlink_socket_connection_attempts_snapshot'"),
                result.output());
        assertFalse(result.output().contains("CONTEXT_CREATED"), result.output());
    }

    private ProcessResult runContextProbe(String library) throws Exception {
        Path java = Path.of(System.getProperty("java.home"), "bin", "java");
        ProcessBuilder builder =
                new ProcessBuilder(
                        java.toString(),
                        "--enable-native-access=ALL-UNNAMED",
                        "-cp",
                        System.getProperty("java.class.path"),
                        ConnectionContextProbe.class.getName());
        if (library != null) builder.environment().put("ZLINK_LIBRARY_PATH", library);
        builder.redirectErrorStream(true);
        Process process = builder.start();
        if (!process.waitFor(PROBE_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail("context probe did not terminate");
        }
        return new ProcessResult(
                process.exitValue(),
                new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));
    }

    private record ProcessResult(int exitCode, String output) {}

    private byte[] resourceBytes() throws IOException {
        try (var in = LibraryLoaderTest.class.getResourceAsStream(RESOURCE)) {
            if (in == null)
                throw new IOException("missing test resource: " + RESOURCE);
            return in.readAllBytes();
        }
    }

    private Set<Path> nativeTempDirectories() throws IOException {
        Path systemTemp = Path.of(System.getProperty("java.io.tmpdir"));
        try (Stream<Path> entries = Files.list(systemTemp)) {
            return entries.filter(Files::isDirectory)
                    .filter(path -> path.getFileName().toString()
                            .startsWith("zlink-native-"))
                    .collect(Collectors.toSet());
        }
    }
}
