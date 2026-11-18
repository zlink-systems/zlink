package systems.zlink.framework.testsupport;

import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;

/**
 * Runs a test fixture's {@code main} in a child JVM and returns its output.
 *
 * <p>The child writes UTF-8 with an English locale so the parent reads the same bytes on every
 * platform locale.
 */
public final class ChildJvm {
    private ChildJvm() {}

    /** Runs the fixture with TLAB and escape analysis disabled, so allocation counts are exact. */
    public static void runInAllocationJvm(Class<?> fixture) throws Exception {
        runInJvm(
                fixture,
                List.of(
                        "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                        "-XX:-UseTLAB",
                        "-XX:-DoEscapeAnalysis",
                        "-XX:ActiveProcessorCount=2",
                        "-XX:FlightRecorderOptions=stackdepth=128"));
    }

    public static String runInJvm(Class<?> fixture, List<String> jvmOptions, String... args)
            throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = fixture.getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
            }
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        var command = new ArrayList<String>();
        command.add(Path.of(System.getProperty("java.home"), "bin", "java").toString());
        command.addAll(
                List.of(
                        "--enable-native-access=ALL-UNNAMED",
                        "-Dfile.encoding=UTF-8",
                        "-Dstdout.encoding=UTF-8",
                        "-Dstderr.encoding=UTF-8",
                        "-Dsun.stdout.encoding=UTF-8",
                        "-Dsun.stderr.encoding=UTF-8",
                        "-Duser.language=en",
                        "-Duser.country=US"));
        command.addAll(jvmOptions);
        command.addAll(
                List.of(
                        "-cp",
                        String.join(java.io.File.pathSeparator, classpath),
                        fixture.getName()));
        command.addAll(List.of(args));
        Path output = Files.createTempFile("zlink-child-jvm-", ".log");
        try {
            var process =
                    new ProcessBuilder(command)
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            int exit = process.waitFor();
            String log = Files.readString(output, StandardCharsets.UTF_8);
            System.out.print(log);
            if (exit != 0)
                throw new AssertionError(
                        "child JVM exited with " + exit + System.lineSeparator() + log);
            return log;
        } finally {
            Files.deleteIfExists(output);
        }
    }
}
